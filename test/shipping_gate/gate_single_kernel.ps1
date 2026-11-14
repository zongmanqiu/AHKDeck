# m192 P5 shipping gate: the release folder carries exactly ONE AutoHotkey PE.
#
# m190's kernel_pair compared an AHK_H DLL against a tools\ compile base. m192
# retires both: the deck spawns <exeDir>\AutoHotkey64.exe once per session, and
# core/ExeCompiler.cpp bundles every compiled script into a copy of that same
# image. This gate checks the SHIPPED LAYOUT itself:
#   1. AHKDeck.exe + AutoHotkey64.exe exist, and the kernel is a plain EXE
#   2. no AutoHotkey64.dll, no tools\ folder, and no other PE besides the deck
#   3. the staged kernel is byte-identical to .temp\kernel_official (a copy,
#      not a leftover from an older build), and has a version resource
# -SelfTest creates a deliberately polluted layout under .temp and demands the
# same checks report a failure. A gate that cannot fail is not a gate.
#
# ASCII only (PowerShell 5 + GBK console safety).
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$KernelDir = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# PE characteristics: 0x2000 (IMAGE_FILE_DLL) is the only bit we care about.
# Reading the header is enough - no LoadLibrary, no execution.
function Test-IsDllImage([string]$path) {
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $br = New-Object System.IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $peOffset = $br.ReadInt32()
        $fs.Position = $peOffset + 4 + 18
        $characteristics = $br.ReadUInt16()
        return (($characteristics -band 0x2000) -ne 0)
    }
    finally { $fs.Dispose() }
}

# Returns a single object so nothing else leaks into the pipeline.
function Invoke-LayoutCheck([string]$dir, [string]$freshKernel) {
    $lines = New-Object System.Collections.Generic.List[string]
    $bad = 0

    $deck = Join-Path $dir 'AHKDeck.exe'
    $kernel = Join-Path $dir 'AutoHotkey64.exe'
    if (Test-Path -LiteralPath $deck) { [void]$lines.Add('ok   AHKDeck.exe present') }
    else { [void]$lines.Add('FAIL AHKDeck.exe missing'); $bad++ }

    $kernelThere = Test-Path -LiteralPath $kernel
    if ($kernelThere) { [void]$lines.Add('ok   AutoHotkey64.exe present') }
    else { [void]$lines.Add('FAIL AutoHotkey64.exe missing'); $bad++ }

    $dll = Join-Path $dir 'AutoHotkey64.dll'
    if (Test-Path -LiteralPath $dll) { [void]$lines.Add('FAIL retired AutoHotkey64.dll is back'); $bad++ }
    else { [void]$lines.Add('ok   no AutoHotkey64.dll') }

    $tools = Join-Path $dir 'tools'
    if (Test-Path -LiteralPath $tools) { [void]$lines.Add('FAIL retired tools\ folder is back'); $bad++ }
    else { [void]$lines.Add('ok   no tools\ folder') }

    # Every PE under the release root other than the deck itself. A compiled
    # script, a stale base, or a DLL dropped anywhere in here must be caught.
    $pe = @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.exe', '.dll') } |
        Where-Object { $_.Name -ne 'AHKDeck.exe' })
    $names = ($pe | ForEach-Object { $_.Name }) -join ','
    if ($pe.Count -eq 1 -and $pe[0].Name -ieq 'AutoHotkey64.exe') {
        [void]$lines.Add('ok   exactly one extra PE: AutoHotkey64.exe')
    }
    else {
        [void]$lines.Add('FAIL expected exactly one extra PE, found ' + $pe.Count + ' [' + $names + ']')
        $bad++
    }

    if ($kernelThere) {
        if (-not (Test-IsDllImage $kernel)) { [void]$lines.Add('ok   kernel is a plain EXE image') }
        else { [void]$lines.Add('FAIL kernel carries IMAGE_FILE_DLL'); $bad++ }

        $v = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($kernel)
        if ($v.ProductVersion) { [void]$lines.Add('ok   kernel version ' + $v.ProductVersion) }
        else { [void]$lines.Add('FAIL kernel has no version resource'); $bad++ }

        if ($freshKernel -ne '' -and (Test-Path -LiteralPath $freshKernel)) {
            $a = (Get-FileHash -LiteralPath $freshKernel -Algorithm SHA256).Hash
            $b = (Get-FileHash -LiteralPath $kernel -Algorithm SHA256).Hash
            if ($a -eq $b) { [void]$lines.Add('ok   staged kernel is byte-identical to .temp\kernel_official') }
            else { [void]$lines.Add('FAIL staged kernel differs from .temp\kernel_official'); $bad++ }
        }
        else { [void]$lines.Add('     skipped byte check: no fresh kernel at ' + $freshKernel) }
    }

    return [pscustomobject]@{ Fails = $bad; Lines = $lines }
}

$fresh = ''
if ($KernelDir -ne '') { $fresh = Join-Path $KernelDir 'AutoHotkey64.exe' }

if ($SelfTest) {
    # Rebuild the two retired shapes next to a copy of the real layout. If the
    # checks above still pass, they are not checking anything.
    $projectRoot = Split-Path -Parent $BuildDir
    $badDir = Join-Path $projectRoot '.temp\single_kernel_gate'
    if (Test-Path -LiteralPath $badDir) { Remove-Item -LiteralPath $badDir -Recurse -Force }
    New-Item -ItemType Directory -Path $badDir -Force | Out-Null
    try {
        Copy-Item (Join-Path $BuildDir 'AHKDeck.exe') (Join-Path $badDir 'AHKDeck.exe')
        Copy-Item (Join-Path $BuildDir 'AutoHotkey64.exe') (Join-Path $badDir 'AutoHotkey64.exe')
        Copy-Item (Join-Path $BuildDir 'AutoHotkey64.exe') (Join-Path $badDir 'AutoHotkey64.dll')
        New-Item -ItemType Directory -Path (Join-Path $badDir 'tools') -Force | Out-Null
        Copy-Item (Join-Path $BuildDir 'AutoHotkey64.exe') (Join-Path $badDir 'tools\AutoHotkey64.exe')

        $result = Invoke-LayoutCheck $badDir $fresh
        $result.Lines | ForEach-Object { Write-Output $_ }
        if ($result.Fails -gt 0) {
            Write-Output ('ok   self-test: polluted layout reported ' + $result.Fails + ' failure(s)')
            Write-Output 'result: 0 failure(s)'
            exit 0
        }
        Write-Output 'FAIL self-test: a layout with a DLL and tools\ copy passed the gate'
        Write-Output 'result: 1 failure(s)'
        exit 1
    }
    finally {
        if (Test-Path -LiteralPath $badDir) { Remove-Item -LiteralPath $badDir -Recurse -Force }
    }
}

$result = Invoke-LayoutCheck $BuildDir $fresh
$result.Lines | ForEach-Object { Write-Output $_ }
Write-Output ('result: ' + $result.Fails + ' failure(s)')
if ($result.Fails -ne 0) { exit 1 }
exit 0
