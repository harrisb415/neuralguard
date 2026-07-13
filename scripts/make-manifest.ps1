# Emit dist/update-manifest.json for the current version, to be attached to the
# GitHub Release alongside the installer. The in-app updater fetches this file
# tokenlessly at:
#   https://github.com/<owner>/<repo>/releases/latest/download/update-manifest.json
# (the "latest/download" alias always resolves to the newest release's asset).
#
# Usage:  pwsh scripts\make-manifest.ps1
# Run it AFTER scripts\package.ps1 has produced dist\NeuralGuard-Setup-<ver>.exe.
$ErrorActionPreference = "Stop"

$owner = "harrisb415"
$repo  = "NeuralGuard"

$root = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $root "dist"

# --- version: CMakeLists is canonical; assert version.h agrees ----------------
$cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(NeuralGuard\s+VERSION\s+([\d.]+)') {
    throw "Couldn't find project(NeuralGuard VERSION x.y.z) in CMakeLists.txt"
}
$version = $Matches[1]

$verHeader = Get-Content (Join-Path $root "src\core\version.h") -Raw
if ($verHeader -notmatch '#define\s+NG_VERSION\s+"([\d.]+)"') {
    throw "Couldn't find NG_VERSION in src/core/version.h"
}
if ($Matches[1] -ne $version) {
    throw "Version drift: CMakeLists=$version but src/core/version.h=$($Matches[1]). Bump both."
}

# --- installer: hash + size ---------------------------------------------------
$installer = Join-Path $dist "NeuralGuard-Setup-$version.exe"
if (-not (Test-Path $installer)) {
    throw "Installer not found: $installer  (run scripts\package.ps1 first)"
}
$sha  = (Get-FileHash $installer -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $installer).Length

# --- manifest -----------------------------------------------------------------
$assetName = "NeuralGuard-Setup-$version.exe"
$manifest = [ordered]@{
    version   = $version
    installer = $assetName
    url       = "https://github.com/$owner/$repo/releases/download/v$version/$assetName"
    sha256    = $sha
    size      = $size
    notes     = "https://github.com/$owner/$repo/releases/tag/v$version"
    published = (Get-Date -Format "yyyy-MM-dd")
}

$out = Join-Path $dist "update-manifest.json"
$manifest | ConvertTo-Json | Set-Content -Path $out -Encoding utf8
Write-Host "Wrote $out" -ForegroundColor Green
Get-Content $out

# Release step (attach BOTH assets so the updater can find them):
#   gh release create v$version `
#     dist\NeuralGuard-Setup-$version.exe dist\update-manifest.json `
#     -R $owner/$repo --title "v$version — ..." --notes-file <notes>
