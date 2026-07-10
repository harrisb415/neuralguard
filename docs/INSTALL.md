# Installing NeuralGuard

NeuralGuard isn't distributed as a signed installer — it's a personal, solo-built
tool you compile yourself and run on your own machine. This guide covers building
it from source and getting it running, end to end.

If you haven't yet, read the **Safety, up front** section of the
[README](../README.md) before enabling enforcement. Develop and test against a
VM until you trust it; a firewall bug on your only machine can lock you out of
the fix.

## What you end up with

Four engine binaries (`ngmon`, `ngd`, `ngctl`, `ngtray`) plus a WinUI 3 dashboard,
all living together in one folder, e.g. `C:\NeuralGuard\`:

```
C:\NeuralGuard\
  ngmon.exe      - one-shot WFP telemetry spike (diagnostic; not needed day-to-day)
  ngd.exe        - the daemon: recording, enforcement, the Windows service
  ngctl.exe      - headless control (panic, status, block/allow, enforce)
  ngtray.exe     - the system-tray icon; opens the dashboard
  ngpolicy.db    - created on first run - the policy + learned-habit database
  dashboard\
    NeuralGuard.exe   - the WinUI 3 control dashboard, launched by ngtray
    (+ its own DLLs - see step 3)
```

## Requirements

- **Windows 11**, 64-bit.
- **Visual Studio 2026** (Community is fine) with the **Desktop development
  with C++** workload — this pulls in the MSVC toolset, CMake, and the
  Windows 10/11 SDK that both the engine and the dashboard need.
- Internet access for the first build, to restore NuGet packages for the
  dashboard (Windows App SDK, C++/WinRT).

Nothing else needs to be pre-installed: the engine has no external
dependencies (SQLite is vendored in `third_party/`), and the dashboard is
built **self-contained** — it bundles the Windows App SDK runtime itself, so
there's no separate framework install on the machine you deploy to.

## 1. Get the source

```powershell
git clone https://github.com/harrisb415/NeuralGuard.git
cd NeuralGuard
```

## 2. Build the engine

From a **Developer PowerShell for VS 2026** (or a normal PowerShell — the
script locates `cmake.exe` under the VS install):

```powershell
pwsh scripts\build.ps1
```

This configures CMake and builds `ngmon`, `ngd`, `ngctl`, and `ngtray` in
Release into `build\Release\`. Pass `-Clean` to force a full reconfigure.

## 3. Build the dashboard

The dashboard is a separate MSBuild project (Windows App SDK / C++/WinRT
projects don't fit CMake's model well) at `gui\NeuralGuard\NeuralGuard.vcxproj`.

**First build only** — restore its NuGet packages:

```powershell
msbuild gui\NeuralGuard\NeuralGuard.vcxproj /t:restore
```

If that reports a NuGet error, open `gui\NeuralGuard.slnx` in Visual Studio
once instead — its NuGet integration restores automatically on load, and the
command-line build below will work from then on.

Then build it:

```powershell
msbuild gui\NeuralGuard\NeuralGuard.vcxproj `
  /p:Configuration=Release /p:Platform=x64 /p:AppxPackageSigningEnabled=false
```

Output lands in `gui\NeuralGuard\x64\Release\NeuralGuard\` — a whole folder
(~185 MB, mostly the bundled App SDK runtime DLLs), not just one `.exe`.

## 4. Assemble the install folder

Pick a folder (this guide uses `C:\NeuralGuard\`) and copy:

```powershell
$dst = "C:\NeuralGuard"
New-Item -ItemType Directory -Force "$dst\dashboard" | Out-Null

Copy-Item build\Release\ngmon.exe,build\Release\ngd.exe,`
          build\Release\ngctl.exe,build\Release\ngtray.exe $dst

Copy-Item gui\NeuralGuard\x64\Release\NeuralGuard\* $dst\dashboard -Recurse
```

The `dashboard\` subfolder name and location matter: `ngtray` looks for
`dashboard\NeuralGuard.exe` next to itself.

## 5. First run — learning mode

Nothing above enforces anything yet. Start the tray icon:

```powershell
C:\NeuralGuard\ngtray.exe
```

It asks to run elevated once (`requireAdministrator`) — accept it; this is
what lets the dashboard it opens skip a UAC prompt per button click later.
Double-click the tray icon (or right-click → Dashboard) to open the WinUI
dashboard.

From the dashboard, click **Learn** — this records real connections into
`ngpolicy.db` without blocking anything. Let it run during normal use for at
least a few days; watch the **Habits** tab fill in. The **Rules**/**Live**
views are read/act-on-live-data from the start.

## 6. Turn on enforcement

Once you have a baseline you trust, click **Enforce** in the dashboard (or
`ngctl.exe enforce <seconds>` from the command line for a timed, auto-reverting
test run). This:

- permits your learned baseline and Tier-0 traffic (loopback/DHCP/DNS/NTP),
- default-denies everything else outbound,
- prompts you (via the tray) the first time something new tries to connect.

**The panic switch** — `ngctl.exe panic`, or Panic in the dashboard/tray menu —
deletes every NeuralGuard filter immediately and fails open. Know where it is
before you need it.

## 7. Run as a background service (optional)

To keep enforcing after you log off or reboot, use the dashboard's
**Settings** tab (Install service), or from an elevated prompt:

```powershell
C:\NeuralGuard\ngd.exe install "C:\NeuralGuard\ngpolicy.db"
```

This registers `NeuralGuard` as an auto-start LocalSystem service with a
restart-on-crash watchdog, and — since it uses a dynamic WFP session — a
crashed enforcer leaves **zero** filters behind rather than a stuck block.

If a manual `ngd enforce` session is still running when you install, it's
stopped automatically so the service can take over the WFP provider.

```powershell
C:\NeuralGuard\ngd.exe uninstall   # stop + remove the service
```

## Updating

There's no auto-updater yet. Pull, rebuild steps 2–3, and copy the refreshed
files over the install folder from step 4 (stop `ngtray`/`NeuralGuard.exe`
first so the files aren't locked). `ngpolicy.db` is untouched by an update.

## Uninstalling

1. If the service is installed: `ngd.exe uninstall`.
2. `ngctl.exe panic` (belt and suspenders — clears any stray filters).
3. Quit the tray icon and close the dashboard.
4. Delete the install folder.
