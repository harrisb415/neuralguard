# In-app updater (scaffold)

NeuralGuard already ships each release as a versioned Inno Setup installer
(`NeuralGuard-Setup-X.Y.Z.exe`) attached to a GitHub Release. The updater reuses
that: it detects a newer release, downloads the installer, verifies it, and hands
off to it. The installer does the actual file replacement — the updater never
swaps running binaries itself.

> **Status: scaffold.** The pure logic (version compare, manifest parse, URL
> construction) is complete. The WinHTTP fetch, SHA-256 verify, and installer
> hand-off are written but **not yet exercised end-to-end against a real
> release**. Validate on the VM before shipping (checklist at the bottom).

## Components

| Piece | Where | Role |
|-------|-------|------|
| `NG_VERSION` | `src/core/version.h` | The running build's version. Hand-bumped per release. |
| `ng::Updater` | `src/core/updater.{h,cpp}` | check → download → apply. WinHTTP + BCrypt + std only. |
| `ngd update` | `src/ngd/main.cpp` → `RunUpdate` | CLI: `ngd update [check\|apply]`. |
| Software-updates card | dashboard `MainWindow` Settings | GUI: check + install with progress. |
| `update-manifest.json` | release asset | version + url + sha256 + size + notes. |
| `scripts/make-manifest.ps1` | build host | Emits the manifest from the built installer. |

The **same `updater.cpp`** is compiled into both targets — into `ngcore` (CMake,
for `ngd`) and directly into the dashboard vcxproj (`NotUsing` PCH, like
`deps\sqlite3.c`). It deliberately has no `Db`/`ngcore` dependency so it drops
into either build.

## Manifest

Published as a release asset and fetched tokenlessly from the `latest` alias:

```
https://github.com/harrisb415/NeuralGuard/releases/latest/download/update-manifest.json
```

```json
{
  "version":   "1.2.1",
  "installer": "NeuralGuard-Setup-1.2.1.exe",
  "url":       "https://github.com/harrisb415/NeuralGuard/releases/download/v1.2.1/NeuralGuard-Setup-1.2.1.exe",
  "sha256":    "<lowercase hex>",
  "size":      44645596,
  "notes":     "https://github.com/harrisb415/NeuralGuard/releases/tag/v1.2.1",
  "published": "2026-07-12"
}
```

`check()` fetches it, reads `version`, and reports an update when
`compareVersions(latest, NG_VERSION) > 0`.

## Security model

- **Transport:** HTTPS to github.com (WinHTTP, redirects followed to the CDN).
- **Integrity:** the manifest carries the installer's SHA-256 + byte size;
  `download()` refuses a file that doesn't match either.
- **Trust gap (be honest):** the installer is **not** Authenticode-signed (v1
  dropped EV/WHQL), and the manifest itself is not signed. Integrity rests on
  HTTPS + the manifest hash. If the GitHub account or release is compromised, a
  bad installer could be served. **TODO(hardening):** Ed25519-sign the manifest
  and pin the public key in `updater.cpp` (mirror OpsPoint's `updater.js`), so a
  swapped installer/manifest is rejected. Optionally add Authenticode signing.

## Release flow (updated)

1. Bump `src/core/version.h` **and** `CMakeLists.txt` VERSION (+ README, CHANGELOG).
2. `scripts/package.ps1` → `dist/NeuralGuard-Setup-<ver>.exe`.
3. `scripts/make-manifest.ps1` → `dist/update-manifest.json` (asserts the two
   version strings match).
4. Commit the version files, then:
   ```
   gh release create v<ver> \
     dist/NeuralGuard-Setup-<ver>.exe dist/update-manifest.json \
     -R harrisb415/NeuralGuard --title "v<ver> — ..." --notes-file <notes>
   ```
   Both assets must be attached — the updater needs the manifest at
   `releases/latest/download/update-manifest.json`.

## Remaining TODOs before this is shippable

- [ ] Wire `updater.cpp` into the CMake `ngcore` target (+ `winhttp` link) and
      the dashboard vcxproj (`NotUsing` PCH + `..\..\src\core` include dir).
- [ ] `ngd update check` / `ngd update apply` end-to-end on the VM against a real
      `v-next` release (make a throwaway release one patch above current).
- [ ] Dashboard Settings card: confirm the background check + progress marshaling
      to the UI thread, and that the app exits cleanly so files unlock.
- [ ] Inno Setup: add `CloseApplications`/restart-manager handling so an in-use
      `NeuralGuard.exe`/service doesn't block the silent upgrade.
- [ ] Decide the elevation story for `apply()` (installer is per-user today).
- [ ] (Hardening) Ed25519-signed manifest + pinned key.
- [ ] (Nice) periodic background check with a "update available" nudge in the UI.
