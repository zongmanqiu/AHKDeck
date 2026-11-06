# m023: same-script A/B run, official AutoHotkey 2.0.25 vs the SHIPPED deck.
#
# Why this exists: m023 patched the kernel itself (a text-loaded main script now
# gets its script folder as the load-time working directory, which is what makes a
# plain "#Include name.inc" resolve like it does under AutoHotkey.exe). The m004
# pair gate compares two standalone exes, so it cannot see that branch at all -
# only the deck can. This gate re-reads the parity claim at the shipped endpoint.
#
# Verdict per case: exit code and out.txt bytes must match. A case where official
# blocks on its own error dialog is reported as DIALOG_OFFICIAL, not as a diff:
# the deck's contract (m010) is precisely that it never shows that dialog.
#
# ASCII only (PowerShell 5 + GBK console safety). Opt-in, not in ctest: it runs
# every corpus case twice.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File .main\test\kernel_gate\gate_deck_pair.ps1

param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))),
    [int]$TimeoutMs = 20000
)
$ErrorActionPreference = 'Continue'

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public class DeckPair {
  public delegate bool EnumCb(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public static int Dialogs(int procId) {
    int count = 0;
    EnumWindows((h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == (uint)procId && IsWindowVisible(h)) {
        var sb = new StringBuilder(64);
        GetClassName(h, sb, 64);
        if (sb.ToString() == "#32770") count++;
      }
      return true;
    }, IntPtr.Zero);
    return count;
  }
}
"@

$offExe = Join-Path $Root '.3rd\AutoHotkey_2.0.25\AutoHotkey64.exe'
$deckExe = Join-Path $Root '.build\AHKDeck.exe'
$corpora = @(
    (Join-Path $Root '.temp\m002\armB\corpus'),
    (Join-Path $Root '.temp\m004\corpus\edge')
)
$base = Join-Path $Root '.temp\m023pair'
foreach ($p in @($offExe, $deckExe)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}
foreach ($c in $corpora) {
    if (-not (Test-Path $c)) { throw "missing corpus: $c (m004 phase B inputs)" }
}
if (Test-Path $base) { Remove-Item $base -Recurse -Force }
New-Item -ItemType Directory -Force -Path $base | Out-Null

function Run-Engine([string]$exe, [bool]$isDeck, [string]$script, [string]$wd) {
    $outP = Join-Path $wd 'out.txt'
    if (Test-Path $outP) { Remove-Item $outP -Force }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = if ($isDeck) { '--run "' + $script + '"' } else { '/ErrorStdOut=UTF-8 "' + $script + '"' }
    $psi.WorkingDirectory = $wd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $soTask = $p.StandardOutput.ReadToEndAsync()
    $seTask = $p.StandardError.ReadToEndAsync()
    $dlg = 0
    $timedOut = -not $p.WaitForExit($TimeoutMs)
    if (-not $timedOut) {
        $dlg = [DeckPair]::Dialogs($p.Id)
    }
    else {
        $dlg = [DeckPair]::Dialogs($p.Id)
        $p.Kill()
    }
    $so = ''
    $se = ''
    try { $so = $soTask.Result } catch {}
    try { $se = $seTask.Result } catch {}
    $rc = if ($timedOut) { 'TIMEOUT' } else { [string]$p.ExitCode }
    $out = ''
    if (Test-Path $outP) { $out = [System.IO.File]::ReadAllText($outP) }
    # Two fields can never match by construction: A_AhkExe names the engine that
    # ran the script, and the corpus writes each side's own workdir into out.txt.
    # Tokenise exactly those two, nothing else - A_AhkVersion and every error
    # field stay under strict comparison.
    $out = $out.Replace($wd, '<WD>')
    $out = $out.Replace($exe, '<EXE>')
    return [pscustomobject]@{ rc = $rc; out = $out; stdout = $so; stderr = $se; dialogs = $dlg; timedOut = $timedOut }
}

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('case,deck_rc,off_rc,deck_out_bytes,off_out_bytes,deck_dialogs,verdict')
$pass = 0
$diff = 0
$officialDialog = 0

foreach ($corpus in $corpora) {
    foreach ($f in (Get-ChildItem $corpus -Filter '*.ahk' | Sort-Object Name)) {
        $case = $f.BaseName
        $deckWd = Join-Path $base (Join-Path $case 'deck')
        $offWd = Join-Path $base (Join-Path $case 'off')
        foreach ($d in @($deckWd, $offWd)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        $deckScript = Join-Path $deckWd $f.Name
        $offScript = Join-Path $offWd $f.Name
        Copy-Item $f.FullName $deckScript -Force
        Copy-Item $f.FullName $offScript -Force

        $d = Run-Engine $deckExe $true $deckScript $deckWd
        $o = Run-Engine $offExe $false $offScript $offWd

        $verdict = 'PASS'
        if ($d.dialogs -gt 0) { $verdict = 'DIALOG_DECK' }
        elseif ($o.timedOut -and $o.dialogs -gt 0) { $verdict = 'DIALOG_OFFICIAL' }
        elseif ($d.rc -ne $o.rc) { $verdict = 'DIFF_RC' }
        elseif ($d.out -cne $o.out) { $verdict = 'DIFF_OUT' }
        switch ($verdict) {
            'PASS' { $pass++ }
            'DIALOG_OFFICIAL' { $officialDialog++; $pass++ }
            default { $diff++ }
        }
        $rows += ('{0},{1},{2},{3},{4},{5},{6}' -f $case, $d.rc, $o.rc, $d.out.Length, $o.out.Length, $d.dialogs, $verdict)
    }
}

$csv = Join-Path $base 'deck_pair.csv'
[System.IO.File]::WriteAllLines($csv, $rows, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("cases={0} pass={1} diff={2} official-dialog-cases={3}" -f ($pass + $diff), $pass, $diff, $officialDialog)
Write-Output ("csv: " + $csv)
if ($diff -gt 0) {
    $rows | Where-Object { $_ -notmatch ',PASS$' -and $_ -notmatch ',DIALOG_OFFICIAL$' } | ForEach-Object { Write-Output $_ }
    exit 1
}
exit 0
