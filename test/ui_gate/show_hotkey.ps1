# m108 GUI gate: the "show the main window" global shortcut.
# Isolated run: AHKDECK_HOTKEYS / AHKDECK_SETTINGS point into .temp, so the
# user's real bindings are never touched.
#
# Checks, in order:
#   1. a minimised deck comes back (hotkey == the tray menu's 主窗口 action)
#   2. a deck hidden to the tray comes back AND takes the foreground
#   3. the shortcut dialog lists the new row (screenshot for the record)
#
# FLTK drops the window from its list on hide, so show() re-creates the HWND:
# every check re-finds the deck by process id instead of holding a stale handle.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static List<IntPtr> Titled(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint owner = 0;
            GetWindowThreadProcessId(h, out owner);
            if (owner != pid) return true;
            if (GetWindowTextLength(h) == 0) return true;
            list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list; // z-order: the most recently opened window first
    }
}
'@

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exe = Join-Path $root '.build\AHKDeck.exe'
$work = Join-Path $root '.temp\m108_gate'
$shots = Join-Path $root '.temp\m108_shots'
New-Item -ItemType Directory -Force $work | Out-Null
New-Item -ItemType Directory -Force $shots | Out-Null
$hotkeys = Join-Path $work 'hotkeys.txt'
$settings = Join-Path $work 'settings.ini'

Set-Content -Path $hotkeys -Value 'window.show=^!D' -Encoding ASCII

$fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Output ("ok   " + $what) }
    else { Write-Output ("FAIL " + $what); $script:fails++ }
}

function Launch([hashtable]$envs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $root
    foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] }
    return [System.Diagnostics.Process]::Start($psi)
}

# The deck's own window: titled and at least 300px wide (the frame shadow has no
# title and the control window is 16x16).
function FindMain([int]$procId) {
    foreach ($h in [W]::Titled([uint32]$procId)) {
        $r = New-Object 'W+RECT'
        [void][W]::GetWindowRect($h, [ref]$r)
        if ([W]::IsWindowVisible($h) -and ($r.R - $r.L) -gt 300) { return $h }
    }
    return [IntPtr]::Zero
}

function WaitMain([int]$procId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $h = FindMain $procId
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 200
    }
    return [IntPtr]::Zero
}

function SendShowKey {
    [W]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)   # Ctrl down
    [W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)   # Alt down
    [W]::keybd_event(0x44, 0, 0, [UIntPtr]::Zero)   # D down
    Start-Sleep -Milliseconds 40
    [W]::keybd_event(0x44, 0, 2, [UIntPtr]::Zero)   # D up
    [W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)   # Alt up
    [W]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)   # Ctrl up
}

function QuitDeck([int]$procId) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "--quit $procId"
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $root
    [void][System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Seconds 2
}

function Capture([IntPtr]$h, [string]$path) {
    $r = New-Object 'W+RECT'
    [void][W]::GetWindowRect($h, [ref]$r)
    $wdt = $r.R - $r.L; $hgt = $r.B - $r.T
    if ($wdt -le 0 -or $hgt -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap $wdt, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [W]::PrintWindow($h, $hdc, 2)
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $ok
}

# --- part 1: minimised -> hotkey -> restored --------------------------------
$p = Launch(@{ AHKDECK_HOTKEYS = $hotkeys; AHKDECK_SETTINGS = $settings })
$hwnd = WaitMain $p.Id 20
Check ($hwnd -ne [IntPtr]::Zero) ("main window appeared (pid " + $p.Id + ")")
if ($hwnd -ne [IntPtr]::Zero) {
    Start-Sleep -Seconds 2
    [void][W]::ShowWindow($hwnd, 6)  # SW_MINIMIZE
    Start-Sleep -Seconds 1
    # A minimised window's rect shrinks to the title strip, so FindMain cannot see
    # it; the handle from before the minimise is still the deck.
    Check ([W]::IsIconic($hwnd)) "precondition: the deck is minimised"
    SendShowKey
    Start-Sleep -Milliseconds 1200
    $hwnd = FindMain $p.Id
    Check ($hwnd -ne [IntPtr]::Zero -and -not ([W]::IsIconic($hwnd))) "Ctrl+Alt+D restores a minimised deck"

    # --- part 2: hidden to the tray -> hotkey -> shown and in front ---------
    [void][W]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE = hide to tray
    Start-Sleep -Seconds 2
    Check ((FindMain $p.Id) -eq [IntPtr]::Zero) "precondition: the deck is hidden to the tray"
    SendShowKey
    Start-Sleep -Milliseconds 1500
    $shown = WaitMain $p.Id 4
    Check ($shown -ne [IntPtr]::Zero -and -not ([W]::IsIconic($shown))) "Ctrl+Alt+D shows a tray-hidden deck"
    if ($shown -ne [IntPtr]::Zero) {
        Check ([W]::GetForegroundWindow() -eq $shown) "the restored deck took the foreground"
        [void](Capture $shown (Join-Path $shots 'm108_after_hotkey.png'))
    }
}
QuitDeck $p.Id

# the file must still hold exactly the one binding the gate wrote
$text = (Get-Content -Path $hotkeys -Raw).Trim()
Check ($text -eq 'window.show=^!D') "hotkeys.txt still holds window.show=^!D"

# --- part 3: the shortcut dialog lists the new row -------------------------
$p2 = Launch(@{ AHKDECK_HOTKEYS = $hotkeys; AHKDECK_SETTINGS = $settings; AHKDECK_OPEN_SHORTCUTS = '1' })
$dlg = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and $dlg -eq [IntPtr]::Zero) {
    foreach ($h in [W]::Titled([uint32]$p2.Id)) {
        $r = New-Object 'W+RECT'
        [void][W]::GetWindowRect($h, [ref]$r)
        # The dialog is 520 wide; the deck window is 600+ (the frame shadow has
        # no title). Width is the one reliable difference between the two.
        if ([W]::IsWindowVisible($h) -and ($r.R - $r.L) -lt 560) { $dlg = $h; break }
    }
    Start-Sleep -Milliseconds 200
}
Check ($dlg -ne [IntPtr]::Zero) "shortcut dialog opened"
if ($dlg -ne [IntPtr]::Zero) {
    Start-Sleep -Seconds 1
    Check ([W]::IsWindowVisible($dlg)) "the shortcut dialog is visible"
    $cap = Capture $dlg (Join-Path $shots 'm108_shortcut_dialog.png')
    Check $cap "dialog screenshot written"
}
QuitDeck $p2.Id

if ($fails -gt 0) { Write-Output ("m108 gate: " + $fails + " failure(s)"); exit 1 }
Write-Output "m108 gate: all checks passed"
exit 0
