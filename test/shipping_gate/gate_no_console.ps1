# m013 shipping gate - the product exe must not be a console-subsystem image.
#
# Why a machine check instead of a comment: rules-standing.md 13 says double
# clicking AHKDeck.exe must not open a terminal. That property lives in one byte
# of the PE header (IMAGE_OPTIONAL_HEADER.Subsystem: 2 = WINDOWS GUI, 3 = CONSOLE),
# and nothing in the build fails if someone drops WIN32 from add_executable - the
# app would still work, still print from a console, and open a black window on
# every double-click. Only a reading of the shipped file can hold that line.
#
# The gate proves it can go red before it is trusted: -ConsoleSample points at a
# binary we know is console-subsystem (the test host), and the parser must report
# it as CONSOLE. A detector that calls everything "GUI" would otherwise look green
# forever.
#
# It also checks the other half of the promise: the CLI must still write to the
# console it was started from (AttachConsole path), so --version has to produce
# text and exit 0.
#
# ASCII only: PowerShell 5 on a GBK console has eaten ASCII characters after
# non-ASCII comments in this project before.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$ConsoleSample
)
$ErrorActionPreference = 'Stop'

function Get-Subsystem([string]$Path) {
    if (-not (Test-Path $Path)) { throw "not found: $Path" }
    # One 4 KB read covers every offset this parser touches: e_lfanew lives at 0x3C
    # and the fields we want are within ~100 bytes after it.
    $head = [System.IO.File]::ReadAllBytes($Path)
    if ($head.Length -lt 512) { throw "too small to be a PE image: $Path" }
    $peOff = [BitConverter]::ToInt32($head, 0x3C)
    if ($peOff -le 0 -or ($peOff + 96) -gt $head.Length) { throw "bad e_lfanew in $Path" }
    if ([System.Text.Encoding]::ASCII.GetString($head, $peOff, 2) -ne 'PE') { throw "not a PE image: $Path" }
    # 'PE\0\0' (4) + COFF header (20) = the optional header starts 24 past the
    # signature. Reading it any earlier lands on the machine field (0x8664 = AMD64),
    # which is exactly the wrong number an earlier version of this parser reported.
    $opt = $peOff + 24
    $magic = [BitConverter]::ToUInt16($head, $opt)
    if ($magic -ne 0x10b -and $magic -ne 0x20b) {
        throw "unknown optional header magic 0x$($magic.ToString('x')) in $Path"
    }
    # Subsystem is at offset 68 of the optional header in both PE32 and PE32+.
    return [BitConverter]::ToUInt16($head, $opt + 68)
}

$failures = @()
function Add-Failure([string]$m) { $script:failures += $m }

# 1) self-test: the sample must read as CONSOLE (3), or this gate proves nothing.
$sampleSub = $null
try { $sampleSub = Get-Subsystem $ConsoleSample } catch { Add-Failure ("sample unreadable: " + $_.Exception.Message) }
if ($sampleSub -ne $null) {
    if ($sampleSub -ne 3) {
        Add-Failure ("SELF-TEST FAILED: the console sample reports subsystem $sampleSub, expected 3 " +
            "- this gate cannot tell GUI from console and must not be trusted")
    } else {
        Write-Output ('self-test OK: console sample reports subsystem 3 (CONSOLE)')
    }
}

# 2) the shipped exe must be GUI (2)
$productSub = $null
try { $productSub = Get-Subsystem $Exe } catch { Add-Failure ("product unreadable: " + $_.Exception.Message) }
if ($productSub -ne $null) {
    if ($productSub -eq 3) {
        Add-Failure ("$([System.IO.Path]::GetFileName($Exe)) is CONSOLE subsystem - double-click opens a terminal")
    } elseif ($productSub -ne 2) {
        Add-Failure ("unexpected subsystem $productSub")
    } else {
        Write-Output ('product OK: subsystem 2 (WINDOWS GUI) - no terminal on double-click')
    }
}

# 3) the CLI must still reach the console it was started from. A GUI-subsystem
#    exe that silently loses stdout would pass check 2 and still break --journal.
$cliOut = ''
$cliRc = -1
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = '--version'
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $t = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit(15000)) { try { $p.Kill() } catch { }; Add-Failure '--version did not finish in 15 s' }
    else { $p.WaitForExit(); $cliRc = $p.ExitCode; $cliOut = ($t.Result -replace "\r?\n", ' ').Trim() }
    $p.Dispose()
} catch { Add-Failure ('--version run failed: ' + $_.Exception.Message) }
if ($cliRc -ne 0) { Add-Failure ("--version exit code = $cliRc, expected 0") }
if ($cliOut -notmatch 'AHKDeck') { Add-Failure ("--version produced no console text ('$cliOut') - the AttachConsole path is broken") }
else { Write-Output ('CLI OK: --version printed through the attached console: ' + $cliOut) }

if ($failures.Count -gt 0) {
    foreach ($f in $failures) { Write-Output ('  FAIL: ' + $f) }
    exit 1
}
Write-Output 'OK: GUI subsystem, no console window on double-click, CLI still writes to its console'
exit 0
