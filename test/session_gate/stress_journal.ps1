# m012 stress harness - does the host always exit after a fatal script?
#
# Why this exists: the shipped journal gate bounds the host at 20 s, which is
# enough to keep ctest fast but far too coarse to measure a rare (~1 in 40)
# "host writes the record, then never exits" hang. This script runs the same
# command N times, times every run, and keeps a ledger. When a run exceeds the
# bound it dumps the thread list (state, wait reason, start address mapped to
# module+offset) BEFORE killing, because that process is the only witness.
#
# The tool proves itself first: -SelfTest runs a script that MUST stay resident
# (long.ahk) and fails if any run appears to exit - a hang detector that cannot
# see a hang is worse than none.
#
# ASCII only: PowerShell 5 on a GBK console has eaten the next ASCII character of
# a command after a Chinese comment here before.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Probe,
    [int]$Runs = 200,
    [int]$BoundMs = 4000,
    [string]$ScriptName = 'boom.ahk',
    [string]$ExtraArgs = '',
    [string]$Ledger = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Output ('FAIL: host not built: ' + $Exe); exit 1 }
$target = Join-Path $Probe $ScriptName
if (-not (Test-Path $target)) { Write-Output ('FAIL: probe script missing: ' + $target); exit 1 }
if ($Ledger -eq '') {
    $Ledger = Join-Path $env:TEMP ('ahkdeck-m012-' + $ScriptName.Replace('.ahk', '') + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.tsv')
}

function Get-ThreadDump([System.Diagnostics.Process]$Proc) {
    # Module map of the hung process: a thread blocked in AHK_H's own teardown and
    # one blocked in the CRT look identical without this.
    $mods = @()
    try { $mods = @($Proc.Modules) } catch { }
    $out = @()
    try {
        foreach ($t in $Proc.Threads) {
            $where = ''
            try {
                $addr = [Int64]$t.StartAddress
                foreach ($m in $mods) {
                    $base = [Int64]$m.BaseAddress
                    if ($addr -ge $base -and $addr -lt ($base + [Int64]$m.ModuleMemorySize)) {
                        $where = [System.IO.Path]::GetFileName($m.ModuleName) + '+0x' + ('{0:x}' -f ($addr - $base))
                        break
                    }
                }
            } catch { }
            if ($where -eq '') { $where = '0x' + ('{0:x}' -f [Int64]$t.StartAddress) }
            $out += ('tid={0} state={1} wait={2} start={3}' -f $t.Id, $t.ThreadState, $t.WaitReason, $where)
        }
    } catch { $out += ('thread dump failed: ' + $_.Exception.Message) }
    return ($out -join '; ')
}

# One run: real pipes, bounded wait, diagnostics before the kill.
function Invoke-OneRun {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $argline = '--journal --run "' + $target + '"'
    if ($ExtraArgs -ne '') { $argline += ' ' + $ExtraArgs }
    $psi.Arguments = $argline
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $exited = $p.WaitForExit($BoundMs)
    $ms = $sw.ElapsedMilliseconds
    if (-not $exited) {
        $dump = Get-ThreadDump $p
        try { $p.Kill(); [void]$p.WaitForExit(5000) } catch { }
        return @{ exited = $false; ms = $ms; rc = $null; lines = 0; dump = $dump }
    }
    $p.WaitForExit()
    $rc = $p.ExitCode
    $text = $outTask.Result
    $errText = $errTask.Result
    $p.Dispose()
    $lineCount = @($text -split "\r?\n" | Where-Object { $_ -ne '' }).Count
    return @{ exited = $true; ms = $ms; rc = $rc; lines = $lineCount; dump = ('stderr=' + ($errText -replace "\r?\n", ' / ')) }
}

$hangs = 0
$wrongRc = 0
$wrongStream = 0
$slowest = 0
$rows = @("run`texit_ms`texited`trc`tlines`tnote")

function New-Row([int]$Index, $Ms, [string]$Exited, $Rc, $Lines, [string]$Note) {
    # Single-quoted strings do not expand backtick-t, so the tabs are built here
    # explicitly - a ledger whose columns are literal "`t" is not a ledger.
    return ('{0}' -f $Index) + "`t" + ('{0}' -f $Ms) + "`t" + $Exited + "`t" + ('{0}' -f $Rc) + "`t" + ('{0}' -f $Lines) + "`t" + $Note
}

for ($i = 1; $i -le $Runs; $i++) {
    $r = Invoke-OneRun
    if ($r.exited) {
        if ($r.ms -gt $slowest) { $slowest = $r.ms }
        if ($SelfTest) {
            $rows += (New-Row $i $r.ms 'yes' $r.rc $r.lines '')
        }
        elseif ($r.rc -ne 2) {
            # the shape under test: a fatal script must give rc 2 and exactly
            # started + record + exited
            $wrongRc++; $rows += (New-Row $i $r.ms 'yes' $r.rc $r.lines 'BAD_RC')
        }
        elseif ($r.lines -ne 3) {
            $wrongStream++; $rows += (New-Row $i $r.ms 'yes' $r.rc $r.lines 'BAD_STREAM')
        }
        else {
            $rows += (New-Row $i $r.ms 'yes' $r.rc $r.lines '')
        }
    }
    else {
        $hangs++
        $rows += (New-Row $i $r.ms 'NO' '?' '?' ('HUNG ' + $r.dump))
        Write-Output ('HUNG run ' + $i + ': ' + $r.dump)
    }
    if ($i % 50 -eq 0) { Write-Output ('  ... ' + $i + '/' + $Runs + ' runs, hangs=' + $hangs) }
}

Set-Content -Path $Ledger -Value ($rows -join "`r`n") -Encoding ASCII

if ($SelfTest) {
    # long.ahk is resident by design: every single run MUST be reported as not
    # exiting. Anything else means this harness is blind.
    if ($hangs -lt $Runs) {
        Write-Output ('SELFTEST FAIL: resident script reported ' + $hangs + ' hangs out of ' + $Runs)
        Write-Output ('ledger: ' + $Ledger)
        exit 1
    }
    Write-Output ('SELFTEST OK: ' + $hangs + '/' + $Runs + ' resident runs detected as not exiting (bound ' + $BoundMs + ' ms); ledger ' + $Ledger)
    exit 0
}

if ($wrongRc -ne 0) { Write-Output ('FAIL: wrong exit code in ' + $wrongRc + ' run(s), expected 2') }
if ($wrongStream -ne 0) { Write-Output ('FAIL: journal stream not 3 lines in ' + $wrongStream + ' run(s)') }
if ($hangs -eq 0 -and $wrongRc -eq 0 -and $wrongStream -eq 0) {
    Write-Output ('CLEAN: ' + $Runs + ' runs, 0 hangs, 0 bad rc, 0 bad stream, slowest ' + $slowest + ' ms; ledger ' + $Ledger)
    exit 0
}
Write-Output ('LEDGER: ' + $Ledger)
Write-Output ('RESULT: runs=' + $Runs + ' hangs=' + $hangs + ' bad_rc=' + $wrongRc + ' bad_stream=' + $wrongStream)
exit 1
