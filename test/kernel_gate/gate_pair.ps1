# m004 gate phase B: same-script A/B run (official AutoHotkey 2.0.25 vs AHK_H v2.0.25).
#
# Both engines are the SAME version on purpose: "no difference from official"
# means capability parity at equal kernel version. Any behaviour diff left over
# is therefore a real fork difference, not an upstream feature we lack.
#
# Phase A (gate_api.ps1) checks the API surface. This phase checks BEHAVIOUR, and
# more importantly it checks that nothing fails SILENTLY:
#   - stdout/stderr are captured (Write-Output / FileAppend to stdout)
#   - out.txt written into each engine's own workdir (A_ScriptDir isolation)
#   - every top-level window of the child pid is sampled while it runs, so a
#     modal #32770 error dialog is RECORDED (title + child static text) instead of
#     being reported as a bare timeout
#   - crash exit codes are decoded (0xC0000005 etc.) instead of "-1073741819"
#   - each case/engine keeps out.txt, stdout.txt, stderr.txt, windows.txt,
#     meta.txt under .temp/m004/logs/<case>/<engine>/
#   - .temp/m004/errors.md aggregates every abnormal result for human/AI reading
#
# ASCII only (PowerShell 5 + GBK console safety).

param(
    [string]$Root = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))),
    [int]$TimeoutMs = 20000
)
$ErrorActionPreference = 'Continue'

# UI Automation is the ONLY reliable way to read AHK's error dialog body: it lives
# in a RichEdit control, and GetWindowText cannot read control text cross-process.
try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
} catch {
    Write-Output ('WARN: UI Automation unavailable, dialog bodies will be empty: ' + $_.Exception.Message)
}

Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public class GateWin {
  public delegate bool EnumCb(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumCb cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
"@

$corpora = @(
    (Join-Path $Root '.temp\m002\armB\corpus'),
    (Join-Path $Root '.temp\m004\corpus\edge')
)
$base = Join-Path $Root '.temp\m004\pair'
$logs = Join-Path $Root '.temp\m004\logs'
$csvPath = Join-Path $base 'pair_results.csv'
$errPath = Join-Path $Root '.temp\m004\errors.md'
foreach ($d in @($base, $logs)) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

$engines = [ordered]@{
    off = Join-Path $Root '.3rd\AutoHotkey_2.0.25\AutoHotkey64.exe'
    h   = Join-Path $Root '.temp\ahkh_build\AutoHotkey64.exe'
}
foreach ($k in $engines.Keys) {
    if (-not (Test-Path $engines[$k])) { throw "engine missing: $($engines[$k])" }
}

# --- helpers ---------------------------------------------------------------

function Fmt-Rc($rc) {
    if ("$rc" -eq 'TIMEOUT') { return 'TIMEOUT' }
    $i = [int32]$rc
    if ($i -lt 0) {
        $u = [System.BitConverter]::ToUInt32([System.BitConverter]::GetBytes($i), 0)
        return ('0x{0:X8}' -f $u)
    }
    return [string]$i
}

function Classify([string]$rcText, [bool]$timedOut, [string]$wins) {
    if ($timedOut) {
        if ($wins -ne '') { return 'DIALOG_BLOCK' }
        return 'HANG_NO_WINDOW'
    }
    if ($rcText -eq '0xC0000005') { return 'CRASH_AV' }
    if ($rcText -eq '0xC0000409') { return 'CRASH_FASTFAIL' }
    if ($rcText -eq '0xC00000FD') { return 'CRASH_STACK_OVERFLOW' }
    if ($wins -ne '') { return 'DIALOG' }
    if ($rcText -match '^0x8') { return 'EXIT_HRESULT' }
    return 'OK'
}

# Sample every top-level window owned by $procId; for a #32770 also read the body
# through UI Automation (the message lives in a RichEdit control).
function Read-DialogUia([IntPtr]$hwnd) {
    $txt = ''
    try {
        $el = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        if ($el) {
            $all = $el.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                               [System.Windows.Automation.Condition]::TrueCondition)
            $sb = New-Object System.Text.StringBuilder
            foreach ($c in $all) {
                try {
                    $tp = $c.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
                    if ($tp) {
                        $t = $tp.DocumentRange.GetText(6000)
                        if ($t) { [void]$sb.Append(($t -replace "`r`n", "`n").Trim() + "`n") }
                    }
                } catch {}
            }
            $txt = $sb.ToString().Trim()
        }
    } catch {}
    return $txt
}

function Get-ProcWindows([int]$procId) {
    $script:winLines = New-Object System.Collections.Generic.List[string]
    $cb = [GateWin+EnumCb]{
        param($hwnd, $lp)
        [uint32]$wp = 0
        [void][GateWin]::GetWindowThreadProcessId($hwnd, [ref]$wp)
        if ($wp -ne $procId) { return $true }
        $cn = New-Object System.Text.StringBuilder 128
        [void][GateWin]::GetClassName($hwnd, $cn, 128)
        $cls = $cn.ToString()
        $vis = [GateWin]::IsWindowVisible($hwnd)
        $t = New-Object System.Text.StringBuilder 1024
        [void][GateWin]::GetWindowText($hwnd, $t, 1024)
        $title = $t.ToString().Trim()
        if (-not $vis -and $cls -ne '#32770') { return $true }
        $script:winLines.Add("[top:$cls] $title")
        if ($cls -eq '#32770') {
            $key = [string]$hwnd
            if (-not $script:uiaSeen.ContainsKey($key)) {
                $script:uiaSeen[$key] = $true
                $u = Read-DialogUia $hwnd
                if ($u -ne '') { $script:uiaTexts.Add($u) }
            }
            $ccb = [GateWin+EnumCb]{
                param($ch, $lp2)
                $c2 = New-Object System.Text.StringBuilder 128
                [void][GateWin]::GetClassName($ch, $c2, 128)
                $t2 = New-Object System.Text.StringBuilder 2048
                [void][GateWin]::GetWindowText($ch, $t2, 2048)
                $txt = $t2.ToString().Trim()
                if ($txt -ne '') {
                    if ($txt.Length -gt 400) { $txt = $txt.Substring(0, 400) + '...' }
                    $script:winLines.Add("  [child:$($c2.ToString())] " + ($txt -replace "`r`n", ' / '))
                }
                return $true
            }
            [void][GateWin]::EnumChildWindows($hwnd, $ccb, [IntPtr]::Zero)
        }
        return $true
    }
    [void][GateWin]::EnumWindows($cb, [IntPtr]::Zero)
    return ($script:winLines -join "`n")
}

function ReadText([string]$p) {
    if (Test-Path $p) { return [IO.File]::ReadAllText($p) }
    return ''
}

function WriteText([string]$p, [string]$t) {
    [IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($false)))
}

function Normalize([string]$t, [string]$wd) {
    if ($null -eq $t) { return '' }
    $t = $t.Replace($wd, '<WD>')
    $t = $t.Replace($Root, '<ROOT>')
    # Only the engine PATH may differ. A_AhkVersion is NOT normalized: both
    # engines are v2.0.25, so a version mismatch must surface as a diff.
    $t = [regex]::Replace($t, '(?m)^AHKEXE=.*$', 'AHKEXE=<EXE>')
    $t = $t.Replace("`r`n", "`n")
    return $t
}

# --- run --------------------------------------------------------------------

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('case,off_rc,off_cls,h_rc,h_cls,verdict,detail,off_ms,h_ms,off_dlg,h_dlg')
$total = 0; $pass = 0; $bad = 0; $dlgCount = 0
$errLines = New-Object System.Collections.Generic.List[string]
$errLines.Add('# m004 pair run - abnormal results')
$errLines.Add('')
$errLines.Add('Generated by `.main/test/kernel_gate/gate_pair.ps1`.')
$errLines.Add('Every case below had a dialog, a crash, a hang, or stderr output on at least one engine.')
$errLines.Add('')
$errLines.Add('Raw artifacts: `.temp/m004/logs/<case>/<engine>/`.')
$errLines.Add('')

foreach ($corpus in $corpora) {
    if (-not (Test-Path $corpus)) { continue }
    foreach ($f in (Get-ChildItem $corpus -Filter '*.ahk' | Sort-Object Name)) {
        $case = $f.BaseName
        $total++
        $res = @{}
        foreach ($k in $engines.Keys) {
            $wd = Join-Path $logs (Join-Path $case $k)
            New-Item -ItemType Directory -Force -Path $wd | Out-Null
            $scriptCopy = Join-Path $wd $f.Name
            Copy-Item $f.FullName $scriptCopy -Force
            $outP = Join-Path $wd 'out.txt'
            if (Test-Path $outP) { Remove-Item $outP -Force }

            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $engines[$k]
            $psi.Arguments = '/ErrorStdOut=UTF-8 "' + $scriptCopy + '"'
            $psi.WorkingDirectory = $wd
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.CreateNoWindow = $false

            $sw = [Diagnostics.Stopwatch]::StartNew()
            $p = [System.Diagnostics.Process]::Start($psi)
            $soTask = $p.StandardOutput.ReadToEndAsync()
            $seTask = $p.StandardError.ReadToEndAsync()

            $seen = New-Object System.Collections.Generic.List[string]
            $script:uiaSeen = @{}
            $script:uiaTexts = New-Object System.Collections.Generic.List[string]
            while (-not $p.HasExited -and $sw.ElapsedMilliseconds -lt $TimeoutMs) {
                $w = Get-ProcWindows $p.Id
                foreach ($ln in ($w -split "`n")) {
                    if ($ln -ne '' -and -not $seen.Contains($ln)) { $seen.Add($ln) }
                }
                Start-Sleep -Milliseconds 120
            }
            $timedOut = $false
            if (-not $p.HasExited) {
                $timedOut = $true
                try { $p.Kill() } catch {}
                [void]$p.WaitForExit(5000)
            }
            $sw.Stop()
            $ms = $sw.ElapsedMilliseconds
            $rcRaw = if ($timedOut) { 'TIMEOUT' } else { $p.ExitCode }
            $rcText = Fmt-Rc $rcRaw
            $so = ''; $se = ''
            try { $so = $soTask.Result } catch {}
            try { $se = $seTask.Result } catch {}
            $winText = ($seen -join "`n")
            $uiaText = ($script:uiaTexts -join "`n---`n")
            $outText = ReadText $outP

            WriteText (Join-Path $wd 'stdout.txt') $so
            WriteText (Join-Path $wd 'stderr.txt') $se
            WriteText (Join-Path $wd 'windows.txt') $winText
            WriteText (Join-Path $wd 'dialog_uia.txt') $uiaText
            $cls = Classify $rcText $timedOut ($winText + $uiaText)
            $meta = "case=$case`nengine=$k`nexe=$($engines[$k])`nrc=$rcRaw`nrc_hex=$rcText`nclass=$cls`ntimed_out=$timedOut`nelapsed_ms=$ms`nout_bytes=$($outText.Length)`nstdout_bytes=$($so.Length)`nstderr_bytes=$($se.Length)`ndialog_bytes=$($winText.Length)`nuia_bytes=$($uiaText.Length)`n"
            WriteText (Join-Path $wd 'meta.txt') $meta

            $res[$k] = @{
                rc = $rcText; cls = $cls; ms = $ms; out = $outText
                so = $so; se = $se; win = $winText; uia = $uiaText; timedOut = $timedOut
            }
        }

        $o = $res['off']; $h = $res['h']
        $nOff = Normalize $o.out (Join-Path $logs (Join-Path $case 'off'))
        $nH   = Normalize $h.out (Join-Path $logs (Join-Path $case 'h'))
        $sOff = Normalize ($o.so + $o.se) (Join-Path $logs (Join-Path $case 'off'))
        $sH   = Normalize ($h.so + $h.se) (Join-Path $logs (Join-Path $case 'h'))

        $verdict = 'PASS'
        if ($o.timedOut -and $h.timedOut) { $verdict = 'HANG_BOTH' }
        elseif ($o.timedOut -or $h.timedOut) { $verdict = 'HANG_ONE' }
        elseif ($o.rc -ne $h.rc) { $verdict = 'DIFF_RC' }
        elseif ($nOff -ne $nH) { $verdict = 'DIFF_OUT' }
        elseif ($sOff -ne $sH) { $verdict = 'DIFF_STREAM' }
        if ($verdict -eq 'PASS') { $pass++ } else { $bad++ }

        $dOff = if ($o.win -ne '' -or $o.uia -ne '') { 'yes' } else { 'no' }
        $dH   = if ($h.win -ne '' -or $h.uia -ne '') { 'yes' } else { 'no' }
        $detail = 'rc ' + $o.rc + '/' + $h.rc
        if ($nOff -ne $nH) { $detail += ' | out differs' }
        if ($sOff -ne $sH) { $detail += ' | stream differs' }
        if ($o.timedOut -or $h.timedOut) { $detail += ' | timeout ' + $o.ms + 'ms/' + $h.ms + 'ms' }
        $line = '"' + $case + '",' + $o.rc + ',' + $o.cls + ',' + $h.rc + ',' + $h.cls + ',' + $verdict + ',"' + $detail + '",' + $o.ms + ',' + $h.ms + ',' + $dOff + ',' + $dH
        if ($dOff -eq 'yes' -or $dH -eq 'yes') { $dlgCount++ }
        $rows.Add($line)
        Write-Output ("{0,-26} {1,-12} off={2,-10} h={3,-10}" -f $case, $verdict, $o.rc, $h.rc)

        # aggregate abnormal results for human/AI reading
        if ($o.cls -ne 'OK' -or $h.cls -ne 'OK' -or $o.se -ne '' -or $h.se -ne '' -or $verdict -ne 'PASS' -or $dOff -eq 'yes' -or $dH -eq 'yes') {
            $errLines.Add("## $case  -> $verdict")
            $errLines.Add('')
            foreach ($k in 'off','h') {
                $r = $res[$k]
                $errLines.Add("- **$k**: rc=$($r.rc) class=$($r.cls) $($r.ms)ms")
                if ($r.uia -ne '') {
                    $errLines.Add('  - error dialog body (UI Automation):')
                    $errLines.Add('')
                    $errLines.Add('    ```')
                    foreach ($ul in ($r.uia -split "`n")) { $errLines.Add('    ' + $ul) }
                    $errLines.Add('    ```')
                }
                if ($r.win -ne '') {
                    $errLines.Add('  - dialog/window:')
                    $errLines.Add('')
                    $errLines.Add('    ```')
                    foreach ($wl in ($r.win -split "`n")) { $errLines.Add('    ' + $wl) }
                    $errLines.Add('    ```')
                }
                if ($r.se -ne '') {
                    $errLines.Add('  - stderr:')
                    $errLines.Add('')
                    $errLines.Add('    ```')
                    foreach ($sl in (($r.se -replace "`r`n", "`n") -split "`n")) {
                        if ($sl.Trim() -ne '') { $errLines.Add('    ' + $sl) }
                    }
                    $errLines.Add('    ```')
                }
                if ($r.out -ne '') {
                    $tail = $r.out
                    if ($tail.Length -gt 800) { $tail = '...' + $tail.Substring($tail.Length - 800) }
                    $errLines.Add('  - out.txt tail:')
                    $errLines.Add('')
                    $errLines.Add('    ```')
                    foreach ($ol in (($tail -replace "`r`n", "`n") -split "`n")) { $errLines.Add('    ' + $ol) }
                    $errLines.Add('    ```')
                }
            }
            $errLines.Add('')
        }
    }
}

WriteText $csvPath ($rows -join "`n")
WriteText $errPath ($errLines -join "`n")

Write-Output ("SUMMARY total={0} pass={1} abnormal={2} cases_with_modal_dialog={3}" -f $total, $pass, $bad, $dlgCount)
Write-Output ('CSV     -> ' + $csvPath)
Write-Output ('ERRORS  -> ' + $errPath)
Write-Output ('ARTIFACTS -> ' + $logs)
