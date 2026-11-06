# m004 gate phase A: API surface diff (runtime probe, not source inference).
# Candidate names are harvested from each kernel's source tables (g_BIF / g_BIV_A),
# then VERIFIED AT RUNTIME on both engines by dynamic dereference. A name counts as
# "present" only if the engine actually resolves it.
# Reference is OFFICIAL AUTOHOTKEY 2.0.25 - the same version as our pinned kernel.
# Capability parity is only meaningful same-version-to-same-version; comparing
# against a newer official build would flag new upstream features as "missing".
# ASCII only (PowerShell 5 + GBK console safety).
param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
)
$ErrorActionPreference = 'Stop'

$srcOff = Join-Path $Root '.3rd\AutoHotkey-v2.0.25-src\source\script.cpp'
$srcH   = Join-Path $Root '.3rd\AutoHotkey_H-v2.0.25\source\script.cpp'
$work   = Join-Path $Root '.temp\m004\api'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$workOff = Join-Path $work 'off'
$workH   = Join-Path $work 'h'
New-Item -ItemType Directory -Force -Path $workOff, $workH | Out-Null

$engines = [ordered]@{
    off = Join-Path $Root '.3rd\AutoHotkey_2.0.25\AutoHotkey64.exe'
    h   = Join-Path $Root '.temp\ahkh_build\AutoHotkey64.exe'
}

function Get-Block([string]$text, [string]$pattern) {
    $m = [regex]::Match($text, $pattern, 'Singleline')
    if (-not $m.Success) { throw "table not found: $pattern" }
    return $m.Groups[1].Value
}

function Get-FuncNames([string]$path) {
    $t = [IO.File]::ReadAllText($path)
    $body = Get-Block $t 'FuncEntry\s+g_BIF\[\]\s*=\s*\{(.*?)\n\};'
    $names = [regex]::Matches($body, '(?m)^\s*BIF[ni1]\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)') | ForEach-Object { $_.Groups[1].Value }
    return $names
}

function Get-VarNames([string]$path) {
    $t = [IO.File]::ReadAllText($path)
    $body = Get-Block $t 'VarEntry\s+g_BIV_A\[\]\s*=\s*\{(.*?)\n\};'
    $names = [regex]::Matches($body, '(?m)^\s*A_[wx]+\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)') | ForEach-Object { 'A_' + $_.Groups[1].Value }
    return $names
}

$offFuncs = Get-FuncNames $srcOff
$hFuncs   = Get-FuncNames $srcH
$offVars  = Get-VarNames $srcOff
$hVars    = Get-VarNames $srcH

$all = @($offFuncs + $hFuncs + $offVars + $hVars) | Sort-Object -Unique

Write-Output ("candidates: offFuncs={0} hFuncs={1} offVars={2} hVars={3} union={4}" -f $offFuncs.Count, $hFuncs.Count, $offVars.Count, $hVars.Count, $all.Count)

$list = 'names := [' + (($all | ForEach-Object { '"' + $_ + '"' }) -join ',') + ']'
$probe = @"
#Requires AutoHotkey v2.0
$list
out := A_ScriptDir "\surface.txt"
r := ""
for name in names
{
    try
    {
        v := %name%
        if (Type(v) = "Func")
            r .= name "``tFunc``t" v.MinParams "``t" v.MaxParams "``t" (v.IsVariadic ? 1 : 0) "``n"
        else
            r .= name "``t" Type(v) "``n"
    }
    catch as e
    {
        r .= name "``tERROR``t" e.Message "``n"
    }
}
FileAppend r, out, "UTF-8"
"@
[IO.File]::WriteAllText((Join-Path $workOff 'probe_surface.ahk'), $probe, (New-Object System.Text.UTF8Encoding($false)))
[IO.File]::WriteAllText((Join-Path $workH 'probe_surface.ahk'), $probe, (New-Object System.Text.UTF8Encoding($false)))

foreach ($k in $engines.Keys) {
    $wd = if ($k -eq 'off') { $workOff } else { $workH }
    $runner = Join-Path $wd 'run_wrap.ps1'
    $exe = $engines[$k]
    $args = @($exe, (Join-Path $wd 'probe_surface.ahk'))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = '/ErrorStdOut=UTF-8 "' + (Join-Path $wd 'probe_surface.ahk') + '"'
    $psi.WorkingDirectory = $wd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $done = $p.WaitForExit(30000)
    if (-not $done) { try { $p.Kill() } catch {}; throw "probe timed out: $k" }
    $se = $p.StandardError.ReadToEnd()
    Write-Output ("engine={0} rc={1} stderr={2}" -f $k, $p.ExitCode, ($se -replace "`r`n", ' | '))
}

$map = @{}
foreach ($k in $engines.Keys) {
    $wd = if ($k -eq 'off') { $workOff } else { $workH }
    $f = Join-Path $wd 'surface.txt'
    if (-not (Test-Path $f)) { throw "no surface.txt for $k" }
    $d = @{}
    foreach ($line in [IO.File]::ReadAllLines($f)) {
        if ($line -eq '') { continue }
        $parts = $line -split "`t"
        $val = $parts[1..($parts.Count - 1)] -join '/'
        # An unresolvable name is reported by the probe as an error, not as a
        # missing row: treat "Variable not found" as ABSENT so AHKH_ONLY /
        # OFFICIAL_ONLY are named correctly instead of landing in TYPE_DIFF.
        if ($val -like 'ERROR/Variable not found.*') { $val = 'MISSING' }
        $d[$parts[0]] = $val
    }
    $map[$k] = $d
}

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add("name,official,ahkh,verdict")
foreach ($n in $all) {
    $o = if ($map['off'].ContainsKey($n)) { $map['off'][$n] } else { 'MISSING' }
    $h = if ($map['h'].ContainsKey($n))   { $map['h'][$n] }   else { 'MISSING' }
    $v = 'SAME'
    if ($o -eq $h) { $v = 'SAME' }
    elseif ($o -eq 'MISSING') { $v = 'AHKH_ONLY' }
    elseif ($h -eq 'MISSING') { $v = 'OFFICIAL_ONLY' }
    elseif (($o -split '/')[0] -eq ($h -split '/')[0]) { $v = 'SIG_DIFF' }
    else { $v = 'TYPE_DIFF' }
    $rows.Add(('"' + $n + '","' + $o + '","' + $h + '",' + $v))
}
[IO.File]::WriteAllLines((Join-Path $work 'api_diff.csv'), $rows, (New-Object System.Text.UTF8Encoding($false)))

$O = $rows | Select-Object -Skip 1 | ForEach-Object { $_ -split ',' }
$counts = @{}
foreach ($v in 'SAME','OFFICIAL_ONLY','AHKH_ONLY','SIG_DIFF','TYPE_DIFF') { $counts[$v] = 0 }
foreach ($row in ($rows | Select-Object -Skip 1)) {
    $v = $row.Substring($row.LastIndexOf(',') + 1)
    $counts[$v]++
}
Write-Output ("RESULT " + (($counts.Keys | ForEach-Object { $_ + '=' + $counts[$_] }) -join ' '))
$offOnly = $rows | Select-Object -Skip 1 | Where-Object { $_ -like '*OFFICIAL_ONLY' }
if ($offOnly) { Write-Output '--- OFFICIAL_ONLY ---'; $offOnly | ForEach-Object { Write-Output $_ } }
$ahOnly = $rows | Select-Object -Skip 1 | Where-Object { $_ -like '*AHKH_ONLY' }
if ($ahOnly) { Write-Output '--- AHKH_ONLY ---'; $ahOnly | ForEach-Object { Write-Output $_ } }
$sig = $rows | Select-Object -Skip 1 | Where-Object { $_ -like '*SIG_DIFF*' -or $_ -like '*TYPE_DIFF*' }
if ($sig) { Write-Output '--- SIG/TYPE_DIFF ---'; $sig | ForEach-Object { Write-Output $_ } }
Write-Output ('CSV -> ' + (Join-Path $work 'api_diff.csv'))
