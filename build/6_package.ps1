# 6_package.ps1 - build the portable release zip from .build, whitelist only.
#
# The manifest and the rules live in build/打包清单.md. In short: the folder a
# user unzips must contain exactly what the release advertises - AHKDeck.exe,
# AutoHotkey64.exe (m192 P4: the ONE kernel, both the engine's per-session
# child and the image 「编译为 EXE」 bundles), Scripts\ (empty - the folder the
# deck scans), docs\ (the MCP access doc), licenses\ (upstream license texts),
# LICENSE, THIRD-PARTY-NOTICES.md. Never: AutoHotkey64.dll or tools\ (the m190
# AHK_H pair, retired in m192 P4), settings.json, hotkeys.txt, temp\ (m177:
# logs, backups, work, deleted, the update cache and the updater's own scratch),
# or the pre-m177 names (Logs\, Backups\, .temp\, ahkdeck_backup\,
# ahkdeck_update.log). Those are per-machine state, not the product.
#
# AutoHotkey64.exe is a REQUIRED item: a release built where the kernel was
# missing would break both running scripts and 编译为 EXE on every user machine.
# The throw below fails that build closed.
#
# The zip is made with 7z (m119), not System.IO.Compression. Set AHKDECK_7Z to
# the full path of 7z.exe/7za.exe when it is not on PATH.
#
# Usage (from anywhere; paths are derived from this script):
#   powershell -ExecutionPolicy Bypass -File .main\build\6_package.ps1

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$build = Join-Path $root ".build"
$temp = Join-Path $root ".temp"

if (-not (Test-Path (Join-Path $build "AHKDeck.exe"))) {
    throw "AHKDeck.exe not found in .build - run 4_build.bat first"
}

# Version comes from CMakeLists' project() line, the single source of truth.
$line = Select-String -Path (Join-Path $root ".main\CMakeLists.txt") `
    -Pattern 'project\(AHKDeck VERSION ([0-9.]+)' | Select-Object -First 1
if (-not $line) { throw "project(AHKDeck VERSION ...) not found in CMakeLists.txt" }
$version = $line.Matches[0].Groups[1].Value

$stage = Join-Path $temp ("package-" + $version)
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

$items = @("AHKDeck.exe", "AutoHotkey64.exe", "LICENSE", "THIRD-PARTY-NOTICES.md",
           "docs", "licenses")
foreach ($item in $items) {
    $src = Join-Path $build $item
    if (-not (Test-Path $src)) { throw "missing release item in .build: $item" }
    Copy-Item $src -Destination $stage -Recurse -Force
}

# Scripts ships EMPTY (m125). Copying .build\Scripts would put whatever the
# developer happens to be testing with into the release - the deck scans that
# folder, so a stray probe script ends up on every user's disk. The folder still
# has to exist in the zip (some extractors drop empty directories, and the deck
# recreates it at startup anyway), so it is created here.
$srcScripts = Join-Path $build "Scripts"
if (-not (Test-Path $srcScripts)) { throw "missing release item in .build: Scripts" }
New-Item -ItemType Directory -Path (Join-Path $stage "Scripts") | Out-Null

$sevenZip = $env:AHKDECK_7Z
if (-not $sevenZip) {
    $found = Get-Command 7z, 7za -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $sevenZip = $found.Source }
}
if (-not $sevenZip) {
    foreach ($candidate in @(
            (Join-Path $env:ProgramFiles "7-Zip\7z.exe"),
            (Join-Path ${env:ProgramFiles(x86)} "7-Zip\7z.exe")) ) {
        if (Test-Path $candidate) { $sevenZip = $candidate; break }
    }
}
if (-not $sevenZip) {
    throw "7-Zip not found - install it or set AHKDECK_7Z to 7z.exe/7za.exe"
}

$zip = Join-Path $build ("AHKDeck_" + $version + ".zip")
if (Test-Path $zip) { Remove-Item $zip -Force }

Push-Location $stage
try {
    & $sevenZip a -tzip -mx=9 $zip * | Out-Null
    if ($LASTEXITCODE -gt 1) { throw "7z failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}

$entries = [System.IO.Compression.ZipFile]::OpenRead($zip).Entries |
    ForEach-Object { $_.FullName }
$size = (Get-Item $zip).Length
"zip     : $zip"
"version : $version"
"7z      : $sevenZip"
"bytes   : $size"
"entries : $($entries.Count)"
$entries | Sort-Object | ForEach-Object { "  " + $_ }
