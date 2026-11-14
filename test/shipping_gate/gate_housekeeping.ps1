# m109/m177 shipping gate: the startup cleanup of temp\.
#
# The real exe runs in a throwaway copy (its own exe directory), so the user's
# .build data is never touched. m177 collapsed the four per-machine directories
# into one temp\ root, so the gate now proves the whole tree is cleaned by one
# pass - not one pass per directory name.
#
# Four phases:
#   1. fresh settings.json gains housekeeping.max_age_days = 30 (the setting the
#      options window writes), and startup creates temp\ + its four subdirectories
#   2. with 30 days: a 40-day-old file is deleted from logs\ / backups\ / work\
#      and deleted\, a 1-day-old file stays, and an emptied subfolder is removed
#   3. max_age_days = 0 (never) turns the cleanup off
#   4. max_age_days = 180 keeps a 40-day-old file - the window is the setting,
#      not a fixed 30
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exe = Join-Path $root '.build\AHKDeck.exe'
# m192 P4: the throwaway copy mirrors the shipped layout - ONE kernel EXE, no
# DLL. The cleanup gate never runs a script, but a stale DLL here would make the
# sample layout lie about what a user actually unzips.
$kernel = Join-Path $root '.build\AutoHotkey64.exe'
$work = Join-Path $root '.temp\m109_gate'
$run = Join-Path $work 'run'

$fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Output ("ok   " + $what) }
    else { Write-Output ("FAIL " + $what); $script:fails++ }
}

function Reset-Run {
    if (Test-Path $run) { Remove-Item $run -Recurse -Force }
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item $exe (Join-Path $run 'AHKDeck.exe')
    Copy-Item $kernel (Join-Path $run 'AutoHotkey64.exe')
}

# A file whose last-write time is $days days in the past.
function New-OldFile([string]$path, [int]$days) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    Set-Content -Path $path -Value 'm177 gate' -Encoding ASCII
    (Get-Item $path).LastWriteTime = (Get-Date).AddDays(-$days)
}

function Start-Deck {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $run 'AHKDeck.exe'
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $run
    return [System.Diagnostics.Process]::Start($psi)
}

function Stop-Deck([int]$procId) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $run 'AHKDeck.exe'
    $psi.Arguments = "--quit $procId"
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $run
    [void][System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 2
}

function Write-Days([int]$days) {
    $json = '{ "housekeeping": { "max_age_days": ' + $days + ' } }'
    Set-Content -Path (Join-Path $run 'settings.json') -Value $json -Encoding UTF8
}

# --- phase 1: layout on first run + the knob --------------------------------
Reset-Run
$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id
$settingsPath = Join-Path $run 'settings.json'
Check (Test-Path $settingsPath) "settings.json written"
$cfg = Get-Content $settingsPath -Raw | ConvertFrom-Json
Check ($null -ne $cfg.housekeeping) "settings.json has a housekeeping section"
Check ($cfg.housekeeping.max_age_days -eq 30) "the default is 30 days"
foreach ($sub in @('', 'logs', 'backups', 'work', 'deleted')) {
    $rel = 'temp'
    if ($sub) { $rel = "temp\$sub" }
    Check (Test-Path (Join-Path $run $rel)) ("startup creates " + $rel)
}

# --- phase 2: 30 days cleans the whole tree in one pass ---------------------
Reset-Run
Write-Days 30
$old = @(
    'temp\logs\old-session.jsonl',
    'temp\backups\2026-09-01\old.ahk',
    'temp\work\old-editor.ahk',
    'temp\deleted\old-script-20260901.ahk'
)
$new = @(
    'temp\logs\new-session.jsonl',
    'temp\backups\2026-11-07\new.ahk',
    'temp\work\ai-editor.ahk',
    'temp\deleted\new-script-20261113.ahk'
)
foreach ($rel in $old) { New-OldFile (Join-Path $run $rel) 40 }
foreach ($rel in $new) { New-OldFile (Join-Path $run $rel) 1 }

$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id

foreach ($rel in $old) {
    Check (-not (Test-Path (Join-Path $run $rel))) ($rel + ": a 40-day-old file is deleted")
}
foreach ($rel in $new) {
    Check (Test-Path (Join-Path $run $rel)) ($rel + ": a 1-day-old file stays")
}
Check (-not (Test-Path (Join-Path $run 'temp\backups\2026-09-01'))) "temp\backups: the emptied folder is removed"
Check (Test-Path (Join-Path $run 'temp')) "temp itself is kept"
$cfg = Get-Content $settingsPath -Raw | ConvertFrom-Json
Check ($cfg.housekeeping.max_age_days -eq 30) "the setting survives a launch"

# --- phase 3: max_age_days = 0 means never clean ----------------------------
Reset-Run
Write-Days 0
$oldLog = Join-Path $run 'temp\logs\old-session.jsonl'
New-OldFile $oldLog 40
$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id
Check (Test-Path $oldLog) "0 days (never): nothing is deleted"

# --- phase 4: the window is the setting, not a fixed 30 --------------------
Reset-Run
Write-Days 180
$oldLog = Join-Path $run 'temp\logs\old-session.jsonl'
New-OldFile $oldLog 40
$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id
Check (Test-Path $oldLog) "180 days: a 40-day-old file is kept"

if ($fails -gt 0) { Write-Output ("m177 gate: " + $fails + " failure(s)"); exit 1 }
Write-Output "m177 gate: all checks passed"
exit 0
