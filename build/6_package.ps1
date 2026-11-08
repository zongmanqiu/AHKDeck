# 6_package.ps1 - build the portable release zip from .build, whitelist only.
#
# The folder a user unzips must contain exactly what the release advertises:
#   AHKDeck.exe, AutoHotkey64.dll, Scripts\ (samples), docs\ (the MCP access
#   doc), licenses\ (upstream license texts), LICENSE, THIRD-PARTY-NOTICES.md.
# Never: settings.json, Logs\, update.cache.json, .temp\, ahkdeck_backup\, the
# installer bat. Those are per-machine state, not the product.
#
# Usage (from anywhere; paths are derived from this script):
#   powershell -ExecutionPolicy Bypass -File .main\build\6_package.ps1

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$build = Join-Path $root ".build"
$temp = Join-Path $root ".temp"
$srcDocs = Join-Path $root ".main\docs"
$srcLicenses = Join-Path $root ".main\licenses"

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

$items = @("AHKDeck.exe", "AutoHotkey64.dll", "LICENSE", "THIRD-PARTY-NOTICES.md", "Scripts")
foreach ($item in $items) {
    $src = Join-Path $build $item
    if (-not (Test-Path $src)) {
        if ($item -eq "THIRD-PARTY-NOTICES.md" -or $item -eq "LICENSE") { continue }
        throw "missing release item: $item"
    }
    Copy-Item $src -Destination $stage -Recurse -Force
}

# docs: taken from source so the zip always matches the shipped text.
if (Test-Path $srcDocs) {
    $dstDocs = Join-Path $stage "docs"
    New-Item -ItemType Directory -Path $dstDocs -Force | Out-Null
    Copy-Item (Join-Path $srcDocs "*") -Destination $dstDocs -Force
}

# licenses: upstream license originals travel with the program (GPL-2.0).
if (Test-Path $srcLicenses) {
    $dstLicenses = Join-Path $stage "licenses"
    New-Item -ItemType Directory -Path $dstLicenses -Force | Out-Null
    Copy-Item (Join-Path $srcLicenses "*") -Destination $dstLicenses -Force
}

$zip = Join-Path $build ("AHKDeck_" + $version + ".zip")
if (Test-Path $zip) { Remove-Item $zip -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)

$entries = [System.IO.Compression.ZipFile]::OpenRead($zip).Entries |
    ForEach-Object { $_.FullName }
$size = (Get-Item $zip).Length
"zip     : $zip"
"version : $version"
"bytes   : $size"
"entries : $($entries.Count)"
$entries | Sort-Object | ForEach-Object { "  " + $_ }
