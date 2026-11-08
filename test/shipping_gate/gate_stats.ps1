# m107 gate: --stats must be a one-line, machine-readable resource reading.
#
# The status bar shows "CPU 3% . memory 52MB . threads 12 . handles 231 . GPU 1%".
# A drawn label cannot be checked by a machine, so the same ResourceSampler that
# feeds the label also feeds this CLI mode, and the gate reads the shipped exe.
# It proves three things a unit test cannot:
#   1 psapi / Toolhelp / PDH really run on a live process (numbers are sane);
#   2 the CLI path prints JSON to the console it was started from;
#   3 no window and no dialog is involved (the process exits on its own).
#
# ASCII only: PowerShell 5 on a GBK console has eaten ASCII characters after
# non-ASCII comments in this project before. The Chinese labels of the status
# line are checked by test_resources (C++ source, /utf-8), not here.
param(
    [Parameter(Mandatory = $true)][string]$Exe
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Output "FAIL: host not built: $Exe"; exit 1 }

$failures = @()
function Add-Failure([string]$m) { $script:failures += $m }

$stdout = ''
$stderr = ''
$rc = -1
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = '--stats'
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(15000)) {
        try { $p.Kill(); [void]$p.WaitForExit(3000) } catch { }
        Add-Failure '--stats did not finish in 15 s'
    } else {
        $p.WaitForExit()
        $rc = $p.ExitCode
        $stdout = $outTask.Result
        $stderr = $errTask.Result
    }
    $p.Dispose()
} catch { Add-Failure ('--stats run failed: ' + $_.Exception.Message) }

if ($rc -ne 0) { Add-Failure ("--stats exit code = $rc, expected 0") }
if ($stderr -ne '') { Add-Failure ('stderr must stay empty in --stats mode: ' + $stderr) }

$lines = @($stdout -split "\r?\n" | Where-Object { $_ -ne '' })
if ($lines.Count -ne 1) {
    Add-Failure ("expected exactly one line of JSON, got {0}" -f $lines.Count)
} else {
    $obj = $null
    try { $obj = ConvertFrom-Json -InputObject $lines[0] }
    catch { Add-Failure ('--stats line is not JSON: ' + $lines[0]) }
    if ($obj -ne $null) {
        if ($obj.ok -ne $true) { Add-Failure ('ok = ' + $obj.ok) }

        $cpu = [double]$obj.cpu_percent
        if ($cpu -lt 0 -or $cpu -gt 100) { Add-Failure ("cpu_percent out of range: $cpu") }

        # A running AHKDeck process holds at least a kernel DLL and the FLTK
        # window; under 1 MB would mean psapi returned garbage.
        $ws = [int64]$obj.working_set_bytes
        if ($ws -lt 1048576) { Add-Failure ("working_set_bytes too small: $ws") }
        if ([int64]$obj.private_bytes -lt 0) { Add-Failure ('private_bytes is negative') }

        # pid lets anyone confirm these numbers belong to this process.
        if ([int]$obj.pid -le 0) { Add-Failure ('pid = ' + $obj.pid) }

        # Task Manager's Memory column is the private working set, which is a
        # subset of the total: it must be sane and never exceed the total.
        $pws = [int64]$obj.private_working_set_bytes
        if ($pws -lt 1048576) { Add-Failure ("private_working_set_bytes too small: $pws") }
        if ($pws -gt $ws) { Add-Failure ("private_working_set_bytes $pws exceeds working_set_bytes $ws") }

        if ([int]$obj.threads -lt 1) { Add-Failure ('threads = ' + $obj.threads) }
        if ([int]$obj.handles -lt 1) { Add-Failure ('handles = ' + $obj.handles) }

        # -1 means "this machine exposes no GPU counter"; anything else is a
        # percentage and cannot exceed 100 (the sampler clamps it).
        $gpu = [double]$obj.gpu_percent
        if ($gpu -lt -1 -or $gpu -gt 100) { Add-Failure ("gpu_percent out of range: $gpu") }

        $text = [string]$obj.text
        if ($text -notmatch '^CPU ') { Add-Failure ('text does not start with the CPU segment: ' + $text) }
        if ($text -notmatch '(KB|MB|GB)') { Add-Failure ('text carries no byte unit: ' + $text) }
        if ($gpu -ge 0 -and $text -notmatch 'GPU') { Add-Failure ('text omits a GPU reading that exists: ' + $text) }
        if ($gpu -lt 0 -and $text -match 'GPU') { Add-Failure ('text shows a GPU reading that does not exist: ' + $text) }

        Write-Output ('reading: ' + $lines[0])
    }
}

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Output ('  FAIL: ' + $f) }
    exit 1
}
Write-Output 'OK: --stats printed one sane JSON line from the shipped exe'
exit 0
