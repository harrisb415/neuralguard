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
- ✅ **Inbound UX — silent block + passive review (never a prompt).** The outbound
  block-notify-retry model does NOT transfer inbound, for four reasons: (1) no user
  intent to anchor the question — *a stranger* caused it, not you; (2) it would be
  **remote-triggerable UI** — anyone who can reach the box could pop dialogs on your
  screen at will; (3) wrong granularity — the decision you want is per *service*
  ("should sshd be reachable?"), not per SYN; (4) the retry mechanic doesn't exist —
  outbound the *app* retries after Allow, but a scanner's SYN has already timed out.
  Plus volume: anything exposed is scanned constantly.
  - **Design:** auto-permit the learned inbound baseline, block everything else
    **silently**, and record it in `inbound_blocked` (deduped per app+local port+proto,
    with an attempts counter and last peer). The tray balloons **once** per new
    service (`notified` flips 0→1 atomically), never again. The user reviews and
    permits at leisure: `ngd inbound` lists blocked services, `ngd inbound allow
    <port>` permits one (unioned into the inbound baseline via `kInboundAllowedSQL`,
    so a hand-allowed service skips the >=3-connection stability bar).
  - **Only OUR drops are surfaced.** `FwpmFilterAdd0`'s filter id is now kept for the
    inbound catch-alls (`Enforcer::isOurInboundBlock`), so the review list can't fill
    with the inbound drops Windows Firewall makes constantly — those aren't ours to
    offer a permit for. Ids reset in `clearFilters()` so a recycled id can't misattribute.
  - **Bug found + fixed while building this:** `reapply()` (any live rule edit)
    called `clearFilters()` — which drops inbound too — then reinstalled only the
    outbound half, silently failing inbound OPEN. It now rebuilds inbound as well,
    which also re-captures the catch-all filter ids (they change on every re-add).
  - VM-verified end to end: 9999 reachable → inbound on → **silently blocked** (14
    attempts, exactly ONE balloon, listed with its peer) → `inbound allow 9999`
    (0→3 permits) → **reachable again**; SSH alive throughout.

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

## Process consolidation — one backend, one frontend

**Goal:** two executables, not four. `ngd` (the Windows service) becomes the single
owner of all WFP state, controlled only over IPC; the tray and the dashboard merge
into one frontend process; `ngctl` stops being able to touch WFP on its own.

**Why — two real bugs, one root shape.** Nothing in the system is the single source
of truth for "what should be running right now":
- [`ServiceMain`](../src/ngd/service.cpp) hardcodes an `EnforceDaemon` and
  `SetMetaMode(db, "enforcing")` on every start. There's no learning/off mode for the
  service — `Recorder` (learning) is only ever instantiated by the foreground CLI —
  and no persisted "what did the user actually want" to resume.
- The dashboard's `MainWindow::StopDaemons()` and ngtray's `StopAndPanic()` both
  `TerminateProcess` any process literally named `ngd.exe`. If the service is
  installed, that's the service's process — the SCM sees an unexpected exit, not a
  clean stop, and `ServiceInstall`'s configured `SC_ACTION_RESTART` (twice, 5s apart)
  brings it straight back up into the hardcoded enforce path. **Clicking Stop can
  silently revert to enforcing within seconds — not just after a reboot.**
- Structurally, three independent places can each hold live WFP filters against the
  same provider: the service's `EnforceDaemon`, a foreground `ngd.exe enforce`/`record`
  spawned fresh by the dashboard's Enforce/Learn buttons (`RunTool(...)`, completely
  unaware the service exists), and `ngctl` (links `ngcore`, opens its own dynamic
  `Enforcer` session for `enforce`/`enforce-in`/`panic`). Only a one-shot cleanup at
  install time (`StopManualEnforcers`) papers over the overlap.

**Prior art:** TinyWall and Windows Defender Firewall both use exactly two roles — one
privileged backend that is the *only* thing touching the filtering engine, and one
frontend process that is simultaneously the tray icon and the window, talking to the
backend over IPC/RPC. simplewall skips the service entirely (single always-elevated
process) — not an option here, since the whole point of installing NeuralGuard as a
service is protection before/without a login. TinyWall's shape is the target.

### Phase A — Stop/Panic stop crashing the service ✅ DONE
- ✅ **New `ngd stop [db]`** owns the distinction both UIs were getting wrong: the
  service is stopped through the SCM (`ControlService(SERVICE_CONTROL_STOP)`, then
  poll until it reports `STOPPED` — `ControlService` only *requests* the stop), while
  a foreground worker, which has no SCM lifecycle, is killed as before (its dynamic
  WFP session tears down with it = fail-open). `StopManualEnforcers` gained an
  `excludePid` so the sweep can never touch the service's own process.
- ✅ **Both UIs delegate to it.** `MainWindow::StopDaemons()` and ngtray's
  `StopAndPanic()` no longer enumerate processes at all — the by-image-name kill is
  gone from both. The tray runs it off the UI thread and freezes its mode poll until
  the stop lands (`g_stopping` + `WM_STOP_DONE`), so the icon can't flap back to
  green while the daemon is still legitimately unwinding.
- ✅ **`meta('mode')` is now set by the thing that did the stopping.** The UIs used to
  write `idle` speculatively; since mode is only touched on transitions, an optimistic
  `idle` written before a stop that then *failed* was a lie nothing would ever
  correct. `ngd stop` writes it, and only when the stop actually succeeded.
- ✅ **VM-verified, including reproducing the bug first.** With the old binary, a
  kill-by-image-name showed `STOPPED` (so the UI looked right) and the service was
  back **RUNNING under a new PID ~10s later**; the SCM's own event-log entry spells
  it out: *"terminated unexpectedly … corrective action will be taken in 5000
  milliseconds: Restart the service."* With the fix: service-only → stopped, still
  stopped after 15s, filters 0, `mode=idle`; worker-only → *"Stopped 1 foreground
  worker(s)"*, 107 filters → 0; **both at once** → *"service stopped (filters
  reverted)"* + *"Stopped 1 foreground worker(s)"*, and 15s later both still gone —
  proving the service went through the SCM and was never hard-killed. Re-running
  against nothing prints *"Nothing to stop"* and exits 0.
- ✅ **The installer was a third caller of the same bug — found the hard way.** Both
  `CurStepChanged(ssInstall)` and `CurUninstallStepChanged` ran
  `taskkill /F /IM ngd.exe`. Observed for real on the physical host: an uninstall
  killed the service, the SCM restarted it ~5s later (during the confirmation
  MsgBox), so `ngd uninstall`'s `ControlService(STOP)` hit a `START_PENDING`
  service and was rejected — `DeleteService` then only *marked* it. Result: the
  files were deleted but **a still-enforcing service was left running**, invisible
  in Add/Remove Programs, with ngtray/ngctl/dashboard already gone so nothing could
  stop it (and it held `ngd.exe` + the DB locked, which is why those two survived).
  Same latent bug on upgrade: a restarted service re-locks `ngd.exe`, so the engine
  silently doesn't get replaced. Both paths now call a shared `StopNeuralGuard`
  (`ngd stop` via `runas`, then taskkill for the GUI processes only, which have no
  SCM lifecycle). It returns success so a declined elevation prompt *warns* rather
  than silently leaving a firewall behind, and the failure text now says how to
  recover. Costs no extra UAC on the normal update path: `Updater::apply` launches
  the installer with the default verb, so it inherits the already-elevated
  dashboard's token. Verified: `ISCC` compiles the script clean.
- **Still open, by design (Phase B):** the service is `SERVICE_AUTO_START` and
  `ServiceMain` hardcodes enforce, so a **reboot** still comes up enforcing
  regardless of what you left it in. Phase A only fixes the "Stop didn't stop it"
  half; durable intent needs `desired_mode`.

### Phase B — Service becomes mode-aware, controlled over IPC ◐ B1 DONE
- ✅ **B1 — `desired_mode`, and the service actually resumes it.** New
  `meta('desired_mode')` (`enforcing` | `learning` | `idle`) records what the user
  *wants*; `meta('mode')` stays what *is* running. `ServiceMain` no longer hardcodes
  enforcement — `RunDesiredMode` dispatches to `EnforceDaemon`, to `Recorder`
  (**learning was previously unreachable from the service at all** — only ngd's
  foreground CLI ever built one), or to an idle wait that keeps the service alive
  holding no filters (that's also where B2's command listener will live).
  - `ngd stop` gained **`--off`** to separate two different intents that were about
    to be conflated: `--off` = *the user* asked to stop, so persist
    `desired_mode=idle` and stay off across reboots (Stop button, tray Panic);
    plain `stop` = maintenance, leave intent alone. Caught while wiring it: the
    installer calls `ngd stop` to unlock files during an **upgrade**, so had stop
    always written `idle`, every upgrade would have silently left the user
    unprotected at the next boot. `ngd install` conversely asserts
    `desired_mode='enforcing'` — installing the service is an explicit "protect me".
  - New `ngd mode [enforcing|learning|idle] [db]` reads/sets intent (writes meta
    only, no admin). Seeded to `enforcing` so upgrades never silently stop
    protecting; `Db::meta`/`Db::setMeta` added and the ~5 private copies of those
    two statements start collapsing onto them.
  - **VM-verified end to end:** enforcing → Stop(`--off`) → *simulated reboot*
    (`sc start`) → comes up **RUNNING but idle, 0 filters** (old behaviour: straight
    back to enforcing). `mode learning` → restart → **`mode=learning`, 0 filters**,
    and confirmed genuinely recording (483,501 → 483,549 flow_events under real
    traffic) — the service running learning mode for the first time. Plain `ngd stop`
    preserved `desired_mode=learning`, proving the upgrade path is safe.
- ✅ **B2 — live control over IPC.** The service now hosts a command channel and
  switches modes in place, so changing what the engine does no longer means
  restarting it or (as the dashboard did) launching a rival copy of it.
  - **A second pipe, not the existing one.** `\\.\pipe\neuralguard`'s server is the
    *tray* (ngd connects to it to ask the user about a block); commands run the
    opposite way, and a pipe has one server — hence `\\.\pipe\neuralguard-cmd`
    with ngd as server (`core/cmd.h`, shared `CmdSend` client). The roadmap's
    "extend the existing pipe" wasn't possible; this is the same idea, corrected.
  - **Security is structural:** the pipe takes the default DACL for a
    LocalSystem-created pipe, which grants write to SYSTEM/Administrators only, so
    commands require elevation with no check of ours to get wrong. The tray is
    already elevated and the dashboard inherits its token → no UAC per action.
  - Verbs: `STATUS`, `MODE <enforcing|learning|idle>` (persists desired_mode *and*
    switches live), `PANIC` (revert filters, go idle, **stay** idle). `RunModeLoop`
    re-reads desired_mode whenever a mode returns due to a switch rather than a stop.
  - `ngctl` gained `mode`, and `panic`/`status` are now service-aware. `panic` asks
    the service first: a *local* panic only rips filters out from under a daemon
    that keeps running and still believes it's enforcing (it would re-apply them on
    the next rule edit) — the service's own panic stops enforcing and stays stopped.
  - ✅ **Fixed a silent-failure bug found while testing:** a failed enforcer start
    reported `SERVICE_STOPPED` with **exit code 0** — indistinguishable from a
    deliberate stop, so no event was logged and the SCM's restart policy (which only
    retries on a *failure* exit) never fired. A firewall could fail to start and
    report success. `RunOneMode`/`RunModeLoop` now propagate setup failure and
    ServiceMain exits `ERROR_SERVICE_SPECIFIC_ERROR`. Verified by forcing the real
    provider-conflict race: `WIN32_EXIT_CODE 1066` + *"terminated with the following
    service-specific error"* in the event log, where it used to be silent — and the
    restart policy now self-heals the transient case.
  - **VM-verified:** enforcing (111 filters, PID 128) → `ngctl mode learning` → mode
    switched to learning with **0 filters and the same PID 128** — in place, no
    restart, no rival process; back to enforcing → 109 filters; `mode enforcing`
    again → *"OK already enforcing"*; `mode bogus` → `ERR`; `ngctl panic` → service
    stays RUNNING at `mode=idle desired=idle`, 0 filters.
- Extend the existing `\\.\pipe\neuralguard` protocol (today: tray-notify only, one
  direction) with request/response command verbs — `MODE`, `ENFORCE`, `LEARN`,
  `STOP`, `PANIC` — so a *running* service can be told to switch modes live, instead
  of the dashboard spawning a competing foreground `ngd.exe` to get a different mode.
- `ngctl` becomes a thin pipe **client** for these verbs and stops calling `Enforcer`
  itself. (Its standalone timed test commands — `enforce <seconds>`, `enforce-in
  <seconds>` — either move behind a debug flag or retire once the dashboard's own
  controls are trusted; decide when we get there.)
- This closes the structural half of bug 1 (no learning mode, no persistence) and
  the three-independent-WFP-owners problem in the same pass.

### Phase C — Merge `ngtray` into the WinUI dashboard ✅ DONE
- ✅ **One frontend process.** `gui/NeuralGuard/Tray.cpp` owns the icon, the menu,
  the balloons and the prompt pipe (`\\.\pipe\neuralguard`) — all lifted from
  ngtray. The message-only window is created on the **WinUI UI thread** (which
  already pumps messages), so menu clicks land there and can touch XAML with no
  marshalling; the prompt pipe keeps its own thread, since it blocks on a client
  and a blocked UI thread is a dead app.
- ✅ **Status and Panic render in-app** (InfoBar) via the B2 command pipe — the
  `cmd.exe /k ngctl status` window is gone. Closing the window hides to tray
  (`AppWindow().Closing` → `Cancel(true)` + `Hide()`); `--tray` starts it as an icon
  with no window; Quit really exits.
- ✅ **Enforce/Learn/Stop/Panic now command the service** instead of spawning a
  rival `ngd.exe` that fought it for the same WFP provider. Falls back to the old
  foreground worker only when no service is reachable, so a service-less setup still
  works. `requireAdministrator` moved from ngtray's manifest to the dashboard's —
  it used to inherit ngtray's token, and the command pipe only admits Administrators.
- ✅ **`ngtray.exe` retired** from CMake, `package.ps1` and the installer.
- **VM-verified:** dashboard launches clean (`MainWindow made → Mica set →
  Activated`, tray started in the ctor before Activate); **both** pipes served —
  `neuralguard` by the *dashboard* (ngtray not running) and `neuralguard-cmd` by
  the service; `ngctl notify` blocked waiting on a real prompt dialog, proving the
  block-notify-retry path survived the move; `requireAdministrator` confirmed in the
  embedded manifest. *(The icon's appearance itself needs a human eye on the VM
  console — I can't see the GUI over SSH.)*

### Phase D — Installer catches up ✅ DONE
- ✅ Single Start Menu entry → `dashboard\NeuralGuard.exe`; `UninstallDisplayIcon`
  likewise. `[Files]` no longer ships `ngtray.exe`; `[Run]` launches the dashboard
  (keeping `shellexec`, since a manifested-admin target can't be CreateProcess'd).
- ✅ **"Start at login" is now checked by default.** Leaving it opt-in meant you
  could install a background firewall and get no icon, no prompts, and no way to
  stop it without a command line — which is exactly what happened on the physical
  host. The startup shortcut passes `--tray`.
- ✅ **`[InstallDelete]`** removes a ≤1.4.0 `ngtray.exe` and its startup shortcut on
  upgrade, or a stale tray would keep starting at login and fight the dashboard for
  the icon and the prompt pipe.
- Verified: `ISCC` compiles the script with ngtray absent from the staged layout.

**Non-goal:** one process total. Session-0 isolation means a LocalSystem service can
never own UI in the interactive desktop session — two processes (backend service +
frontend tray/dashboard) is the real floor, not one.

### Post-release fixes (found by actually using v1.5.0/1.5.1/1.5.2/1.5.3) ✅ DONE — shipped as v1.5.1, v1.5.2, v1.5.3, v1.5.4
Both found within hours, from the user installing on their physical host and
reporting exactly what they saw — not from VM testing, which hadn't exercised
either path.
- ✅ **Install-as-service still jumped to enforcing with no button pressed.** B1's
  fix made the service resume `desired_mode`, but two spots still forced it to
  `enforcing`: the schema seed's default for a brand-new database, and an explicit
  write in `ServiceInstall()` reasoned as "installing is an implicit protect-me" —
  which is exactly the presumption the user called out: *"nothing should ever go
  right to enforce mode unless i press enforce."* Fresh databases now default to
  `idle`; `ServiceInstall()` no longer touches `desired_mode` at all. VM-verified
  the real failure mode: uninstall the service, set `desired_mode=idle`, reinstall
  (the literal "Install as service" button path) → `mode=idle desired=idle`, 0
  filters — where it previously would have jumped to enforcing.
- ✅ **Choosing "install for all users" silently installed per-user anyway.**
  `DefaultDirName={%USERPROFILE}\NeuralGuard` is a literal path that ignores which
  install mode Inno's own admin-mode dialog resolved to. Switched to `{autopf}`
  (Program Files in per-machine mode, falls back correctly in per-user mode) and
  the login-startup shortcut from `{userstartup}` to `{autostartup}` to match.
  `[InstallDelete]` still cleans up the old always-per-user shortcut on upgrade.
- ✅ **Found while fixing the above, not reported:** the dashboard's own startup
  diagnostics log (`App.xaml.cpp`'s `Mark()`) hardcoded
  `%USERPROFILE%\NeuralGuard\dashboard\progress.txt` — for a per-machine install
  this would have silently written to a folder that doesn't exist, breaking the one
  diagnostic tool for a startup that fails before any window shows. Now derives the
  path from the running exe's own module path (`NgDir()` already did this
  correctly; `Mark()` was the one place that hadn't been updated to match).
- ✅ **1.5.2 — the tray icon never appeared at login, on either machine it was
  tried on.** "Start at login" installed a Startup-folder shortcut to the
  requireAdministrator-manifested dashboard. Windows does not reliably
  auto-elevate a manifested-admin exe launched from the Startup folder at
  logon — the process just doesn't get far enough to log anything, which is
  exactly why `progress.txt` had zero "Hidden to tray" entries despite the
  machine having been used since installing. Replaced with a **scheduled task**
  (`schtasks /create ... /sc onlogon /rl highest`, created via `ShellExec('runas', ...)`
  since registering an /rl highest task needs an elevated caller and Setup itself
  might not be one) — the standard mechanism for auto-launching an elevated app
  at logon without a UAC prompt every time, because Task Scheduler's "run with
  highest privileges" grants elevation from the token established once at
  registration, not via a fresh interactive consent each run.
  - **Verified conclusively on the host** (which has a real logged-in interactive
    user, unlike the VM at the time): registered the exact task the fixed
    installer creates, triggered it from an *unelevated* context while polling
    for `consent.exe` every 300ms — **never appeared** — and the resulting
    process (a) actually launched, (b) survived an unelevated `Stop-Process`
    attempt (access denied → genuinely elevated), and (c) reached "Hidden to
    tray" in `progress.txt`. All from a single, silent, unattended trigger.
  - The equivalent Startup-shortcut-based launch, tested the same way earlier,
    had `Last Run Time` stuck at Task Scheduler's "never run" sentinel and zero
    log entries — consistent with the process never getting past whatever gate
    stopped it from launching at all.
  - `[InstallDelete]` now also removes the `{autostartup}\NeuralGuard.lnk` that
    1.5.0/1.5.1 installed, so upgrading can't leave the old (broken) shortcut
    sitting alongside the new task.
- ✅ **1.5.3 — the 1.5.2 login task wasn't quite finished, and the Live/History
  views had two real UX bugs**, all found the same day the user actually lived
  with the app instead of just installing it.
  - **Task Scheduler's own defaults undid half the point of a login task.** The
    1.5.2 task had "start only on AC power" / "stop if going on batteries"
    active — Task Scheduler's default for a new task, not something either
    schtasks call had set. **The user found this themselves**: NeuralGuard
    vanished on unplugging a laptop, traced it to the task, and unchecked it by
    hand in Task Scheduler before this landed — this makes that fix permanent
    for every install instead of a one-off manual correction on one machine.
    `InstallStartupTask` now builds a full XML task definition (`schtasks
    /create /xml`, since the plain-flag syntax has no switch for either
    setting) with both explicitly disabled. **Found in the same pass, not
    reported:** Task Scheduler also defaults `ExecutionTimeLimit` to 3 days —
    harmless for a batch job, fatal for a persistent tray, which any always-on
    machine would eventually hit. Set to unlimited (`PT0S`) in the same XML.
    Verified on the host: registered the actual generated task, confirmed
    `DisallowStartIfOnBatteries=false`, `StopIfGoingOnBatteries=false`,
    `ExecutionTimeLimit=PT0S` in the task's own XML, then triggered it
    unelevated while polling for `consent.exe` (never appeared) and confirmed
    it survived an unelevated `Stop-Process` (genuinely elevated).
  - **History was removed entirely.** It turned out to be running the exact
    same query as Live, filtered to `verdict LIKE '%DROP%' OR verdict='BLOCK'`
    — a tab just labeled "History" reads as "everything that happened," and
    showing only denials with no on-screen indication that it was pre-filtered
    looked like a bug ("why is it all red?"). Asked the user whether to keep
    the filter and rename the tab, or drop the filter so History matches Live;
    they chose the latter — which makes the two tabs byte-for-byte identical,
    so the tab was deleted outright (nav item, view-tag branches, template/
    column wiring) rather than kept as a redundant duplicate.
  - **Live couldn't be scrolled while the 1-second auto-refresh was running.**
    `RefreshCurrent()` replaced the ListView's entire `ItemsSource` every tick,
    which resets a WinUI `ListView`'s scroll position to the top. First fix:
    capture the `ScrollViewer`'s vertical offset before the rebuild and restore
    it after (deferred one dispatcher tick so the restore doesn't race the
    pending layout pass) — scrolling now held, but every tick still visibly
    **flickered**, since replacing ItemsSource tears down and recreates every
    realized row container even when most rows didn't change.
  - **The flicker fix needed a second attempt.** First cut: keep a persistent
    `IObservableVector` for Live and mutate it in place via a common-prefix/
    common-suffix diff against last tick's row ids, instead of replacing
    ItemsSource. This is wrong for this specific feed: the query is capped at
    `LIMIT 300`, so once the feed is full, every new row at the front pushes
    one off the back - which shifts every surviving row's *index* by however
    many rows arrived. Index-aligned prefix/suffix comparison then matches
    nothing most ticks, degenerating to "remove all, insert all" - worse than
    the swap it was meant to replace. User-reported ("still flashing") caught
    this before it shipped. Rewritten to match what the feed actually is: find
    where last tick's first row reappears in this tick's results (that
    position is exactly how many rows are new), verify the remainder still
    lines up, and only insert the new rows at the front + trim any excess off
    the tail - the true minimal edit for an append-at-front, trim-at-back
    sliding window. Falls through to a full rebuild if the alignment check
    fails (a filter or sort is active, or a burst exceeded the page size), so
    it degrades to correct-but-unoptimized rather than wrong.
  - Deployed and confirmed on the live host via direct exe/xbf/pri hot-swap
    ahead of packaging a release — each of the three iterations (scroll only →
    broken flicker fix → corrected flicker fix) was confirmed or refuted by the
    user directly, since `NeuralGuard.exe` runs elevated and Windows' UIPI
    blocks a standard-integrity automation session from reading OR controlling
    an elevated window's content (confirmed directly: a computer-use screenshot
    of the running window came back solid black despite a valid, visible,
    non-minimized window rect - the same restriction that blocks driving Task
    Manager or a UAC prompt).
- ✅ **1.5.4 — the in-app updater never confirmed completion or relaunched.**
  `OnInstallUpdate` downloads, calls `Updater::apply()` (launches
  `Setup.exe /VERYSILENT /NORESTART /SUPPRESSMSGBOXES` via `ShellExecuteExW`),
  shows "closing to finish updating," waits 1.5s, then exits the dashboard so
  the installer can overwrite its files - all of which worked correctly (a
  manual restart always showed the update had actually landed). The gap was
  entirely on the other side: the installer's own `[Run]` "Launch NeuralGuard
  now" entry had `skipifsilent`, so it explicitly did NOT fire during a
  `/VERYSILENT` run. `updater.cpp` is the ONLY caller that ever installs
  silently (confirmed by grep across `src/`, `scripts/`, `installer/`), and it
  does so specifically because it's completing an update the user asked for
  in-app - `skipifsilent` was exactly backwards for that one caller. Removed;
  no `--tray` argument on that Run entry either, since the user is actively at
  the app when this fires and the window reappearing IS the confirmation - no
  separate "update succeeded" message needed.

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
