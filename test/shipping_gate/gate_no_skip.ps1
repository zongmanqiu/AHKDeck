# m195: a green ctest summary is NOT proof that conditional branches ran.
# test_execompiler.cpp prints "AHKDECK_SKIP:" when its end-to-end arms cannot
# be staged; the old gate reported 21/21 while those arms never executed.
# 5_test.bat captures `ctest -V` into a log and this gate audits that log.
#
# The audit also refuses an empty log or one with no ctest summary, so a broken
# redirection cannot masquerade as "no skips found".
param(
    [string]$Log = "",
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$marker = 'AHKDECK_SKIP:'

function Invoke-Audit([string]$path) {
    $lines = @(Get-Content -LiteralPath $path)
    $skips = @($lines | Where-Object { $_.Contains($marker) })
    $summaryLines = @($lines | Where-Object { $_ -match 'tests passed|tests failed' })
    $summary = if ($summaryLines.Count -gt 0) { $summaryLines[-1].Trim() } else { '' }
    $fails = New-Object System.Collections.Generic.List[string]
    if ($lines.Count -eq 0) { [void]$fails.Add('log is empty') }
    if ($summary -eq '') { [void]$fails.Add('no ctest summary line') }
    foreach ($s in $skips) { [void]$fails.Add('skipped branch: ' + $s.Trim()) }
    return [pscustomobject]@{ Fails = $fails; Summary = $summary; Lines = $lines.Count }
}

function Write-Sample([string]$path, [string[]]$body) {
    [System.IO.File]::WriteAllText($path, ($body -join "`n") + "`n")
}

if ($SelfTest) {
    $dir = Join-Path $env:TEMP ('ahkdeck_skip_selftest_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    $summary = '100% tests passed, 0 tests failed out of 21'
    try {
        $bad = Join-Path $dir 'bad.log'
        $good = Join-Path $dir 'good.log'
        $empty = Join-Path $dir 'empty.log'
        Write-Sample $bad @('1/1 Test #12: execompiler .... Passed', $marker + ' end-to-end green arm', $summary)
        Write-Sample $good @('1/1 Test #12: execompiler .... Passed', 'ok   end-to-end green arm', $summary)
        Write-Sample $empty @('just a stray line')

        $rBad = Invoke-Audit $bad
        $rGood = Invoke-Audit $good
        $rEmpty = Invoke-Audit $empty
        $ok = ($rBad.Fails.Count -gt 0) -and ($rGood.Fails.Count -eq 0) -and ($rEmpty.Fails.Count -gt 0)
        Write-Output ("selftest bad_sample_fails={0} good_sample_fails={1} empty_log_fails={2} ok={3}" -f
            $rBad.Fails.Count, $rGood.Fails.Count, $rEmpty.Fails.Count, $ok)
        if (-not $ok) { exit 1 }
        exit 0
    }
    finally {
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path -LiteralPath $Log)) {
    Write-Output ('FAIL: ctest log not found: ' + $Log)
    exit 1
}
$result = Invoke-Audit $Log
if ($result.Fails.Count -gt 0) {
    foreach ($f in $result.Fails) { Write-Output ('FAIL: ' + $f) }
    exit 1
}
Write-Output ('ok   no skipped branches ({0} log lines) - {1}' -f $result.Lines, $result.Summary)
exit 0
