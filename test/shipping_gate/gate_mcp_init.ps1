# m214 gate: --mcp-init must print three parseable agent blocks, and
# --mcp-install must merge (never clobber) an agent's existing config.
# ASCII only: PowerShell 5 on a GBK console mangles non-ASCII comments here.
param(
    [Parameter(Mandatory = $true)][string]$Exe
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Exe)) { Write-Output "FAIL: host not built: $Exe"; exit 1 }

$failures = @()
function Add-Failure([string]$m) { $script:failures += $m }

function Invoke-Host([string]$ArgLine, [string]$HomeDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $ArgLine
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    if ($HomeDir) { $psi.EnvironmentVariables['USERPROFILE'] = $HomeDir }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(15000)) {
        try { $p.Kill(); [void]$p.WaitForExit(3000) } catch { }
        $p.Dispose()
        Add-Failure ("host did not finish: $ArgLine")
        return ''
    }
    $out = $outTask.Result
    $err = $errTask.Result
    $rc = $p.ExitCode
    $p.Dispose()
    if ($rc -ne 0) { Add-Failure ("$ArgLine exit code = $rc; stdout=[" + $out + "]; stderr=[" + $err + "]") }
    if ($err -ne '') { Add-Failure ("$ArgLine wrote stderr: $err") }
    return $out
}

# --- --mcp-init: three blocks, each valid JSON with our stdio endpoint ---
$initOut = Invoke-Host '--mcp-init' ''
foreach ($agent in @('workbuddy', 'claude', 'cursor')) {
    $pattern = '(?ms)^\[' + $agent + '\].*?^\{(?<json>.*?)^\}'
    $match = [regex]::Match($initOut, $pattern)
    if (-not $match.Success) {
        Add-Failure ("--mcp-init has no block for $agent")
        continue
    }
    try { $obj = ConvertFrom-Json -InputObject ('{' + $match.Groups['json'].Value + '}') }
    catch { Add-Failure ("$agent block is not JSON: " + $_.Exception.Message); continue }
    $server = $obj.mcpServers.ahkdeck
    if ($null -eq $server) { Add-Failure ("$agent block has no ahkdeck server") }
    else {
        if (-not [string]$server.command) { Add-Failure ("$agent block has no command") }
        if ([string]$server.args -notmatch 'mcp-stdio') {
            Add-Failure ("$agent block has wrong args: " + [string]$server.args)
        }
    }
}

# --- --mcp-install: merge, keep other servers and unknown keys, back up ---
$testHome = Join-Path ([IO.Path]::GetTempPath()) ('ahkdeck-mcp-init-' + [guid]::NewGuid().ToString('N'))
$dir = Join-Path $testHome '.workbuddy'
[void][IO.Directory]::CreateDirectory($dir)
$cfg = Join-Path $dir 'mcp.json'
[IO.File]::WriteAllText($cfg, '{"mcpServers":{"other":{"command":"python"}},"theme":"dark"}', [Text.Encoding]::UTF8)

$installOut = Invoke-Host '--mcp-install workbuddy --yes' $testHome
if (-not (Test-Path ($cfg + '.bak'))) { Add-Failure 'install did not back up the old config' }
try { $merged = Get-Content $cfg -Raw -Encoding UTF8 | ConvertFrom-Json }
catch { Add-Failure ('installed config is not JSON: ' + $_.Exception.Message); $merged = $null }
if ($null -ne $merged) {
    if ([string]$merged.theme -ne 'dark') { Add-Failure 'install dropped an unknown top-level key' }
    if ($null -eq $merged.mcpServers.other) { Add-Failure 'install dropped another MCP server' }
    if ($null -eq $merged.mcpServers.ahkdeck) { Add-Failure 'install did not add ahkdeck' }
}
$again = Invoke-Host '--mcp-install workbuddy --yes' $testHome
if ($again -notmatch 'was present') { Add-Failure 're-install did not detect the existing entry' }
try { [IO.Directory]::Delete($testHome, $true) } catch { }

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Output ("FAIL " + $_) }
    Write-Output ("mcp_init: " + $failures.Count + " failure(s)")
    exit 1
}
Write-Output 'mcp_init: all checks passed'
