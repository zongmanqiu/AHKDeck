# m202: the kernel version has ONE display parameter (core/Version.h), but the
# number also lives in three places a human must keep in sync when upgrading
# the official kernel:
#   1. patches/AutoHotkey-v2.0.31/source/ahkversion.h  (RAW_AHK_VERSION + AHK_VERSION_N)
#   2. build/deps.txt                                  (the pinned upstream tag)
#   3. .build/AutoHotkey64.exe                         (PE ProductVersion)
# A parameter that can quietly disagree with the shipped binary is worse than
# no parameter at all, so this gate compares all four. -SelfTest builds
# deliberately poisoned copies and demands the checker report a failure.
#
# ASCII only (PowerShell 5 + GBK console safety).
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Get-Declared([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $text = Get-Content -LiteralPath $path -Raw
    $m = [regex]::Match($text, 'kKernelVersion\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $m.Success) { return '' }
    return $m.Groups[1].Value
}

function Invoke-VersionCheck([string]$versionHeader, [string]$patchHeader,
                             [string]$depsFile, [string]$kernelExe) {
    $lines = New-Object System.Collections.Generic.List[string]
    $bad = 0
    $v = Get-Declared $versionHeader

    if ($v -match '^[0-9]+\.[0-9]+\.[0-9]+$') {
        [void]$lines.Add('ok   kKernelVersion = ' + $v)
    } else {
        [void]$lines.Add('FAIL kKernelVersion missing or malformed in ' + $versionHeader)
        $bad++
        return [pscustomobject]@{ Fails = $bad; Lines = $lines; Version = $v }
    }

    # 1. Upstream patch must pin the same string and the same numeric tuple.
    if (Test-Path -LiteralPath $patchHeader) {
        $patch = Get-Content -LiteralPath $patchHeader -Raw
        $raw = [regex]::Match($patch, 'define\s+RAW_AHK_VERSION\s+"([^"]+)"')
        if ($raw.Success -and $raw.Groups[1].Value -eq $v) {
            [void]$lines.Add('ok   RAW_AHK_VERSION matches ' + $v)
        } else {
            [void]$lines.Add('FAIL RAW_AHK_VERSION does not match ' + $v)
            $bad++
        }
        $tuple = $v.Replace('.', ',') + ',0'
        $num = [regex]::Match($patch, 'define\s+AHK_VERSION_N\s+([0-9,\s]+)')
        $numClean = ($num.Groups[1].Value -replace '\s', '')
        if ($num.Success -and $numClean -eq $tuple) {
            [void]$lines.Add('ok   AHK_VERSION_N matches ' + $tuple)
        } else {
            [void]$lines.Add('FAIL AHK_VERSION_N does not match ' + $tuple)
            $bad++
        }
    } else {
        [void]$lines.Add('FAIL patch header missing: ' + $patchHeader)
        $bad++
    }

    # 2. deps.txt must pin a tag named after exactly this version, and the
    #    dependency NAME must carry it too (no "source says 2.0.32, URL 2.0.31").
    if (Test-Path -LiteralPath $depsFile) {
        $deps = Get-Content -LiteralPath $depsFile
        $wanted = 'AutoHotkey-v' + $v + '-src='
        $active = @($deps | Where-Object { $_ -match '^AutoHotkey-v[0-9.]+-src=' })
        if ($active.Count -ge 1) {
            [void]$lines.Add('ok   deps.txt has ' + $active.Count + ' official kernel line(s)')
        } else {
            [void]$lines.Add('FAIL deps.txt has no official kernel source line')
            $bad++
        }
        $line = @($active | Where-Object { $_.StartsWith($wanted) })
        if ($line.Count -eq 1 -and $line[0] -match ('refs/tags/v' + [regex]::Escape($v) + '\|')) {
            [void]$lines.Add('ok   deps.txt pins refs/tags/v' + $v)
        } else {
            [void]$lines.Add('FAIL deps.txt does not pin refs/tags/v' + $v + ' for ' + $wanted)
            $bad++
        }
    } else {
        [void]$lines.Add('FAIL deps.txt missing: ' + $depsFile)
        $bad++
    }

    # 3. The shipped PE is the ground truth. FileVersionInfo is metadata only:
    #    no LoadLibrary, no execution.
    if (Test-Path -LiteralPath $kernelExe) {
        $pv = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($kernelExe).ProductVersion
        if ($pv -eq $v) {
            [void]$lines.Add('ok   AutoHotkey64.exe ProductVersion = ' + $v)
        } else {
            [void]$lines.Add('FAIL AutoHotkey64.exe ProductVersion is ' + $pv + ', parameter says ' + $v)
            $bad++
        }
    } else {
        [void]$lines.Add('FAIL shipped kernel missing: ' + $kernelExe)
        $bad++
    }

    return [pscustomobject]@{ Fails = $bad; Lines = $lines; Version = $v }
}

$versionHeader = Join-Path $Root 'core\Version.h'
# The patch archive is named after the version. Deriving it from the declared
# parameter means an upgrade that bumps only kKernelVersion points at a
# missing archive and fails loudly, instead of silently validating the old one.
$declared      = Get-Declared $versionHeader
$patchHeader   = Join-Path $Root ('patches\AutoHotkey-v' + $declared + '\source\ahkversion.h')
$depsFile      = Join-Path $Root 'build\deps.txt'
$kernelExe     = Join-Path $BuildDir 'AutoHotkey64.exe'

if ($SelfTest) {
    # Poison one source at a time. A checker that cannot fail is not a gate.
    $projectRoot = Split-Path -Parent $BuildDir
    $badDir = Join-Path $projectRoot '.temp\kernel_version_gate'
    if (Test-Path -LiteralPath $badDir) { Remove-Item -LiteralPath $badDir -Recurse -Force }
    New-Item -ItemType Directory -Path $badDir -Force | Out-Null
    $poisoned = 0
    try {
        $good = Invoke-VersionCheck $versionHeader $patchHeader $depsFile $kernelExe
        if ($good.Fails -ne 0) {
            Write-Output 'FAIL self-test: the real tree does not pass its own gate'
            exit 1
        }
        Write-Output 'ok   self-test: real tree passes'

        $fake = Join-Path $badDir 'Version.h'
        $src = Get-Content -LiteralPath $versionHeader -Raw
        $src = $src -replace '"[0-9]+\.[0-9]+\.[0-9]+"', '"9.9.9"'
        Set-Content -LiteralPath $fake -Value $src -Encoding ASCII
        $r1 = Invoke-VersionCheck $fake $patchHeader $depsFile $kernelExe
        if ($r1.Fails -gt 0) {
            Write-Output 'ok   self-test: a wrong kKernelVersion is caught'
            $poisoned++
        } else {
            Write-Output 'FAIL self-test: wrong kKernelVersion passed'
        }

        $fakePatch = Join-Path $badDir 'ahkversion.h'
        $pSrc = Get-Content -LiteralPath $patchHeader -Raw
        $pSrc = $pSrc -replace '(define\s+RAW_AHK_VERSION\s+")[^"]+(")', '${1}8.8.8${2}'
        Set-Content -LiteralPath $fakePatch -Value $pSrc -Encoding ASCII
        $r2 = Invoke-VersionCheck $versionHeader $fakePatch $depsFile $kernelExe
        if ($r2.Fails -gt 0) {
            Write-Output 'ok   self-test: a mismatched RAW_AHK_VERSION is caught'
            $poisoned++
        } else {
            Write-Output 'FAIL self-test: mismatched RAW_AHK_VERSION passed'
        }

        Write-Output ('self-test: ' + $poisoned + ' of 2 poisoned samples caught')
        if ($poisoned -eq 2) { exit 0 }
        exit 1
    }
    finally {
        if (Test-Path -LiteralPath $badDir) { Remove-Item -LiteralPath $badDir -Recurse -Force }
    }
}

$result = Invoke-VersionCheck $versionHeader $patchHeader $depsFile $kernelExe
$result.Lines | ForEach-Object { Write-Output $_ }
Write-Output ('result: ' + $result.Fails + ' failure(s)')
if ($result.Fails -ne 0) { exit 1 }
exit 0
