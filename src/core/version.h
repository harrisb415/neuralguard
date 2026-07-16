#pragma once
// Single source of the RUNTIME version string, compiled into ngd/ngctl and the
// WinUI dashboard so the in-app updater can compare this build against the
// latest published release.
//
// BUMP THIS on every release, alongside CMakeLists.txt (project VERSION),
// README.md, and CHANGELOG.md. scripts/make-manifest.ps1 asserts NG_VERSION
// matches the CMakeLists version so the two can't silently drift.
#define NG_VERSION "1.5.4"
