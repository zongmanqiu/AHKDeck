param(
    [Parameter(Mandatory = $true)][string]$PatchDir,
    [Parameter(Mandatory = $true)][string]$DstDir,
    [Parameter(Mandatory = $true)][string]$Baseline
)

# Every file under .main\patches that is listed in baseline.sha256 replaces a
# pristine upstream file wholesale. If the destination is neither the expected
# pristine text nor our own patched text, the upstream tree moved (version bump
# or manual edit) and the patch no longer means what it says. Fail loudly.

if (-not (Test-Path $Baseline)) { exit 0 }

$base = @{}
foreach ($line in Get-Content $Baseline) {
    if ($line -match '^([0-9a-fA-F]{64})\s+(.+)$') {
        $base[$matches[2].Trim()] = $matches[1].ToLower()
    }
}

$failed = @()
foreach ($file in (Get-ChildItem $PatchDir -Recurse -File)) {
    $rel = $file.FullName.Substring($PatchDir.Length + 1).Replace('\', '/')
    $label = 'source/' + $rel
    if (-not $base.ContainsKey($label)) { continue }  # new file, no baseline
    $dst = Join-Path $DstDir $rel
    if (-not (Test-Path $dst)) { continue }           # fresh tree
    $dstHash = (Get-FileHash $dst -Algorithm SHA256).Hash.ToLower()
    $patchHash = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower()
    if ($dstHash -ne $base[$label] -and $dstHash -ne $patchHash) {
        $failed += $label
    }
}

if ($failed.Count -gt 0) {
    Write-Host '  [ERROR] patch baseline mismatch (upstream changed or hand-edited):'
    foreach ($f in $failed) { Write-Host ('    - ' + $f) }
    Write-Host '  Rebase the patch on the new upstream text before rebuilding.'
    exit 1
}
exit 0
