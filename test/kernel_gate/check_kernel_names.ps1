# m004 phase C gate: block AutoHotkey_H-only built-in names in the AHK code we ship.
#
# Why: those names do not exist in official AutoHotkey 2.0.30, so a script that
# uses one of them silently stops being "runs exactly like official AutoHotkey".
# The list is generated from the phase A API diff (see ahkh_only_names.txt).
#
# Scope: every *.ahk under .main\scripts and .build\Scripts, plus any *.ahk under
# .main outside legacy\ and test\kernel_gate\ (test corpus is allowed to poke at
# the differences - that is what it is for).
#
# Exit code 1 on any hit, so 5_test.bat can gate the build on it.
# ASCII only (PowerShell 5 + GBK console safety).

param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
)
$ErrorActionPreference = 'Stop'

$listPath = Join-Path $PSScriptRoot 'ahkh_only_names.txt'
$names = @(
    Get-Content $listPath |
        Where-Object { $_ -notmatch '^\s*(#|$)' } |
        ForEach-Object { $_.Trim() }
)
if ($names.Count -eq 0) { throw "empty name list: $listPath" }

# One alternation, longest first so "UnZipRawMemory" is not shadowed by "UnZip".
$sorted = ($names | Sort-Object { $_.Length } -Descending)
$pattern = '(?i)(?<![A-Za-z0-9_$])(' + ($sorted -join '|') + ')(?![A-Za-z0-9_#])'

$files = New-Object System.Collections.Generic.List[string]
foreach ($dir in @((Join-Path $Root '.main\scripts'), (Join-Path $Root '.build\Scripts'))) {
    if (Test-Path $dir) {
        foreach ($f in Get-ChildItem $dir -Recurse -Filter '*.ahk' -ErrorAction SilentlyContinue) { [void]$files.Add($f.FullName) }
    }
}
foreach ($f in Get-ChildItem (Join-Path $Root '.main') -Recurse -Filter '*.ahk' -ErrorAction SilentlyContinue) {
    $rel = $f.FullName.Substring($Root.Length).Replace('/', '\')
    if ($rel -like '*\legacy\*' -or $rel -like '*\test\kernel_gate\*') { continue }
    if (-not $files.Contains($f.FullName)) { [void]$files.Add($f.FullName) }
}

$hits = New-Object System.Collections.Generic.List[string]
foreach ($f in $files) {
    $lineNo = 0
    foreach ($line in Get-Content $f) {
        $lineNo++
        if ($line -match '^\s*;') { continue }   # whole-line comment: harmless
        foreach ($m in [regex]::Matches($line, $pattern)) {
            $rel = $f.Substring($Root.Length)
            [void]$hits.Add(('{0}:{1}: {2}' -f $rel, $lineNo, $m.Value))
        }
    }
}

if ($hits.Count -gt 0) {
    Write-Output 'AHK_H-only names found in shipped AHK code:'
    foreach ($h in $hits) { Write-Output ('  ' + $h) }
    Write-Output ''
    Write-Output ("FAIL: {0} hit(s). Use the official AutoHotkey equivalent instead." -f $hits.Count)
    exit 1
}

Write-Output ('OK: {0} AHK_H-only names, {1} AHK file(s) scanned, 0 hits' -f $names.Count, $files.Count)
exit 0
