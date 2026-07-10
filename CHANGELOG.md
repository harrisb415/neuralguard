# Changelog

All notable changes to NeuralGuard are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versioning follows [Semantic Versioning](https://semver.org/).

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
