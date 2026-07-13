# Changelog

All notable changes to NeuralGuard are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versioning follows [Semantic Versioning](https://semver.org/).

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
