# m190 shipping gate: the in-process kernel and the compile base must be the
# SAME interpreter.
#
# Before m190 the deck ran AHK_H v2.0.25 in-process while 「编译为 EXE」 bundled
# scripts into the official v2.0.30 image: one version up, one fork apart, and a
# different patch set. A script could then pass in the editor and fail once
# compiled (or the reverse) and nothing in the build noticed. Both artifacts now
# come from one tree (build_kernel.bat: MT_ReleaseDll -> the DLL, MT_Release ->
# the base), so the invariant is checkable on the shipped files alone:
#
#   1. both files exist, and are the right PE kind (DLL is a DLL, base is an EXE)
#   2. their version resources agree exactly (the pairing that was broken)
#   3. when .temp\kernel is still around, the staged base is byte-identical to it
#      - i.e. it was copied, not left over from an older build
#
# -SelfTest points the comparator at a pair that MUST disagree (the deck itself),
# so a comparator that always says ok fails the gate instead of passing it.
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$Base,
    [string]$Deck = '',
    [string]$KernelDir = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

$fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Output ("ok   " + $what) }
    else { Write-Output ("FAIL " + $what); $script:fails++ }
}

function Get-FileVersion([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    return ($info.FileVersion + '|' + $info.ProductVersion)
}

# PE characteristics of a file: 0x2000 (IMAGE_FILE_DLL) is the only bit we care
# about, and reading the header is enough - no LoadLibrary, no execution.
function Test-IsDllImage([string]$path) {
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $peOffset = $br.ReadInt32()
        $fs.Position = $peOffset + 4 + 18   # COFF header: +4 signature, +18 characteristics
        $characteristics = $br.ReadUInt16()
        return (($characteristics -band 0x2000) -ne 0)
    }
    finally { $fs.Dispose() }
}

# Diagnostics go to Write-Host on purpose: anything written to the pipeline
# would be captured as part of this function's return value and turn the boolean
# the caller wants into an array.
function Compare-VersionPair([string]$leftLabel, [string]$leftPath, [string]$rightLabel, [string]$rightPath) {
    $left = Get-FileVersion $leftPath
    $right = Get-FileVersion $rightPath
    if ($null -eq $left) { Write-Host ("     $leftLabel missing: " + $leftPath); return $false }
    if ($null -eq $right) { Write-Host ("     $rightLabel missing: " + $rightPath); return $false }
    $same = ($left -eq $right)
    Write-Host ("     $leftLabel=" + $left + "  $rightLabel=" + $right)
    return $same
}

Write-Output "m190 kernel/base parity"

# ---- self test: the comparator must be able to say no -----------------------
if ($SelfTest) {
    if ([string]::IsNullOrEmpty($Deck)) { throw 'SelfTest needs -Deck' }
    Check (-not (Compare-VersionPair 'dll' $Dll 'deck' $Deck)) `
        'self-test: a mismatched pair is reported as a mismatch'
    Write-Output ("result: " + $script:fails + " failure(s)")
    if ($script:fails -ne 0) { exit 1 }
    exit 0
}

# ---- 1. presence and PE kind ------------------------------------------------
$dllThere = Test-Path -LiteralPath $Dll
$baseThere = Test-Path -LiteralPath $Base
Check $dllThere ("kernel DLL present: " + $Dll)
Check $baseThere ("compile base present: " + $Base)
if (-not ($dllThere -and $baseThere)) {
    Write-Output ("result: " + $script:fails + " failure(s)")
    exit 1
}

Check (Test-IsDllImage $Dll) 'kernel image carries IMAGE_FILE_DLL'
Check (-not (Test-IsDllImage $Base)) 'compile base is a plain EXE image'

# ---- 2. same version, i.e. same interpreter build ---------------------------
Check (Compare-VersionPair 'dll' $Dll 'base' $Base) 'kernel and compile base report the same version'

# ---- 3. the staged base is a copy of the current kernel build --------------
if (-not [string]::IsNullOrEmpty($KernelDir)) {
    $fresh = Join-Path $KernelDir 'AutoHotkey64.exe'
    if (Test-Path -LiteralPath $fresh) {
        $a = (Get-FileHash -LiteralPath $fresh -Algorithm SHA256).Hash
        $b = (Get-FileHash -LiteralPath $Base -Algorithm SHA256).Hash
        Check ($a -eq $b) 'staged base is byte-identical to .temp\kernel\AutoHotkey64.exe'
    }
    else {
        Write-Output ("     skipped byte check: no " + $fresh)
    }
}

Write-Output ("result: " + $script:fails + " failure(s)")
if ($script:fails -ne 0) { exit 1 }
exit 0
