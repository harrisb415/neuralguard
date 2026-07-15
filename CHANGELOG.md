# Changelog

All notable changes to NeuralGuard are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versioning follows [Semantic Versioning](https://semver.org/).

## [1.5.1] - 2026-07-15

Two bugs found within hours of shipping 1.5.0 by actually using it on a real
machine.

### Fixed

- **Installing the service jumped straight to enforcing, with no button ever
  pressed for it.** 1.5.0 had the service resume `desired_mode` on every start,
  but two spots still forced it to `enforcing`: the schema default for a brand
  new database, and an explicit write in `ngd install`/"Install as service"
  reasoned as "installing is an implicit protect-me." Neither should have
  decided that for you. A database that's never had a mode chosen now defaults
  to `idle`, and installing the service no longer touches `desired_mode` at
  all — whatever you'd already set (or the idle default) is what it resumes.
  Only the Enforce button, `ngd mode enforcing`, or `ngctl mode enforcing` ever
  turns enforcement on now.
- **Choosing "install for all users" during setup silently installed per-user
  anyway.** `DefaultDirName` was a literal `{%USERPROFILE}\NeuralGuard`, which
  ignores which install mode you picked. It's now `{autopf}\NeuralGuard` (Inno's
  auto-switching Program Files constant), and the login-startup shortcut
  switched from always-per-user to the matching auto constant too, so a
  per-machine install actually lands per-machine.
- Found while fixing the above: the dashboard's own startup-diagnostics log
  (`progress.txt`) was hardcoded to `%USERPROFILE%\NeuralGuard\dashboard\`,
  which would have silently written to a folder that doesn't exist for a
  per-machine install — breaking the one diagnostic tool this app has for a
  startup that fails before any window shows. It now writes next to the
  running exe.

## [1.5.0] - 2026-07-15

Stop actually stops, the service remembers what you left it in, and the four
executables become two.

### Fixed

- **Stop didn't stop it.** The dashboard's Stop button and the tray's Panic both
  terminated any process named `ngd.exe` — which is the background service's
  process name too. The SCM doesn't read a kill as a stop, it reads it as a
  *crash*, and the service is deliberately configured to restart on failure. So
  Stop appeared to work and then silently went back to enforcing about five
  seconds later, no reboot required. Both now ask `ngd stop`, which stops the
  service through the SCM and only hard-kills foreground workers, which have no
  SCM lifecycle of their own.
- **A reboot always came up enforcing**, whatever you'd last chosen — the service
  hardcoded enforcement on every start, and couldn't run learning mode at all. It
  now records what you *want* (`desired_mode`) separately from what's *running*,
  and resumes it.
- **The uninstaller had the same bug, and it bit for real.** It killed `ngd.exe`,
  the watchdog restarted the service during the confirmation dialog, and the
  uninstall's own stop request was then rejected because the service was still
  starting. The result: files deleted, gone from Add/Remove Programs, the tray and
  control tools removed — and a **still-enforcing service left running with nothing
  left that could stop it**. Upgrades had the same flaw in a quieter form: a
  restarted service re-locks `ngd.exe`, so the engine silently wasn't replaced.
- **A firewall could fail to start and report success.** If the enforcer couldn't
  start, the service reported a clean stop with exit code 0 — no error, nothing in
  the event log, and no restart, because the watchdog only retries on a *failure*
  exit. It now exits with a real error, and the watchdog recovers the transient
  cases on its own.
- **The tray icon didn't appear** until you opened the dashboard by hand. Starting
  at login was opt-in and unchecked, so you could install a background firewall and
  get no icon, no prompts, and no way to stop it without a command line. It's on by
  default now.

### Changed

- **Four executables are now two.** The tray was separate only because a
  `LocalSystem` service can't show UI — but that never meant the *frontend* had to
  be two programs, and being two meant duplicated mode polling, panic paths that
  drifted apart, and a tray whose **Status** could only open a `cmd.exe` window
  because it had no UI of its own. One app (`NeuralGuard.exe`) is now the tray icon
  *and* the window; Status and Panic render in the app. `ngtray.exe` is retired,
  and an upgrade removes the old one so it can't linger at login.
- **The frontend asks the service instead of racing it.** Enforce / Learn / Stop /
  Panic used to launch a whole second copy of the engine that knew nothing about
  the installed service and fought it for the same firewall provider. They now send
  a command to the running service, which switches mode **in place** — no restart,
  no rival process, and no UAC prompt, since the service is already elevated.
- **Panic is coherent.** It used to pull the filters out from under a daemon that
  kept running and still believed it was enforcing — so the next rule edit put them
  back. It now stops enforcement and stays stopped.

### Added

- `ngd mode [enforcing|learning|idle]` and `ngctl mode` — read or change what the
  service runs, live. `ngctl status` now reports the service's mode, and `ngctl
  panic` asks the service when one is running.
- `ngd stop [--off]` — `--off` means *and stay off*; plain `stop` is maintenance and
  preserves your mode, which is what an installer needs when it stops the service
  just to replace files.

## [1.4.0] - 2026-07-14

Full-coverage enforcement: **both directions, both IP versions**, with direction
read from the WFP layer instead of guessed from port numbers.

### Added

- **IPv6 enforcement.** Outbound IPv6 was previously *completely unfiltered* —
  a default-deny that only covered IPv4 left every IPv6 destination open. The
  outbound path is now mirrored onto `ALE_AUTH_CONNECT_V6`, with IPv6 Tier-0
  exemptions (`::1`, `fe80::/10`, `fc00::/7`, and `ff00::/8` multicast — Neighbor
  Discovery and MLD run over those, and IPv6 stops working entirely without them).
  Baseline and user app-permits install at both layers, so a permitted app keeps
  working over IPv6 instead of being wrongly blocked.
- **Inbound enforcement (opt-in).** New default-deny at `ALE_AUTH_RECV_ACCEPT_V4/V6`
  guarding your *listening services*, off by default (`ngd inbound [on|off]`) so an
  upgrade can never silently start blocking them. Anti-lockout is structural: SSH
  (22), RDP (3389), DHCP/DHCPv6, loopback and link-local are permitted *before* the
  catch-all, so no baseline, rule, or ML demotion can cut off your management
  channel. Inbound default-deny only affects *new* inbound accepts — the return
  traffic of connections you initiated is never re-classified, so browsing is
  untouched.
- **`ngd inbound`** — previews the mode, the inbound services that would be
  permitted, and the services we blocked pending your review. `ngd inbound allow
  <port>` permits one, live.
- **Inbound review in the dashboard** — a new **Inbound** view lists blocked
  inbound services (port, attempts, last peer); right-click to allow or re-block.
- Inbound connections are now **learned** (direction-aware habits) regardless of
  whether inbound enforcement is on, so the baseline is ready before you enable it.

### Changed

- **Direction is now a fact, not a guess.** Learning used to infer inbound vs
  outbound from `remotePort < 49152` — a heuristic that wrongly excluded outbound
  connections to high remote ports (P2P, some game/QUIC services) and wrongly
  included inbound connections from low source ports. Direction now comes from the
  ALE layer the event was classified at, which *is* the direction. The heuristic is
  deleted. `habits` and `flow_events` gained a `direction` column.
- **Inbound is never prompted, by design.** A remote party must never be able to
  put a dialog on your screen, and the decision you actually want is per *service*,
  not per connection. Novel inbound is blocked silently, recorded, and the tray
  balloons **once** per new service; you review and allow at your leisure. See
  `docs/DECISIONS.md` (D-5…D-8) for the full coverage model and its honest limits.

### Fixed

- **Live rule edits silently disabled inbound enforcement.** `reapply()` cleared
  every filter — inbound included — then rebuilt only the outbound half, failing
  inbound open. It now rebuilds both.
- ICMP was *investigated* rather than assumed: it turns out WFP's ALE layers
  already classify it, so outbound ICMP was already covered and no transport-layer
  filters were needed. Adding them would have broken Path MTU Discovery and IPv6
  Neighbor Discovery — see D-7.

## [1.3.1] - 2026-07-13

### Added

- **A real icon set.** NeuralGuard finally has its own brand mark instead of a
  stock Windows shield / generic app icon: a shield silhouette with a small
  neural-node mesh, cyan-to-blue gradient tile matching the title bar's
  existing brand chip. Hand-drawn per size tier (not rasterized from one SVG)
  so small sizes stay crisp - the mesh detail drops out below the size where
  it would smudge, leaving a clean solid shield. Generated by
  `scripts/gen_icons.py` (Pillow + a small custom .ico packer) into
  `assets/icons/`.
  - The dashboard's exe/taskbar/title-bar/Alt-Tab icon.
  - Four **live tray-state icons** - `ngtray` now polls `meta('mode')` and
    recolors its tray icon: cyan (learning), green (enforcing), red (just hit
    Panic), grey (idle/offline) - instead of a single static stock icon.
  - The installer wizard's own icon (`SetupIconFile`); the Start Menu/desktop/
    startup shortcuts inherit it automatically from `ngtray.exe`'s embedded icon.

### Fixed

- **`ngtray`'s Panic button had the same stale-status bug** the dashboard's
  Stop/Panic had before v1.2.1: it only removed the WFP filters, leaving the
  `ngd` worker running and `meta('mode')` pinned at its old value - so the new
  live tray icon would have kept showing green after a Panic. Fixed the same
  way: Panic now also terminates the `ngd` worker and resets the mode.

## [1.3.0] - 2026-07-12

### Added

- **In-app updater.** NeuralGuard can now update itself from its GitHub
  Releases. `ngd update [check|apply]` on the CLI, and a **Software updates**
  card in the dashboard's Settings (check, then download & install). It reads a
  signed-by-hash `update-manifest.json` published on the latest release,
  downloads the installer, verifies its size + SHA-256, and hands off to the
  silent installer (which closes the running app, replaces the files, and
  restarts). Shared `ng::Updater` (`src/core/updater.cpp`) compiled into both
  the engine and the dashboard; `NG_VERSION` in `src/core/version.h` is the
  build's version. Release manifests are produced by `scripts/make-manifest.ps1`
  (run automatically by `scripts/package.ps1`). See `docs/UPDATER.md`.

### Changed

- The installer now uses the Restart Manager (`CloseApplications`) and also
  force-closes `ngd.exe`, so an in-app update can replace binaries even while the
  app is running.

## [1.2.1] - 2026-07-12

### Changed

- **Dashboard dark-neon theme** — the WinUI dashboard was rebuilt to match the
  design prototype exactly: a custom integrated title bar (brand mark, mode
  buttons, live status dot), a custom sidebar with geometric glyph icons and a
  cyan selection bar, verdict/state **pill badges** (green allow, cyan
  cap-allow, red block/drop, amber monitor), per-view fixed columns, carded
  Settings, and a Mica backdrop.
- **Flows scores are colored** — anomaly turns amber when negative and
  P(malicious) turns red at/above 0.5.

### Fixed

- **Mode status was stuck on "enforcing."** `Stop` and `Panic` removed the WFP
  filters but left the `ngd` worker running, so `meta('mode')` never cleared.
  Both now terminate the worker and reset the mode, so the status bar is honest.
- **Verdict pills render reliably.** Pill colors are now computed in the row
  model and bound with `x:Bind` instead of a value converter — converters need a
  `FrameworkElement` root to resolve, which a `Window` lacks, so the badges had
  been coming back blank.

## [1.2.0] - 2026-07-11

### Added

- **Machine-learning tier (Phase 4)** — NeuralGuard now learns a statistical
  picture of your normal traffic and scores completed connections in the
  background, off the enforcement path. Two complementary models run on-device
  via ONNX Runtime: an **Isolation Forest** anomaly model ("unlike your own
  normal") and a **LightGBM** supervised classifier ("known-bad patterns").
  The runtime is loaded dynamically by full path, so a missing runtime or model
  degrades gracefully — the firewall runs exactly as before.
- **Shadow mode by default** — scores are logged and shown, never acting on a
  rule. An opt-in **Active** mode lets a strongly-malicious score *demote* a
  trusted app so it prompts again on its next connection; it only ever removes
  an automatic pass and **never auto-blocks**. Anomaly scores alone are advisory
  review flags, never a demotion. Confidence gates are tunable.
- **Feedback loop** — every prompt verdict (and each autonomy auto-allow)
  becomes a labeled training example; `ngd feedback export` writes a CSV that
  `scripts/train_supervised.py --feedback` folds into the next offline run.
- **Weekly digest** — `ngd digest` now surfaces the ML flags, the most
  suspicious and most anomalous flows, and a feedback summary. An optional,
  offline-first `scripts/narrate_digest.py` can turn it into prose (advisory
  only).
- **Off-device training** — `scripts/train_anomaly.py` and
  `scripts/train_supervised.py` build the two models from your own data (and a
  public IDS dataset) and export them to ONNX.
- **Dashboard catches up to Phase 4** — new **Flows**, **Flags**, **Baseline**,
  **Feedback**, and **Digest** views; a **Machine learning** settings section
  (collect toggle, scoring mode, confidence gates); and right-click
  **Distrust / Re-trust** on the Baseline view. Every Phase-4 CLI feature is now
  reachable from the GUI.

### Notes

- **The pipeline ships without pretrained models — by design.** The installer
  includes the ONNX runtime but no models, so scoring is inert (and harmless)
  until you train your own on your traffic and drop `anomaly.onnx` /
  `supervised.onnx` next to `ngd.exe`. The models are the detector; the shipped
  code is the proven pipeline around them. Prove any model out in shadow mode
  before ever enabling Active.

## [1.1.1] - 2026-07-10

### Fixed

- The installer's "Launch NeuralGuard now" finish-page step failed with
  *"CreateProcess failed; code 740 - The requested operation requires
  elevation"* on a real end-user run. `ngtray.exe`'s manifest requires
  Administrator, and Inno Setup's `[Run]` entries launch via `CreateProcess`
  by default, which can't auto-elevate a manifested-admin target — only
  `ShellExecute` can. Start Menu/desktop shortcuts were unaffected (shortcut
  activation is always shell-based); only this one post-install step needed
  the `shellexec` flag added.

## [1.1.0] - 2026-07-10

### Added

- **Installer** — a proper Windows installer (`installer/NeuralGuard.iss`,
  Inno Setup), built by `scripts/package.ps1` alongside the engine and
  dashboard. Installs per-user (no admin prompt at install time), creates
  Start Menu shortcuts, offers to launch NeuralGuard at login, and registers
  a normal Add/Remove Programs uninstaller. Uninstalling stops the tray
  and dashboard, removes the background service if installed, and clears
  any active enforcement filters (each elevated individually, since the
  installer itself intentionally never requires admin) before deleting
  files, so an uninstall can't leave a stray service or a stuck block
  behind. Download `NeuralGuard-Setup-1.1.0.exe` from the
  [releases page](https://github.com/harrisb415/NeuralGuard/releases).

### Fixed

- The dashboard's `NgDir()` (used to locate `ngd.exe`/`ngctl.exe` and the
  policy database) was hardcoded to `%USERPROFILE%\NeuralGuard`, so the
  install location wasn't actually free to be anything else. It's now
  derived from the dashboard's own module path — the app works wherever
  it's installed, including the arbitrary folder a user picks in the new
  installer.

## [1.0.0] - 2026-07-10

First tagged release. Phases 0–2 of [`docs/ROADMAP.md`](docs/ROADMAP.md) are
complete and most of Phase 3; the tool is safe and usable for daily,
single-machine use. See [`docs/INSTALL.md`](docs/INSTALL.md) to build and run it.

### Added — recording & learning

- `ngmon`: user-mode WFP net-event monitor — the Phase 0 spike proving the
  whole foundation (engine session, net-event subscription, per-connection
  process attribution).
- `ngd record`: persists every WFP net event to a SQLite policy store
  (`ngpolicy.db`), with a vendored, dependency-free SQLite build.
- Process identity resolution: device paths normalized to drive letters,
  SHA-256 hashed, and Authenticode-verified (embedded **and catalog**
  signatures, so in-box Windows binaries resolve correctly) — cached in a
  `process_identity` table.
- DNS correlation: a real-time ETW consumer of the DNS client provider maps
  resolved IPs to the domain name the app actually asked for.
- Habit tracking: a decaying `(process key, destination, port, protocol)`
  table (14-day half-life, hour/day histograms) forms the learned baseline.
  The process key survives app updates (signer thumbprint or hash, not path).
- `ngd digest` / `compact` / `novelty` / `promote`: baseline reporting,
  nightly decay/eviction, a novelty score (rarity + recency), and promotion
  of stable `(app, port)` pairs to trusted baseline entries.

### Added — enforcement

- `ng::Enforcer` + `ngctl`: NeuralGuard's own WFP provider and sublayer
  (weighted above Windows Defender Firewall), with `ngctl panic` as the
  first thing built — enumerate and delete every NeuralGuard filter, fail
  open, unconditionally.
- Tier-0 always-exempt rules (loopback, DHCP, DNS, NTP) and outbound
  default-deny, auto-permitting the learned, *stable* baseline.
- `ngd enforce`: the live enforcement daemon — installs baseline permits +
  default-deny, subscribes to WFP net events, and on a novel outbound
  connection, prompts (via the tray) instead of silently blocking. Allow
  writes a permit and the app's retry succeeds (block-notify-retry).
- Configurable autonomy: prompt on every new connection, auto-allow apps
  you already use, or auto-allow everything (log only) — set from the
  dashboard's Settings tab, read live by the enforcement daemon.
- Windows service (`ngd install` / `uninstall` / `service-run`): runs
  enforcement as LocalSystem at boot, with SCM restart-on-crash and a
  **dynamic** WFP session, so a crashed enforcer leaves zero filters behind
  (fail-open-on-death) rather than a stale block.

### Added — WinUI 3 dashboard (`gui/`)

- Full C++/WinRT rewrite of the control surface, replacing the earlier
  native Win32 dashboard: Live / Rules / Habits / Per-app / History views
  over the policy database, with sortable and independently resizable
  columns.
- Right-click rule management on Live/History rows (block/allow a
  destination, timed 1-hour allow, allow-app) and delete on the Rules tab;
  a filter box narrows any table by substring across all columns.
- Rule export/import as pipe-delimited text via the standard file dialogs.
- Settings tab: autonomy radios and Install/Remove for the background
  service, with live status from the Service Control Manager.
- Transient, opaque toast notifications for action feedback (rule changes,
  service state, elevation results).
- Elevation-aware control actions: Enforce/Learn/Stop/Panic and the service
  buttons request elevation via `ShellExecuteEx` only when the dashboard
  isn't already running elevated, so there's at most one UAC prompt.
- Ships **unpackaged and self-contained** (bundles the Windows App SDK
  runtime) — no separate framework install on the target machine.

### Added — system tray (`ngtray`)

- Tray icon with Status / Panic / Quit, and a named-pipe prompt server that
  the enforcement daemon calls into for block-notify-retry decisions.
- Double-click (or the Dashboard menu item, or `ngtray dashboard`) opens the
  WinUI 3 dashboard; if one is already open it's brought to the foreground
  instead of launching a second copy. The tray runs elevated
  (`requireAdministrator`), so the dashboard it launches inherits the token
  and needs no further per-action prompts.

### Fixed

- Installing the background service while a manual `ngd enforce` session
  was still running left the service in a permanently stopped state (both
  claim the same WFP provider). `ngd install` now stops any manual
  enforcer first, so the service reaches `RUNNING`.
- `ngtray dashboard`, run while a tray instance was already up, silently
  did nothing (the single-instance mutex check exited first). It now opens
  or focuses the dashboard before exiting.
- The Live/History views' one-second auto-refresh was tearing down an open
  right-click menu and clearing row selection mid-click. The refresh now
  pauses while a context menu is open and selection survives the refresh.
