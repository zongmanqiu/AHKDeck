# m109 shipping gate: the startup cleanup of .temp\ / Backups\ / Logs\.
#
# The real exe runs in a throwaway copy (its own exe directory), so the user's
# .build data is never touched. Three phases:
#   1. fresh settings.json gains housekeeping.max_age_days = 30 (the discoverable
#      knob for advanced users; there is no UI control for it)
#   2. with 30 days, a 40-day-old file is deleted from all three directories,
#      a 1-day-old file stays, and an emptied timestamp subfolder is removed
#   3. max_age_days = 0 turns the cleanup off: a 40-day-old file survives
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exe = Join-Path $root '.build\AHKDeck.exe'
$dll = Join-Path $root '.build\AutoHotkey64.dll'
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
    Copy-Item $dll (Join-Path $run 'AutoHotkey64.dll')
    New-Item -ItemType Directory -Force (Join-Path $run '.temp') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $run 'Backups') | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $run 'Logs') | Out-Null
}

# A file whose last-write time is $days days in the past.
function New-OldFile([string]$path, [int]$days) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    Set-Content -Path $path -Value 'm109 gate' -Encoding ASCII
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

# --- phase 1: a fresh settings.json gains the knob --------------------------
Reset-Run
$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id
$settingsPath = Join-Path $run 'settings.json'
Check (Test-Path $settingsPath) "settings.json written"
$cfg = Get-Content $settingsPath -Raw | ConvertFrom-Json
Check ($null -ne $cfg.housekeeping) "settings.json has a housekeeping section"
Check ($cfg.housekeeping.max_age_days -eq 30) "the default is 30 days"

# --- phase 2: 30 days cleans the three directories ---------------------------
Reset-Run
Set-Content -Path (Join-Path $run 'settings.json') -Value '{ "housekeeping": { "max_age_days": 30 } }' -Encoding UTF8
$oldTemp = Join-Path $run '.temp\old-editor.ahk'
$newTemp = Join-Path $run '.temp\ai-editor.ahk'
$oldBackup = Join-Path $run 'Backups\2026-09-01\old.ahk'
$newBackup = Join-Path $run 'Backups\2026-11-07\new.ahk'
$oldLog = Join-Path $run 'Logs\old-session.jsonl'
$newLog = Join-Path $run 'Logs\new-session.jsonl'
New-OldFile $oldTemp 40
New-OldFile $newTemp 1
New-OldFile $oldBackup 40
New-OldFile $newBackup 1
New-OldFile $oldLog 40
New-OldFile $newLog 1

$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id

Check (-not (Test-Path $oldTemp)) ".temp: a 40-day-old file is deleted"
Check (Test-Path $newTemp) ".temp: a 1-day-old file stays"
Check (-not (Test-Path $oldBackup)) "Backups: a 40-day-old file is deleted"
Check (-not (Test-Path (Join-Path $run 'Backups\2026-09-01'))) "Backups: the emptied folder is removed"
Check (Test-Path $newBackup) "Backups: a 1-day-old file stays"
Check (-not (Test-Path $oldLog)) "Logs: a 40-day-old file is deleted"
Check (Test-Path $newLog) "Logs: a 1-day-old file stays"
Check (Test-Path (Join-Path $run '.temp')) "the directories themselves are kept"
$cfg = Get-Content $settingsPath -Raw | ConvertFrom-Json
Check ($cfg.housekeeping.max_age_days -eq 30) "the setting survives a launch"

# --- phase 3: max_age_days = 0 means never clean -----------------------------
Reset-Run
Set-Content -Path (Join-Path $run 'settings.json') -Value '{ "housekeeping": { "max_age_days": 0 } }' -Encoding UTF8
$oldTemp = Join-Path $run '.temp\old-editor.ahk'
New-OldFile $oldTemp 40
$p = Start-Deck
Start-Sleep -Seconds 4
Stop-Deck $p.Id
Check (Test-Path $oldTemp) "0 days: nothing is deleted"

if ($fails -gt 0) { Write-Output ("m109 gate: " + $fails + " failure(s)"); exit 1 }
Write-Output "m109 gate: all checks passed"
exit 0
