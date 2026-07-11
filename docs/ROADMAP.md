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
- ⬜ Extend default-deny to IPv6; per-destination/domain tightening.
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
- ⬜ **4e. The feedback loop.** Every prompt decision (Allow once / Always allow /
  Block) becomes a labeled example in `feedback_labels`. A manual retraining script
  folds these into the next offline LightGBM run alongside the public dataset. Set
  expectations correctly: prompts get rare as Phases 2–3 do their job, so this
  dataset grows slowly *by design* — it's for periodic recalibration against your
  own environment, not a fast-turnaround feedback loop.
- ⬜ **4f. Weekly digest + optional LLM narration.** Surfaces anomaly/supervised
  flags in the still-open Phase-3 weekly digest delivery item. An offline LLM may
  turn it into prose — advisory only, it never enforces.
- ✅ **4g. Dashboard surface (the WinUI GUI catches up to Phase 4).** DONE, first
  cut. Until now every Phase-4 feature was CLI-only (`ngd features …`), invisible
  in the dashboard — and DESIGN.md always intended scores to be *watched* in the
  GUI, so this closed a real gap. Settings tab gained a **Machine learning**
  section: a "Collect flow features" toggle (`meta('feature_archive')`) and a
  scoring-mode choice Off / Shadow / Active (`meta('ml_mode')`, Active disabled
  until 4d), written directly like autonomy (no elevation). New **Flows** nav
  item shows completed `flow_features` with their anomaly + P(malicious) scores,
  sortable by clicking either score column. Verified on the VM: dashboard loads
  the new UI, real scored flows appear. *Remaining for later: 4d's review items
  and 4f's in-app digest plug into this surface as they land.*

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
