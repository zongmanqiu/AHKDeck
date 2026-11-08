# m072: the machine reader AGENTS.md rule 2 never had.
#
# Rule 2 says the source tree (.main) holds hand-written sources only: no build
# intermediates, no build outputs, no third-party source. Until now nothing
# checked it, and a stray __pycache__ (from importing the MCP gate modules)
# proved the hole. This gate reads the tree and fails on artifacts.
#
# What counts as an artifact is deliberately narrow: file extensions a compiler
# or interpreter writes, plus the directories those tools create. Hand-written
# build scripts under .main/build/*.bat and third-party *patches* under
# .main/patches are sources, so they are not banned. Guessing at "looks like a
# vendored library" would produce false reds, so it is not attempted here.
#
# ASCII only: PowerShell 5 on a GBK console has eaten non-ASCII bytes before.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

$script:failures = @()
function Add-Failure([string]$m) { $script:failures += $m }

$bannedExtensions = @(
    '.obj', '.o', '.lib', '.exe', '.dll', '.pdb', '.ilk', '.exp', '.idb',
    '.pch', '.ipdb', '.tlog', '.lastbuildstate', '.res', '.pyc', '.pyo',
    '.class', '.jar', '.ncb', '.sdf', '.suo'
)
$bannedDirectories = @('__pycache__', 'CMakeFiles', '.vs', 'ipch')
# .git is history, not a build artifact; the gate must not walk it.
$skipDirectories = @('.git')

function Find-Artifacts([string]$treeRoot) {
    $hits = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $treeRoot)) {
        throw "not found: $treeRoot"
    }
    $stack = New-Object System.Collections.Generic.List[string]
    $stack.Add($treeRoot)
    while ($stack.Count -gt 0) {
        $dir = $stack[$stack.Count - 1]
        $stack.RemoveAt($stack.Count - 1)
        $name = Split-Path -Leaf $dir
        if ($script:skipDirectories -contains $name) { continue }
        foreach ($child in @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction Stop)) {
            if ($child.PSIsContainer) {
                if ($script:bannedDirectories -contains $child.Name) {
                    $hits.Add(("{0} (directory)" -f $child.FullName))
                } else {
                    $stack.Add($child.FullName)
                }
            } else {
                $ext = [System.IO.Path]::GetExtension($child.Name).ToLowerInvariant()
                if ($script:bannedExtensions -contains $ext) {
                    $hits.Add($child.FullName)
                }
            }
        }
    }
    return $hits
}

function Report([string]$label, $hits, [int]$expected) {
    if ($hits.Count -eq $expected) {
        Write-Output ("{0}: {1} artifact(s), expected {2} - PASS" -f $label, $hits.Count, $expected)
        return
    }
    Add-Failure ("{0}: {1} artifact(s), expected {2}" -f $label, $hits.Count, $expected)
    foreach ($hit in $hits) { Add-Failure ("  " + $hit) }
}

if ($SelfTest) {
    # Build a tree that MUST go red, and a sibling that MUST stay green. A
    # detector that cannot fail is not a detector.
    $base = Join-Path $env:TEMP ("ahkdeck_source_tree_selftest_" + [System.Guid]::NewGuid().ToString('N'))
    $bad = Join-Path $base 'bad'
    $good = Join-Path $base 'good'
    New-Item -ItemType Directory -Path (Join-Path $bad 'src') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $bad '__pycache__') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $good 'src') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $bad 'src\main.obj') -Value 'x'
    Set-Content -LiteralPath (Join-Path $bad 'src\app.exe') -Value 'x'
    Set-Content -LiteralPath (Join-Path $bad '__pycache__\m.pyc') -Value 'x'
    Set-Content -LiteralPath (Join-Path $good 'src\main.cpp') -Value 'x'
    Set-Content -LiteralPath (Join-Path $good 'src\build_patch.ps1') -Value 'x'
    try {
        $badHits = Find-Artifacts $bad
        $goodHits = Find-Artifacts $good
        Report 'self-test red tree' $badHits 3
        Report 'self-test green tree' $goodHits 0
    } finally {
        Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    $hits = Find-Artifacts $Root
    Report 'source tree' $hits 0
    if ($script:failures.Count -eq 0) {
        Write-Output 'rule 2 holds: no compiler/interpreter artifacts under the source tree'
    }
}

if ($script:failures.Count -gt 0) {
    Write-Output ''
    Write-Output 'FAILURES:'
    foreach ($f in $script:failures) { Write-Output ('  ' + $f) }
    exit 1
}
Write-Output 'OK'
exit 0
