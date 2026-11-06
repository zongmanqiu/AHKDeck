# m011 gate: --journal must be a machine-readable event stream.
#
# Why a separate gate: the AI/automation story (requirement 4) consumes stdout,
# not the human layout. One JSON object per line, no ANSI, no prose - a single
# unparsable line makes the whole stream worthless to a reader, so this gate
# parses EVERY line it gets, not a sample.
#
# Checks, on AHKDeck.exe --journal --run <probe>:
#   1 every non-empty stdout line parses as JSON
#   2 the first event is "started" (session id, script, relative log path)
#   3 the boom record is present with severity=fail and its own session id
#   4 the stream ends with "exited" and rc = 2 (the official fatal code)
#   5 the host exit code is 2 as well
#   6 the session's JSONL on disk carries the same record (the log path is real)
#
# ASCII only (PowerShell 5 + GBK console: a Chinese comment here has eaten the
# next ASCII character of a command before).
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Probe
)
$ErrorActionPreference = 'Stop'

$failures = @()
function Add-Failure([string]$msg) { $script:failures += $msg }

if (-not (Test-Path $Exe)) { Write-Output "FAIL: host not built: $Exe"; exit 1 }
if (-not (Test-Path $Probe)) { Write-Output "FAIL: probe dir missing: $Probe"; exit 1 }
$ExeDir = Split-Path -Parent (Resolve-Path $Exe).Path

$script = Join-Path $Probe 'boom.ahk'

# Bounded launch, real pipes. Two reasons for the shape:
#   * the harness must never be what hangs: m011 measured the host failing to
#     exit after a fatal script in roughly 1 run out of 40, and an unbounded wait
#     turned that into ctest's 1500 s timeout. The host gets 20 s, then it is
#     killed and reported - the product defect stays loud, the gate stays fast.
#   * Start-Process -PassThru hands back a Process without query access, so its
#     ExitCode reads as empty ("host exit code = , expected 2"). A .NET Process
#     started with UseShellExecute=false carries the real code.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = '--journal --run "' + $script + '"'
$psi.WorkingDirectory = (Get-Location).Path
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$proc = [System.Diagnostics.Process]::Start($psi)
$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()
if (-not $proc.WaitForExit(20000)) {
    try { $proc.Kill(); [void]$proc.WaitForExit(3000) } catch { }
    Write-Output ('FAIL: the host did not exit within 20 s of --journal --run ' `
        + [System.IO.Path]::GetFileName($script) + ' (killed; see .plan/m012)')
    exit 1
}
$lines = @($outTask.Result -split "\r?\n" | Where-Object { $_ -ne $null -and $_ -ne '' })
$errText = ($errTask.Result -split "\r?\n" | Where-Object { $_ -ne '' }) -join ' / '
$hostRc = $proc.ExitCode
$proc.Dispose()
if ($errText -ne '') { Add-Failure ('stderr must stay empty in journal mode: ' + $errText) }

# 1: parse every line
$parsed = @()
foreach ($line in $lines) {
    if ($line -match '^\s*$') { continue }
    if ($line.Contains([char]27)) { Add-Failure ("ANSI escape inside a journal line: " + $line); continue }
    try {
        $obj = ConvertFrom-Json -InputObject $line
    } catch {
        Add-Failure ("line is not JSON: " + $line)
        continue
    }
    $parsed += ,$obj
}
if ($parsed.Count -lt 3) {
    Add-Failure ("expected at least started + record + exited, got {0} line(s)" -f $parsed.Count)
    Write-Output 'journal output:'
    $lines | ForEach-Object { Write-Output ('    ' + $_) }
    Write-Output ("FAIL: {0}" -f ($failures -join '; '))
    exit 1
}

# 2: started event
$started = $parsed[0]
if ($started.event -ne 'started') { Add-Failure ("first line must be the started event, got: " + ($lines[0])) }
$sessionId = $started.session
if (-not $sessionId) { Add-Failure 'started event has no session id' }
if ($started.script -ne 'boom.ahk') { Add-Failure ("started.script = " + $started.script) }
if ($started.log -notmatch '^Logs\\boom-') { Add-Failure ("started.log = " + $started.log) }
if ($started.log.Contains(':')) { Add-Failure ("log path must stay relative: " + $started.log) }

# 3: the kernel record
$rec = $null
$records = @($parsed | Where-Object { -not $_.event })
if ($records.Count -ne 1) { Add-Failure ("expected exactly one kernel record, got {0}" -f $records.Count) }
else {
    $rec = $records[0]
    if ($rec.message -ne 'm011-boom') { Add-Failure ("record.message = " + $rec.message) }
    if ($rec.severity -ne 'fail') { Add-Failure ("record.severity = " + $rec.severity) }
    if ($rec.phase -ne 'runtime') { Add-Failure ("record.phase = " + $rec.phase) }
    if ($rec.session -ne $sessionId) { Add-Failure ("record.session = {0}, started.session = {1}" -f $rec.session, $sessionId) }
    if (-not $rec.file) { Add-Failure 'record.file is empty (the official dialog always names the file)' }
}

# 4: exited event last
$last = $parsed[-1]
if ($last.event -ne 'exited') { Add-Failure ("stream must end with the exited event, got: " + $lines[-1]) }
elseif ($last.rc -ne 2) { Add-Failure ("exited.rc = {0}, expected 2" -f $last.rc) }
elseif ($last.session -ne $sessionId) { Add-Failure 'exited event names a different session' }

# 5: host exit code
if ($hostRc -ne 2) { Add-Failure ("host exit code = {0}, expected 2" -f $hostRc) }

# 6: the JSONL the stream promised
if ($started.log) {
    $logFull = Join-Path $ExeDir $started.log
    if (-not (Test-Path $logFull)) { Add-Failure ("journal promised a log that is not there: " + $started.log) }
    else {
        $jsonl = @(Get-Content $logFull | Where-Object { $_ -notmatch '^\s*$' })
        if ($jsonl.Count -ne 1) { Add-Failure ("{0} holds {1} record(s), expected 1" -f $started.log, $jsonl.Count) }
        else {
            $fileRec = ConvertFrom-Json -InputObject $jsonl[0]
            if ($fileRec.message -ne 'm011-boom') { Add-Failure ('the file and the stream disagree on the message') }
            if ($fileRec.session -ne $sessionId) { Add-Failure ('the file record lost its session id') }
            if ($rec -and $rec.thread -ne $fileRec.thread) { Add-Failure ('the file and the stream disagree on the thread') }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Output 'journal output:'
    $lines | ForEach-Object { Write-Output ('    ' + $_) }
    Write-Output ''
    foreach ($f in $failures) { Write-Output ('  FAIL: ' + $f) }
    exit 1
}

Write-Output ('OK: {0} journal line(s) parsed, session {1}, rc {2}, log {3}' -f $parsed.Count, $sessionId, $hostRc, $started.log)
exit 0
