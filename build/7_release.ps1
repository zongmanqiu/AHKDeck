# m215: safe, fork-friendly release pipeline.
#
#   powershell -File 7_release.ps1                # DRY RUN: preflight + plan, no publish
#   powershell -File 7_release.ps1 -Apply         # publish to both mirrors
#   powershell -File 7_release.ps1 -Apply -Only gitee
#
# Security rules (do not relax):
#   * No token in the repo, in .temp, in git config, in argv, or in logs.
#   * GitHub auth = gh's own browser device flow / keyring.
#   * Gitee auth = git credential manager; if absent, OPEN THE TOKEN PAGE and
#     ask the user to paste once, then store it with git credential approve.
#   * curl receives the Gitee token through `curl -K -` (stdin config), never
#     through a command-line argument.
#   * Release notes come from HISTORY.md with local paths / emails scrubbed.
#   * Every path is derived from $PSScriptRoot; owner/repo from git origin.
#   * Dry-run by default: publishing is irreversible and must be explicit.
param(
    [switch]$Apply,
    [ValidateSet('gitee', 'github')][string]$Only = '',
    [switch]$SkipBuild
)
# 'Stop' makes PowerShell 5.1 treat ANY native-command stderr as a terminating
# error: git push's remote chatter and `gh release view` on a missing release
# would abort the script before the $LASTEXITCODE checks below. This pipeline
# decides success from $LASTEXITCODE / $failures, not from exceptions.
$ErrorActionPreference = 'Continue'

$Root = Split-Path -Parent $PSScriptRoot                 # .main
$Deck = Split-Path -Parent $Root                         # AHKDeck
$BuildDir = Join-Path $Deck '.build'
$TempDir = Join-Path $Deck '.temp'
$HistoryPath = Join-Path $Root 'HISTORY.md'
$UpdatePath = Join-Path $Root 'update.json'
$CmakePath = Join-Path $Root 'CMakeLists.txt'

$failures = @()
function Fail([string]$m) { $script:failures += $m; Write-Output ("FAIL " + $m) }
function Info([string]$m) { Write-Output ("  " + $m) }

if (-not (Test-Path $HistoryPath)) { Fail 'HISTORY.md missing'; exit 1 }
$history = Get-Content $HistoryPath -Raw -Encoding UTF8
$versionMatch = [regex]::Match((Get-Content $CmakePath -Raw -Encoding UTF8),
    'project\s*\(\s*AHKDeck\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) { Fail 'version not found in CMakeLists.txt'; exit 1 }
$version = $versionMatch.Groups[1].Value
Info "version=$version (CMakeLists.txt)"

# --- version rule: strictly increase over the previous HISTORY section ------
$histVersions = @([regex]::Matches($history, '(?m)^##\s+([0-9]+\.[0-9]+\.[0-9]+)') |
    ForEach-Object { $_.Groups[1].Value })
if ($histVersions.Count -lt 2) {
    Fail 'HISTORY.md needs at least two version sections'
} elseif ([version]$histVersions[0] -ne [version]$version) {
    Fail "HISTORY top section $($histVersions[0]) != CMake version $version"
} elseif ([version]$version -le [version]$histVersions[1]) {
    Fail "version $version must strictly increase over previous $($histVersions[1])"
} else {
    Info "version rule: $($histVersions[1]) -> $version (strict increase OK)"
}

# --- release notes: HISTORY top section, privacy-scrubbed -------------------
$sectionMatch = [regex]::Match($history, '(?ms)^## ' + [regex]::Escape($version) +
    '.*?(?=^## |\z)')
if (-not $sectionMatch.Success) { Fail "HISTORY.md has no section for $version" }
$notes = $sectionMatch.Value.Trim()
$notes = $notes -replace '(?i)[A-Za-z]:\\Users\\[^\s)"]+', '(local path omitted)'
$notes = $notes -replace '(?i)[A-Za-z]:[\\/][^\s)"]+', '(local path omitted)'
$notes = $notes -replace '[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}', '(email omitted)'
$notesPath = Join-Path $TempDir ("release_notes_$version.md")
[IO.File]::WriteAllText($notesPath, $notes + "`n", (New-Object Text.UTF8Encoding($false)))
Info "notes: HISTORY.md section -> $notesPath ($($notes.Length) chars, scrubbed)"

if ($notes -match ([string]::Concat([char]0x672A, [char]0x53D1, [char]0x5E03)) -and $Apply) {
    Fail 'HISTORY section is marked unreleased; remove the marker before -Apply'
}

# --- git facts: branch, clean tree, HEAD pushed, remote identity ------------
function Invoke-Git([string[]]$Arguments) {
    $out = @(& git -C $Root @Arguments 2>&1)
    return (@($out) -join "`n")
}
$branch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Trim()
$head = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
$dirty = (Invoke-Git @('status', '--porcelain')).Trim()
$remoteUrl = (Invoke-Git @('remote', 'get-url', 'origin')).Trim()
Info "branch=$branch head=$head"
if ($branch -notin @('main', 'master')) { Fail "unexpected branch '$branch'" }
if ($dirty) { Fail "working tree is dirty: $dirty" }
if (-not $remoteUrl) { Fail 'no origin remote' }

$giteeOwner = ''; $giteeRepo = ''; $githubRepo = ''
$giteeMatch = [regex]::Match($remoteUrl, 'gitee\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$')
$githubMatch = [regex]::Match($remoteUrl, 'github\.com[:/]([^/]+)/([^/]+?)(?:\.git)?$')
if ($giteeMatch.Success) {
    $giteeOwner = $giteeMatch.Groups[1].Value
    $giteeRepo = $giteeMatch.Groups[2].Value
    # Our mirror convention; other owners cannot be guessed - they pass -Only github.
    if ($giteeOwner -eq 'qiuzongman') { $githubRepo = 'zongmanqiu/' + $giteeRepo }
} elseif ($githubMatch.Success) {
    $githubRepo = $githubMatch.Groups[1].Value + '/' + $githubMatch.Groups[2].Value
}
Info "origin=$remoteUrl"
Info "gitee=$giteeOwner/$giteeRepo github=$githubRepo"

# PowerShell parses @('a', 'b' + $v) as THREE elements ('a','b',$v), not two.
# Build the ref names first or git gets a broken argument list and the gate
# wrongly reports "HEAD is not on origin" / "tag missing".
$headRef = 'refs/heads/' + $branch
$tagRef = 'refs/tags/' + $version
$remoteHead = (Invoke-Git @('ls-remote', 'origin', $headRef))
if ($remoteHead -notmatch ([regex]::Escape($head))) {
    Fail 'HEAD is not on origin; push first'
}
$localTag = (Invoke-Git @('rev-parse', '-q', '--verify', $tagRef))
$remoteTag = (Invoke-Git @('ls-remote', '--tags', 'origin', $tagRef))
Info "tag $version local=$([bool]$localTag) remote=$([bool]$remoteTag)"

$update = Get-Content $UpdatePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]$update.version -ne $version) { Fail "update.json version=$($update.version) != $version" }

# --- package + whitelist ----------------------------------------------------
$zip = Join-Path $BuildDir ("AHKDeck_$version.zip")
if (-not (Test-Path $zip)) {
    if ($Apply) { Fail "package missing: $zip (run 6_package.ps1 / leave -SkipBuild off)" }
    else { Info "package not built yet (dry run): $zip" }
} else {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $names = @($archive.Entries | ForEach-Object { $_.FullName })
        $bad = @($names | Where-Object { $_ -match '(?i)(\.git[\\/]|\.temp[\\/]|token|secret|\.pfx$|\.key$|^\.env|Users[\\/])' })
        if ($bad.Count -gt 0) { Fail ('unsafe entries in package: ' + ($bad -join ', ')) }
        foreach ($need in @('AHKDeck.exe', 'AutoHotkey64.exe')) {
            if ($names -notcontains $need) { Fail "package is missing $need" }
        }
        $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
        Info "zip=$zip bytes=$((Get-Item $zip).Length) sha256=$hash entries=$($names.Count)"
    } finally { $archive.Dispose() }
}

# --- full gate + package, unless explicitly skipped -------------------------
if (-not $SkipBuild) {
    Info 'running full.bat (1-5) ...'
    $env:AHKDECK_CHAINED = '1'
    & (Join-Path $PSScriptRoot 'full.bat')
    if ($LASTEXITCODE -ne 0) { Fail 'full.bat failed' }
    Info 'running 6_package.ps1 ...'
    & (Join-Path $PSScriptRoot '6_package.ps1')
    if ($LASTEXITCODE -ne 0) { Fail '6_package.ps1 failed' }
}

if ($failures.Count -gt 0) {
    Write-Output ("PREFLIGHT FAILED: " + $failures.Count + " problem(s); nothing published.")
    exit 1
}
Write-Output ("PREFLIGHT OK for $version" + $(if ($Apply) { ' - publishing' } else { ' (dry run; use -Apply to publish)' }))
if (-not $Apply) {
    Write-Output ('PLAN: ' + $(if ($Only -eq 'gitee') { 'Gitee' } elseif ($Only -eq 'github') { 'GitHub' } else { 'Gitee + GitHub' }) +
        ' release; tag ' + $version + '; attach ' + (Split-Path -Leaf $zip))
    exit 0
}

# --- credentials (never echoed, never written to project files) -------------
function GitCredentialFill([string]$HostName) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = 'git'; $psi.Arguments = 'credential fill'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true; $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Write("protocol=https`nhost=$HostName`n`n")
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $p.Dispose()
    $token = ''; $payload = ''
    foreach ($line in ($out -split "`r?`n")) {
        if ($line.StartsWith('password=')) { $token = $line.Substring(9) }
        $payload += $line + "`n"
    }
    return @{ token = $token; payload = $payload }
}

function GitCredentialApprove([string]$Payload) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = 'git'; $psi.Arguments = 'credential approve'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true; $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Write($Payload + "`n")
    $p.StandardInput.Close()
    $p.WaitForExit()
    $p.Dispose()
}

$script:giteeToken = ''
if ($Only -ne 'github' -and $giteeOwner) {
    $cred = GitCredentialFill 'gitee.com'
    $script:giteeToken = $cred.token
    if (-not $script:giteeToken) {
        Write-Output 'Gitee authorization needed: opening the token page.'
        try { Start-Process 'https://gitee.com/profile/personal_access_tokens' } catch { }
        $secure = Read-Host -AsSecureString 'Paste the Gitee personal access token (never echoed)'
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
        $script:giteeToken = $plain.Trim()
        $payload = "protocol=https`nhost=gitee.com`nusername=$giteeOwner`npassword=$script:giteeToken`n"
        GitCredentialApprove $payload
        Write-Output 'Token stored in the OS credential manager (not in the repository).'
    }
}

if ($Only -ne 'gitee' -and $githubRepo) {
    $ghStatus = & gh auth status 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Output 'GitHub authorization needed: opening the browser device flow.'
        & gh auth login --web --git-protocol https
        if ($LASTEXITCODE -ne 0) { Fail 'gh auth login failed' }
    }
}

if ($failures.Count -gt 0) { Write-Output 'AUTH FAILED; nothing published.'; exit 1 }

# --- tag: create locally if missing, push explicitly ------------------------
if (-not $localTag) {
    & git -C $Root tag -a $version -m "AHKDeck $version"
    if ($LASTEXITCODE -ne 0) { Fail 'git tag failed' }
}
if (-not $remoteTag) {
    & git -C $Root push origin $version
    if ($LASTEXITCODE -ne 0) { Fail 'git push tag failed' }
}

# --- Gitee release (token only in request body / stdin config) --------------
function ConvertTo-AsciiJson([string]$Text) {
    # Gitee decodes API bodies as Latin-1: escape every non-ASCII code unit.
    $sb = New-Object Text.StringBuilder
    foreach ($ch in $Text.ToCharArray()) {
        if ([int]$ch -lt 128) { [void]$sb.Append($ch) }
        else { [void]$sb.Append(('\u{0:X4}' -f [int]$ch)) }
    }
    return $sb.ToString()
}

if ($Only -ne 'github' -and $giteeOwner -and -not $failures.Count) {
    $base = "https://gitee.com/api/v5/repos/$giteeOwner/$giteeRepo/releases"
    $existing = $null
    try {
        $existing = Invoke-RestMethod -Method Get -Headers @{ Authorization = "Bearer $script:giteeToken" } `
            -Uri "$base/tags/$version" -TimeoutSec 30
    } catch { }
    $bodyObj = @{
        access_token = $script:giteeToken
        tag_name = $version
        name = "AHKDeck $version"
        target_commitish = $branch
        body = $notes
        prerelease = $false
    }
    $ascii = ConvertTo-AsciiJson (ConvertTo-Json $bodyObj -Depth 8 -Compress)
    if ($existing -and $existing.id) {
        $rel = Invoke-RestMethod -Method Patch -Uri "$base/$($existing.id)" `
            -ContentType 'application/json' -Body $ascii -TimeoutSec 60
        Info "Gitee release $($existing.id) updated"
    } else {
        $rel = Invoke-RestMethod -Method Post -Uri $base `
            -ContentType 'application/json' -Body $ascii -TimeoutSec 60
        Info "Gitee release $($rel.id) created"
    }
    $leaf = Split-Path -Leaf $zip
    # curl.exe reads the token from stdin config: it never appears in argv or
    # logs. Use curl.exe, NOT curl - PowerShell aliases curl to Invoke-WebRequest.
    $config = "url = `"$base/$($rel.id)/attach_files`"`n" +
        "header = `"Authorization: Bearer $script:giteeToken`"`n" +
        "form = `"file=@$($zip -replace '\\','/')`"`n"
    $attach = $config | & curl.exe -s -K -
    # Gitee echoes back Chinese names as GBK-mangled bytes; ConvertFrom-Json can
    # choke on them even though the upload succeeded. Match the ASCII URL only.
    $attachText = (@($attach) -join "`n")
    $urlMatch = [regex]::Match($attachText, '"browser_download_url"\s*:\s*"([^"]+)"')
    if ($urlMatch.Success) {
        Info "Gitee asset: $($urlMatch.Groups[1].Value)"
    } else {
        Fail ('Gitee attach failed: ' + $attachText)
    }
}

# --- GitHub release (gh owns the token) -------------------------------------
if ($Only -ne 'gitee' -and $githubRepo -and -not $failures.Count) {
    $existingGh = & gh release view $version --repo $githubRepo 2>&1
    if ($LASTEXITCODE -ne 0) {
        & gh release create $version --repo $githubRepo --target $branch `
            --title "AHKDeck $version" --notes-file $notesPath $zip
    } else {
        & gh release upload $version $zip --repo $githubRepo --clobber
    }
    if ($LASTEXITCODE -ne 0) { Fail 'GitHub release failed' }
    else { Info "GitHub release: https://github.com/$githubRepo/releases/tag/$version" }
}

if ($failures.Count -gt 0) {
    Write-Output ("PUBLISH HAD " + $failures.Count + " FAILURE(S)")
    exit 1
}
Write-Output "RELEASE OK: $version"
