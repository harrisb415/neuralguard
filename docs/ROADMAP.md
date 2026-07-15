# NeuralGuard — Roadmap

Solo build plan. Phases are ordered so that **each one produces something usable on
its own** and de-risks the next. Durations are effort-shaped, not calendar
deadlines; several phases are gated by "let it run and collect data," which is wall-
clock time, not work.

The guiding rule: **you have a working, safe tool at the end of Phase 2.**
Everything after that makes it smarter, not functional-for-the-first-time.

---

## Phase 0 — WFP telemetry spike  ✅ DONE

**Goal:** prove the whole foundation in ~200 lines. No enforcement, no ML, no DB.

- Open a WFP engine session (`FwpmEngineOpen0`), subscribe to net events
  (`FwpmNetEventSubscribe4`), and print each allow/drop with the 5-tuple, image
  path (from `appId`), and user SID. Implemented in [`src/ngmon/main.cpp`](../src/ngmon/main.cpp).

**Result:** working — `ngmon` streams live connections attributed to processes on
the test VM (built on host via VS 2026, run on the VM over SSH).

**Two gotchas found and documented in the code:**
1. Events need engine-side enablement — `FwpmEngineSetOption0(FWPM_ENGINE_COLLECT_NET_EVENTS)`
   plus the `CLASSIFY_ALLOW` keyword — or you get *zero* events, not just missing allows.
2. Those options can't be set from a `FWPM_SESSION_FLAG_DYNAMIC` session
   (`FWP_E_DYNAMIC_SESSION_IN_PROGRESS`), so `ngmon` uses a normal session.
   Also enable the *Filtering Platform Connection* audit subcategory
   (`auditpol /set /subcategory:"Filtering Platform Connection" /success:enable /failure:enable`).

## Phase 1 — Learning mode (passive)

**Goal:** record a real baseline. Still enforces nothing.

- Stand up `ngd` as a proper Windows service and `ngctl status`.
- Add the SQLite store (`ngpolicy.db`) and schema for flow events + habit tables.
- Build the live `PID → identity` table and the DNS-ETW correlation (IP → domain).
- Compute and persist the **identity key** (§4 of DESIGN) per connection; accumulate
  habit counts with decay.
- `ngctl log` / `ngctl rules` to inspect what's being learned.

**Progress:**
- ✅ Recorder — `ngd record` persists every WFP net event to `ngpolicy.db`
  (`flow_events`); `ngd dump` reads them back. SQLite vendored (amalgamation,
  no package manager); WFP helpers shared with `ngmon` via `src/common/wfp_util.h`.
- ✅ Process identity — `\device\harddiskvolumeN\...` normalized to `C:\...`,
  image SHA-256, and Authenticode signer (embedded **and catalog**, so in-box
  Windows binaries resolve to their Microsoft signer). De-duplicated in a
  `process_identity` table and cached in memory.
- ✅ Refactor — split the growing `ngd/main.cpp` into an `ngcore` static lib
  (`db`, `identity`, `signer`, `util`) + `ngd` (`recorder`, `main`), so later
  binaries (ngctl, service, tray backend) reuse the core.
- ✅ DNS correlation — a real-time ETW consumer of `Microsoft-Windows-DNS-Client`
  (event 3008) maps resolved IP → domain, so each flow records the name the app
  actually asked for (`flow_events.remote_domain`), not just an IP. (Note: DNS
  answers arrive as v4-mapped IPv6 and must be unwrapped to match the flow's IPv4.)
- ✅ Identity key + decaying habit table (`ng::HabitTracker`, `habits` table).
  One row per `(process key, destination, port, protocol)` with an exponentially
  decaying count (14-day half-life) + hour-of-day / day-of-week histograms.
  Process key is `sig:<thumbprint>` when signed (survives updates), else
  `sha:<hash>`. Observations deduped by 5-tuple (one connection = one obs).
  *Known limitation (Phase 3):* first contact to a CDN name keys on the rotating
  IP because the DNS ETW event lags the WFP event; later flows self-heal to the
  domain. ASN grouping / domain-backfill is a later refinement.

**Implementation complete.** Remaining gate is a *usage* step, not code: run `ngd`
passively on the physical machine for a few weeks of normal use to accumulate a
real baseline before enabling any enforcement (Phase 2).
It's safe — it's read-only. Stop when a normal day is ~95% covered by learned keys
(watch the "novel connections per day" curve flatten).

## Phase 2 — Enforcement + prompts  ← *the day it replaces your built-in firewall*

**Goal:** actually block, without locking yourself out.

- Create NeuralGuard's own **sublayer** (weight above Defender's range).
- **Build the panic switch first** (`ngctl panic` → delete all filters in our
  sublayer). Do not add a single blocking filter before this exists.
- Hard-code the always-exempt Tier 0 rules (loopback/DHCP/DNS/NTP).
- Auto-generate permit filters (`FwpmFilterAdd0`) for the learned baseline; install a
  default-block for the rest.
- Ship a **minimal `ngtray`**: tray icon showing mode (Learning / Enforcing / Panic),
  **actionable toast prompts** (Allow once / Always / Block) for blocked novel
  connections, and **Panic** in the right-click menu. `ngctl` stays as the headless
  fallback.
- Implement block-notify-retry behind those prompts: *Allow* writes a permit rule and
  the app's retry succeeds.
- Watchdog + fail-open-on-death.

**Progress:**
- ✅ Enforcement primitive + **panic switch** (`ng::Enforcer`, `ngctl`). Creates
  NeuralGuard's provider + a weight-`0xFFFF` sublayer (above Defender), adds
  permit/block filters at the ALE connect layer, and `ngctl panic` enumerates and
  deletes every NeuralGuard filter (then drops the sublayer) to fail open. Verified
  on the VM: `block 1.1.1.1 443` refuses the connection, `panic` restores it; SSH
  unaffected. No default-block yet — only explicit rules.
- ✅ Tier-0 always-exempt + default-deny (outbound IPv4) — `Enforcer::enableDefaultDeny()`
  permits loopback / RFC1918 / link-local / DNS / DHCP / NTP (weight 15) above a
  catch-all block (weight 0); inbound untouched so SSH can't be cut off. `ngctl
  enforce <seconds>` runs it with a mandatory auto-revert (dead-man switch). Verified:
  public outbound blocked while DNS + SSH keep working; clean revert.
- ✅ Auto-permit the observed baseline — `Enforcer::addPermitAppId()` (ALE_APP_ID)
  + `ngctl enforce-baseline <db> <seconds>`: permit every distinct observed
  `(application, remote port)` from the DB, then default-deny the rest (auto-revert).
  Verified against the live baseline (84 permits): known app+port flows, unobserved
  is blocked, DNS/SSH keep working. Per-(app,port) granularity is the safe first cut.
- ✅ **Full coverage — both directions, both IP versions (see "Full-coverage
  enforcement" section below).** Phases A–D done: direction is now read from the
  WFP layer (no port heuristic), outbound enforces v4+v6, inbound enforces v4+v6
  (opt-in via `meta('inbound_mode')`, anti-lockout Tier-0 structural), and ICMP was
  measured to be already covered at ALE (no transport filters needed — see D-7).
  Every connection-attributable gap is closed; below-ALE raw sockets / per-packet
  inspection remain Phase 5 by design.
- ⬜ Per-destination/domain tightening (still open, separate from the above).
- ◐ `ngtray` — native Win32 tray (icon + Status / **Panic** / Quit + startup
  balloon) **and the notify-pipe + prompt**: tray runs a `\\.\pipe\neuralguard`
  server; `ngctl notify <app> <dest> <port>` prompts the user (balloon + Allow
  always/once/Block dialog) and enacts the answer (a permit on Allow). Pipe
  verified reachable; click-through is interactive. Remaining: true inline-button
  toasts (COM activator) + a live mode indicator.
- ✅ **Automatic block-notify-retry** — `ngd enforce [db] [seconds]` is the live
  enforcer: installs stable baseline permits + default-deny, subscribes to WFP net
  events, and on a novel public drop (deduped) prompts the tray off the callback
  thread; Allow → permit (retry succeeds), else stays blocked; auto-reverts on
  stop. Verified headless (baseline permits / novel blocked / drop-detected /
  reverted); live toast click-through is interactive.
- ✅ **Config UI (native Win32)** — WebView2 removed (user chose a native C++ app).
  `ngtray` → Dashboard is a native window (Common Controls): tab control +
  ListViews + status-bar **live mode indicator** (ngd publishes `meta.mode`).
  Double-click the tray icon opens it; the tray runs elevated
  (requireAdministrator) so its actions don't each fire a UAC prompt. All
  requested panels/capabilities shipped:
  - ✅ Live connection feed · **editable rules** (right-click add/delete, applied
    live) · Per-app view · Decision-history tabs
  - ✅ Settings/**autonomy** tab (prompt / auto-allow-known / auto-allow-all) ·
    export-import · block rules · search/filter · timed-allow
  - ✅ **Windows service + watchdog + fail-open-on-death**
- ✅ **Windows service (`ngd install/uninstall/service-run`)** — runs enforcement
  as LocalSystem at boot (zero UAC), SCM restart-on-crash watchdog, and a DYNAMIC
  WFP session so a crashed enforcer leaves no filters (fail-open-on-death).
  Editable rules apply live via a `rules` table + `meta.rules_gen` poll; timed
  allows expire via `expires_epoch`. Dashboard Settings tab has Install/Remove
  buttons. Verified end-to-end on the VM.

**Gate to next phase:** a week of daily use on the physical box with near-zero false
blocks and a manageable prompt rate.

**Progress:**
- ✅ `ngd digest` — a "what's new" report over the baseline (totals, top talkers,
  new-in-7-days, rare/one-off novel ones, chattiest apps). Also fixed a habit
  noise bug it exposed: inbound peers' ephemeral remote ports created junk
  one-off habits; habits now only record remote ports < 49152 (outbound service
  ports).
- ✅ Nightly compaction — `ngd compact` decays every habit's count to now and
  evicts faded ones (< 0.1). SQLite math functions enabled; DB `busy_timeout` set.
- ✅ Novelty score — `ngd novelty` ranks habits by `0.6*rarity + 0.4*newness`
  (rare + recently first seen). Also excluded loopback dests from habits.
  The auto-allow-low-novelty *action* still needs the live decision path (2c tray).
- ✅ Promotion — `ngd promote` classifies (app, port) as stable (≥ N distinct
  connections) vs provisional; `enforce-baseline` now permits only stable pairs
  (default ≥ 3), so one-offs aren't silently trusted (84 → 16 permits on the live
  baseline). Provisional pairs fall to default-deny → a prompt once the tray lands.
- ✅ Configurable autonomy levels — prompt-everything / auto-allow-known /
  auto-allow-all, set from the dashboard's Settings tab and read live by the
  enforce daemon per drop.
- ⬜ Weekly digest delivery (`ngd digest` exists as an on-demand report; no
  scheduled delivery mechanism yet).

## Full-coverage enforcement — both directions, both IP versions

**Goal:** zero coverage gaps. Today enforcement is **outbound-IPv4-only**, and the
learning path infers direction from a remote-port heuristic (`remotePort < 49152`
in `recorder.cpp`/`enforce.cpp`) that has real edge cases (an outbound flow to a
high remote port is wrongly excluded; an inbound flow from a low source port is
wrongly included). A firewall that lets traffic slip by — or wrongly blocks it —
because it *guessed* the direction isn't doing its job.

**The insight that makes this clean:** direction is not something to infer — WFP
assigns it by **layer**. Each ALE connection layer *is* a (direction, IP version):
`ALE_AUTH_CONNECT_V4/V6` = outbound, `ALE_AUTH_RECV_ACCEPT_V4/V6` = inbound. So the
fix is to (a) enforce at all four layers, and (b) read the layer/direction the OS
already stamps on every net event (`layerId` in the classify union member;
`msFwpDirection` on drops) instead of guessing from ports. The `< 49152` heuristic
gets **deleted**, not patched.

**Critical safety fact:** inbound default-deny at `RECV_ACCEPT` does NOT break
outbound connections' return traffic — `RECV_ACCEPT` only fires on *new* inbound
accepts, never on the reply of a flow you initiated outbound (or a DNS response to
your query). So "filter inbound" affects your *listening services* (SSH, RDP, file
sharing), not your browsing.

**Honest scope of "zero gaps":** complete for all connection-attributable traffic
in user-mode WFP — TCP connects/accepts + UDP flows, both directions, both
versions, at the ALE layers, plus the transport layers for ICMP/ICMPv6. Truly
below-ALE raw sockets and per-packet inspection remain Phase 5 (kernel callout)
territory — not claimed here without the driver.

### Phase A — Learning correctness (read-only, zero risk). ← IN PROGRESS
- Read `layerId` from the classify event's typed union member → derive true
  (direction, version); cross-check drops via `msFwpDirection`. Add a
  `layerId → {direction, version}` helper in `wfp_util.h`.
- **Delete the `remotePort < 49152` heuristic** in `recorder.cpp` + `enforce.cpp`.
- Direction-aware habits: outbound keys on the **remote** service port, inbound on
  **your local** service port. Add a `direction` column to `habits` (additive
  migration; existing rows backfill to `out`). Full IPv6 addresses stored/compared.
- Payoff: baseline starts learning inbound + IPv6 correctly, with a real signal,
  before any enforcement change — accumulates the inbound baseline Phase C needs
  while risking nothing. Immediately fixes the high-port bug.

### Phase B — IPv6 outbound enforcement. ✅ DONE
- `Enforcer::addV6` mirrors `addV4` at `ALE_AUTH_CONNECT_V6`; `addPermitCidrV6`
  uses `FWP_V6_ADDR_AND_MASK` (addr + prefix length). `enableDefaultDeny` now
  installs v6 Tier-0 exempts (`::1`, `fe80::/10`, `fc00::/7`, `ff00::/8` multicast
  — ND/MLD die without it — + DNS / DHCPv6 547 / NTP) and a v6 catch-all block.
- `addPermitAppId` + app-scoped `applyUserRule` install at BOTH layers, so a
  baseline/user-permitted app works over IPv6 (else v6 default-deny would wrongly
  block it); v4-address-pinned rules stay v4-only. `panic`/`countRules` already
  enumerate `CONNECT_V6`, so cleanup covers v6 automatically.
- VM-verified: filter count 0 → **19** during enforce (10 v4 + 9 v6) → 0 after;
  v4 still blocks/reverts correctly; `::1` + SSH stay alive; clean timed revert.
  The real global-v6 drop couldn't be exercised (NAT VM has no IPv6 internet
  route — `curl -6` fails identically enforced or not), but the v6 catch-all is a
  structural mirror of the proven v4 one at the parallel layer.

### Phase C — Inbound enforcement (most gated). ◐ PRIMITIVE DONE
- ✅ **Inbound enforcement primitive + anti-lockout Tier-0.** `Enforcer::
  enableInboundDefaultDeny` installs, at `RECV_ACCEPT_V4/V6`, the anti-lockout
  exempts FIRST (weight 15) — **SSH 22 / RDP 3389** local ports, DHCP 68 / DHCPv6
  546, loopback, link-local — then a catch-all block (weight 0). `addPermitAppId
  Inbound` permits an app to accept on a local port (baseline inbound permit,
  weight 12). `ngctl enforce-in <seconds>` runs it standalone with the same timed
  dead-man switch. (ICMPv6 ND is NOT at RECV_ACCEPT — it's transport-layer, so
  inbound default-deny here doesn't touch it; that's Phase D.)
- ✅ **VM-verified with the real host→VM path:** a listener on non-exempt port
  9999 was reachable at baseline (True), **BLOCKED during `enforce-in`** (14
  inbound filters active → False), reachable again after revert; a NEW SSH
  connection (port 22 exempt) worked throughout — **the anti-lockout holds, you
  can't get locked out.** (Testing gotcha: overlapping manual enforcers share the
  provider GUID, so one instance's panic clears them all — run one at a time.)
- ✅ **C2 — daemon integration, opt-in.** `meta('inbound_mode')` = `off`
  (DEFAULT) | `enforce`. Off = outbound-only enforcement exactly as before, but
  inbound is still *learned*; `enforce` = `ngd enforce` also installs the stable
  inbound baseline permits + inbound default-deny. Opt-in by design so an upgrade
  can never silently start blocking someone's listening services.
  - `flow_events` gained a **`direction`** column (populated from the ALE layer,
    like habits). The inbound baseline needs it — local/remote ports alone can't
    tell an inbound accept from an outbound connect (exactly what the old port
    heuristic was guessing). Old rows stay NULL → they never qualify, rather than
    being mislabelled inbound.
  - `kInboundBaselineSQL` mirrors `kBaselineSQL` but keys on `local_port` +
    `direction='in'`. (Habits couldn't be used: they key on `process_key`, and a
    signer thumbprint maps to hundreds of binaries — far too broad to permit.)
  - **`ngd inbound [on|off] [db]`** — no arg = *preview*: shows the mode and the
    exact inbound services that would be permitted. This is the "look before you
    leap" step that replaces a separate shadow mode: inbound is always being
    recorded, so you just watch the preview until it covers your real services,
    then turn it on.
  - VM-verified: preview correctly listed `sshd.exe :22/TCP (8 conns)`; with
    `inbound on` the daemon logged *"inbound default-deny active (1 inbound
    service permit(s))"* and filter count hit **37** (19 outbound + 2 outbound
    baseline + 14 inbound Tier-0/catch-all + 2 inbound baseline); SSH alive
    throughout; clean revert to 0. Real DB confirmed `inbound_mode=off`.
- ⬜ Inbound prompt UX is still a design call (auto-permit-known + log vs.
  prompt-per-conn; a listening server makes per-conn prompts noisy).

### Phase D — ICMP coverage + the ADR. ✅ DONE (premise disproved; no code needed)
- **The plan here was wrong, and measuring beat assuming.** Phase D assumed ICMP
  "isn't an ALE connection" and would be a silent hole needing
  `OUTBOUND/INBOUND_TRANSPORT_V4/V6` filters. **It isn't a hole:** WFP's ALE layers
  classify ICMP as a connection-like flow, so the existing outbound catch-all
  already blocks it. VM-proven: with outbound default-deny active `ping 8.8.8.8`
  → **"General failure"** (the WFP-block signature) at 100% loss; with enforcement
  off the same ping replies. Tier-0 *address* exemptions apply to ICMP correctly
  too — LAN ICMP is permitted (no "General failure", just no reply because the
  peer's firewall drops echo), public ICMP is blocked.
- ✅ **Decision: do NOT add transport-layer filters.** Redundant outbound, and
  actively harmful inbound — a blanket inbound ICMP block breaks Path MTU
  Discovery (ICMP frag-needed / ICMPv6 *Packet Too Big*; IPv6 can't fragment, so
  it blackholes large transfers) and would break IPv6 ND outright. Inbound ICMP is
  left unfiltered **deliberately**: low security value (recon/nuisance), Windows
  Firewall already drops inbound echo by default, high and subtle breakage risk.
- ✅ **ADRs written** (`docs/DECISIONS.md` D-5…D-8): direction-is-the-layer,
  the both-directions/both-versions ALE model + structural anti-lockout, the
  no-transport-filters call with its evidence, and an honest definition of what
  "zero coverage gaps" means (complete for connection-attributable traffic in
  user-mode WFP; below-ALE raw sockets + per-packet inspection remain Phase 5).

**Data model / UI (spans phases):** `habits` gains `direction` (key becomes
`(process_key, dest, port, proto, direction)`); `rules` parses v6 + gains direction
/ inbound `local_port`; dashboard Live/History/Baseline gain a Direction column
(now a hard fact, not a guess). **Safety:** every inbound test uses the timed
auto-revert dead-man switch; dynamic WFP session already means crash = fail-open
(now covers inbound); management Tier-0 verified installed before any inbound deny;
VM-only until shadow mode is watched on real hardware.

## Phase 3 — Habit scoring & autonomy

**Goal:** fewer prompts, smarter defaults — still no ML.

- Nightly compaction: decay, evict, and **promote** stable keys to explicit rules.
- Novelty score (recency + time-of-day + destination fan-out) drives auto-allow for
  low-novelty connections so you stop being asked about obvious ones.
- Configurable autonomy: `prompt-everything` → `auto-allow-low-novelty` →
  `auto-allow-and-summarize`.
- Expand `ngtray` from prompts-only into a full **dashboard window**: live connection
  feed, recent blocks with reasons, rule management (add / edit / remove), habit
  stats, the autonomy toggle, and the weekly digest rendered in-app.
- Weekly text digest of what changed (no model yet — just the frequency stats).

## Phase 4 — ML on completed flows

**Goal:** the "AI" earns its name, correctly — scoring finished flows, not SYNs.
See [`DESIGN.md` §6](DESIGN.md#6-where-ml-actually-lives-phase-4) for the full
architecture (two-model design, feature vector, the feedback loop, shadow-mode
rollout, data governance). Planned, not started — every item below is ⬜.

- ✅ **4a. Feature archival + flow-completion detection.** DONE. The flagged
  open question resolved **yes**: a spike proved TCP ESTATS
  (`GetPerTcpConnectionEStats`) delivers per-flow `DataBytesIn`/`DataBytesOut`
  reliably in pure user mode (786/786 reads, cumulative from connection start),
  and polling `GetTcpTable2` and diffing detects completion (a connection
  present then gone = closed, final stats captured). `ng::FlowCollector`
  (`src/core/flowstats.cpp`) writes one `flow_features` row per completed TCP
  flow — process identity (PID→`GetProcessImageFileNameW`→`IdentityResolver`),
  destination IP, port, observed duration, bytes in/out. Opt-in
  (`meta('feature_archive')`, off by default), auto-purged (30-day retention on
  start + `ngd features purge`). CLI: `ngd features [db] [secs]` collects,
  `ngd features dump` shows, `ngd features purge` prunes. Verified on the VM:
  11 real completed flows captured with sensible bytes/durations. *Known
  limits (fine for a foundation): destination is the raw IP (domain correlation
  comes when folded into the enforce daemon, which has the DNS watcher);
  connections open+closed inside one 2s poll are missed; a pre-existing
  connection's duration is measured from first-seen, not its true start.*
- ✅ **4b. Anomaly scorer (unsupervised, no external data).** DONE, both halves.
  **Trainer** (`scripts/train_anomaly.py`): reads `flow_features`, trains an
  Isolation Forest on your own flows, exports ONNX + a feature-spec JSON (the
  contract with the on-device vector builder — 8 features: log duration, log
  bytes in/out, out ratio, is-https, is-http, is-signed, hour). **On-device
  scorer** (`src/core/scorer.cpp`, `ng::AnomalyScorer`): `ngd` loads the ONNX
  model (from `anomaly.onnx` next to the DB) and scores each completed flow via
  ONNX Runtime, writing the result to `flow_features.anomaly_score` — **shadow
  mode only, zero effect on any rule** (`meta('ml_mode')` = shadow/active/off,
  default shadow; `ngd features mode` sets it, `ngd features dump` shows scores).
  ONNX Runtime v1.27.1 is **vendored** (`third_party/onnxruntime/`,
  `onnxruntime.dll` committed) and **loaded dynamically by full path** at scoring
  time, so the engine's core paths (enforce/record/panic, ngctl) have zero static
  dependency on it — if the DLL or model is missing, scoring silently disables and
  everything else runs. Verified on the VM: model + DLL load, flows scored
  (signed HTTPS download +0.04 = normal, small short flows −0.18), *and* clean
  graceful degradation with the DLL removed (no crash, no stray system-DLL
  version-mismatch). Ships in the installer.
- ✅ **4c. Supervised classifier (public dataset).** DONE. `scripts/train_supervised.py`
  maps a CIC-FlowMeter / CICIDS2017 CSV onto the **6 features we share with a
  wire-captured dataset** (log duration, log bytes in/out, out-ratio, is-https,
  is-http — *not* is-signed/hour, which a network IDS dataset lacks: the
  distribution/feature-mismatch reality called out in DESIGN.md §6). Trains
  LightGBM → ONNX (P(malicious) output) with a feature-spec JSON. On-device: the
  scorer was generalized (`AnomalyScorer` → generic `OnnxModel`, feature-vector
  building moved into the collector), so `ngd` runs **both** models per completed
  flow — anomaly (8 feats → `anomaly_score`) and supervised (6 feats →
  `malicious_score`), both shadow-mode, both optional. `ngd features dump` shows
  both. Verified: trainer discriminates synthetic IDS data (benign P=0.0,
  malicious P=1.0); on the VM both models load and score a real flow (signed
  HTTPS download → anom +0.03, mal 0.00). **Still a placeholder until trained on
  real data** — the public dataset is enterprise/lab traffic, so trust it only
  after shadow mode proves it on your own flows.
- ✅ **4d. Confidence gate + demotion wiring.** DONE, built inert-by-default.
  `meta('ml_mode')` shadow→active (defaults to shadow, even across upgrades). In
  active mode the collector, on each captured flow, gates the scores: supervised
  `P(malicious) ≥ ml_malicious_threshold` (default 0.9) writes an `ml_flags`
  `demote` row; anomaly `≤ ml_anomaly_threshold` (default −0.15) writes a `review`
  row (advisory, touches no rule). The enforce baseline (`kBaselineSQL`, shared by
  `installBaseline` and the new read-only `ngd baseline` inspector) excludes any
  `(app,port,proto)` with a `demote` flag → it drops from the auto-permit baseline
  to default-deny and **prompts** on its next connection (block-notify-retry). It
  only ever *removes* a permit — never adds a block, never auto-blocks. Anomaly
  alone is a review flag, never a demotion. New CLI: `ngd baseline`,
  `ngd features flags|clear|demote|threshold`; dashboard Active radio enabled with
  a warning. **Inert until `ml_mode=active` AND a real captured flow crosses the
  gate** — default shadow does nothing. VM-validated: a demote for stable
  `curl.exe:443` drops it from the baseline (22→21 permits); `clear` restores it.
  *(The collector's active-mode write path is exercised by inspection + the manual
  `demote`→baseline round-trip; live capture-and-demote couldn't be reproduced
  over SSH because the collector's `GetTcpTable2` doesn't see SSH-spawned traffic —
  an env quirk, not a code defect: real interactive-session flows are captured.)*
- ✅ **4e. The feedback loop.** DONE. Every enforce prompt
  verdict — and each autonomy auto-allow — becomes a `feedback_labels` row via
  `EnforceDaemon::recordFeedback`: allow / once / auto-allow → label 0 (benign),
  block → label 1 (malicious). `ngd feedback` shows the dataset;
  `ngd feedback export <csv>` writes `duration_ms,bytes_in,bytes_out,remote_port,
  label` (byte counts joined from the matching completed `flow_features`; a blocked
  flow never completed, so those default to 0 and only the port-derived features
  carry signal). `train_supervised.py --feedback <csv>` folds these in alongside
  the public dataset (same 6-feature contract; verified on synthetic rows). Grows
  slowly *by design* — prompts get rare as the baseline learns, so it's periodic
  recalibration, not a fast loop.
- ✅ **4f. Weekly digest + optional LLM narration.** DONE.
  `ngd digest` gained a Phase-4 tail: ML demotions / review flags (`ml_flags`),
  the top-`P(malicious)` and most-anomalous completed flows (`flow_features`), and
  a one-line feedback summary. All advisory — none of it ever enforced.
  `scripts/narrate_digest.py` turns a piped digest into prose, offline-first
  (local Ollama → optional Anthropic API → dependency-free template fallback),
  always stamped advisory-only. *(An in-app dashboard digest is a future nicety;
  the CLI is the delivery mechanism.)*
- ✅ **4g. Dashboard surface (the WinUI GUI catches up to Phase 4).** DONE, first
  cut. Until now every Phase-4 feature was CLI-only (`ngd features …`), invisible
  in the dashboard — and DESIGN.md always intended scores to be *watched* in the
  GUI, so this closed a real gap. Settings tab gained a **Machine learning**
  section: a "Collect flow features" toggle (`meta('feature_archive')`) and a
  scoring-mode choice Off / Shadow / Active (`meta('ml_mode')`, Active disabled
  until 4d), written directly like autonomy (no elevation). New **Flows** nav
  item shows completed `flow_features` with their anomaly + P(malicious) scores,
  sortable by clicking either score column. Verified on the VM: dashboard loads
  the new UI, real scored flows appear.
  **Full CLI parity added (2026-07-11):** the Active radio is enabled with a
  warning; confidence-gate NumberBoxes (`ml_malicious/anomaly_threshold`) live in
  the ML settings; new nav views **Flags** (`ml_flags` demotions/reviews, with
  "Clear all" + right-click remove), **Baseline** (`ngd baseline` with right-click
  Distrust / Re-trust an app = `features demote` / re-trust), **Feedback**
  (`feedback_labels` with Export CSV = `feedback export`), and **Digest** (the
  `ngd digest` summary rendered in-app). All Phase-4 CLI features are now reachable
  from the GUI; actions write the DB directly (+bump `rules_gen`) like the rules
  editor. Deployed + launched clean on the VM interactive session.

**Real open risks, not hidden:** the public dataset is enterprise/lab traffic, not
one person's home network — expect a real transfer gap and don't trust 4c's scores
until shadow mode has proven it out against your own traffic. Byte-count features
(4a) may simply not be available without a spike into ESTATS. No auto-retraining
pipeline exists or is planned for v1 — model staleness is managed by you, manually,
on your own schedule.

## Phase 5 — (Optional) kernel callout driver

**Goal:** only if Phases 0–4 prove it's worth it.

- A KMDF WFP **callout driver** for the things user mode genuinely can't do: true
  **pend-and-prompt** at the SYN (`FwpsPendClassify0` → `FwpsCompleteClassify0` →
  `FwpsReleaseClassifyHandle0`), and per-packet features (timing, entropy) for
  inspected flows via the SPSC ring buffer.
- This is where EV cert + WHQL attestation, HVCI compatibility, Driver Verifier, and
  VM-only development become mandatory. It is a project in itself — hence last, and
  optional.

---

## Deliberately out of scope (see DECISIONS.md)

Windows Security Center / Action Center registration (gated Microsoft partner
program), enterprise MDM/GPO/ADMX, WFAS `.wfw` import/export, federated learning +
differential privacy, cloud model CDN + canary infra, ARM64/QNN NPU inference, EV
cert & WHQL for v1, and any Gbps throughput target. None of these serve a personal,
single-machine tool; each is re-openable if the project ever grows up into a product.

## Stack & references

C++20 · WIL · WFP management API (`fwpuclnt.lib`) · SQLite · ONNX Runtime (Phase 4)
· CMake + vcpkg. Read `simplewall` (henrypp) and Microsoft's `WFPSampler` before
writing Phase 0.
