# Build NeuralGuard (engine + dashboard) and package it into a Setup.exe
# installer (Inno Setup). Usage:  pwsh scripts\package.ps1 [-Clean]
param([switch]$Clean)
$ErrorActionPreference = "Stop"

$root  = Split-Path $PSScriptRoot -Parent
$dist  = Join-Path $root "dist"
$stage = Join-Path $dist "stage"

# --- version -----------------------------------------------------------------
$cmakeText = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeText -notmatch 'project\(NeuralGuard\s+VERSION\s+([\d.]+)') {
    throw "Couldn't find 'project(NeuralGuard VERSION x.y.z ...)' in CMakeLists.txt"
}
$version = $Matches[1]
Write-Host "NeuralGuard version: $version" -ForegroundColor Cyan

# --- 1. build the engine (ngmon/ngd/ngctl/ngtray) -----------------------------
Write-Host "`n== Building engine ==" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "build.ps1") @(if ($Clean) { "-Clean" })
if ($LASTEXITCODE -ne 0) { throw "engine build failed" }
$engineOut = Join-Path $root "build\Release"

# --- 2. build the WinUI dashboard ---------------------------------------------
Write-Host "`n== Building dashboard ==" -ForegroundColor Cyan
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not (Test-Path $msbuild)) { $msbuild = "msbuild" }  # fall back to PATH
$guiProj = Join-Path $root "gui\NeuralGuard\NeuralGuard.vcxproj"

& $msbuild $guiProj /t:restore /v:m /nologo
if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed - see docs\INSTALL.md if this needs a one-time VS-side restore instead" }

& $msbuild $guiProj /p:Configuration=Release /p:Platform=x64 /p:AppxPackageSigningEnabled=false /v:m /nologo /clp:ErrorsOnly
if ($LASTEXITCODE -ne 0) { throw "dashboard build failed" }
$dashOut = Join-Path $root "gui\NeuralGuard\x64\Release\NeuralGuard"

# --- 3. stage the install layout ----------------------------------------------
Write-Host "`n== Staging ==" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path "$stage\dashboard" | Out-Null

foreach ($exe in "ngmon.exe", "ngd.exe", "ngctl.exe", "ngtray.exe") {
    Copy-Item (Join-Path $engineOut $exe) $stage
}
# onnxruntime.dll: ngd loads it at runtime for Phase-4b shadow scoring (optional -
# ngd runs fine without it). The engine build stages it next to ngd.exe.
$ort = Join-Path $engineOut "onnxruntime.dll"
if (Test-Path $ort) { Copy-Item $ort $stage } else { Write-Warning "onnxruntime.dll not found; installed build won't score flows" }
Copy-Item "$dashOut\*" "$stage\dashboard" -Recurse -Force
Write-Host "Staged: $stage"

# --- 4. compile the installer --------------------------------------------------
Write-Host "`n== Building installer ==" -ForegroundColor Cyan
$iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    $found = Get-ChildItem "C:\Program Files*\Inno Setup*\ISCC.exe" -EA SilentlyContinue |
             Select-Object -First 1 -ExpandProperty FullName
    if ($found) { $iscc = $found } else { $iscc = "ISCC.exe" }  # fall back to PATH
}
$iss = Join-Path $root "installer\NeuralGuard.iss"

& $iscc "/DAppVersion=$version" "/DSourceDir=$stage" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC (Inno Setup compile) failed" }

$setupExe = Join-Path $dist "NeuralGuard-Setup-$version.exe"
Write-Host "`nBuilt: $setupExe" -ForegroundColor Green

# --- 5. emit the in-app updater manifest (attach it to the release too) -------
Write-Host "`n== Updater manifest ==" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "make-manifest.ps1")
