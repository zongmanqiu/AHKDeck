# m131 GUI gate: the tray menu's Running submenu + the script's own menu.
# Isolated run: AHKDECK_SETTINGS / AHKDECK_HOTKEYS / AHKDECK_LIBRARY point into
# .temp; the probe script lives in .build\Scripts for the run and is removed at
# the end (m016/m023 convention: user scripts are stashed away, not deleted).
#
# The tray menu is a native Win32 menu (class #32768) driven by a modal loop.
# Facts that shape the driving technique (all earned the hard way):
#   - A real tray click makes the shell grant SetForegroundWindow rights to the
#     deck, so its menu opens with a solid foreground. A gate that only POSTS
#     the tray message has no grant: SetForegroundWindow fails and the menu
#     lives on a borrowed foreground that ANY activation change dismisses
#     (runs 3/4 died within 150 ms; run 2 only got lucky inside that race).
#     Fix: show the 16x16 control window, attach our input queue to the deck's
#     thread (AttachThreadInput) and SetForegroundWindow it - a legitimate
#     foreground hand-over that mirrors what the shell does. Hide it again
#     after each arm.
#   - Posted messages need no foreground at all (same ground as m083's
#     PostMessage WM_CHAR driving): WM_KEYDOWN/WM_KEYUP go straight to the
#     menu window's thread queue and the modal loop consumes them.
#   - A pick is asserted as IsWindow(top)==false, never as a zero-menu instant:
#     the next menu (the script's own) can appear between two polls.
#   - "the probe is running" is read from its hidden main window (kernel class
#     AutoHotkey), not from a marker: the ^!+F9 hotkey TOGGLES, and a retry
#     after the kill arm must be able to start it again.
# Arm A only proves the empty-deck menu opens without a submenu and records a
# screenshot; the greyed 运行中 in that PNG is judged by eye (m079 precedent
# for machine-unreadable UI facts).
#
# Arms, in order:
#   A. empty deck: tray menu opens, screenshot for the record, ESC closes it.
#   B. probe running: tray -> Running -> script item -> the script's OWN menu
#      pops -> its only item runs and writes a marker file.
#   C. probe killed behind the open menu: picking its stale row must NOT pop a
#      menu and must NOT crash - the m122e channel's "not running" guard takes
#      the pick.
param([switch]$Capture, [string]$Arms = 'ABC')
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
# NOTE: no UIAutomationClient here on purpose. Loading the UIA client changed
# how the desktop treats native menus: with it loaded every menu died within
# 150-400 ms (runs 3-5), without it menus live long enough to drive (run 2,
# full chain green). Arm A therefore records a screenshot instead of reading
# items programmatically; the greyed empty state is judged from that PNG.

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint idThread, ref GUITHREADINFO info);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags; public IntPtr hwndActive; public IntPtr hwndFocus;
        public IntPtr hwndCapture; public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize;
        public IntPtr hwndCaret; public RECT rcCaret;
    }
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static List<IntPtr> OfClass(uint pid, string cls) {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint owner = 0;
            GetWindowThreadProcessId(h, out owner);
            if (pid != 0 && owner != pid) return true;
            var sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (sb.ToString() != cls) return true;
            list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }
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
        return list;
    }
}
'@

$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exe = Join-Path $root '.build\AHKDeck.exe'
$work = Join-Path $root '.temp\m131_gate'
$shots = Join-Path $root '.temp\ui_gate_shots'
$scripts = Join-Path $root '.build\Scripts'
$stash = Join-Path $work 'scripts-stash'
New-Item -ItemType Directory -Force $work | Out-Null
New-Item -ItemType Directory -Force $shots | Out-Null
New-Item -ItemType Directory -Force $stash | Out-Null

$probe = Join-Path $scripts 'm131-probe.ahk'
$markAction = Join-Path $scripts 'm131-probe-action.marker'
$hotkeys = Join-Path $work 'hotkeys.txt'
$settings = Join-Path $work 'settings.ini'

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

function Stop-Deck([System.Diagnostics.Process]$p) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = '--quit ' + $p.Id
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void][System.Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit(8000)) { throw "deck (pid $($p.Id)) did not exit within 8 s of --quit" }
}

function WaitMain([int]$procId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        foreach ($h in [W]::Titled([uint32]$procId)) {
            $r = New-Object 'W+RECT'
            [void][W]::GetWindowRect($h, [ref]$r)
            if ([W]::IsWindowVisible($h) -and ($r.R - $r.L) -gt 300) { return $true }
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

# the hidden control window (class AHKDeckControl) carries the tray callback
function FindControl([int]$procId) {
    $list = [W]::OfClass([uint32]$procId, 'AHKDeckControl')
    if ($list.Count -gt 0) { return $list[0] }
    return [IntPtr]::Zero
}

# native menu popups; both the deck's menu and a script's menu live in the deck
# process (the engine is in-process), so the pid filter is the whole story
function Menus([int]$procId) {
    return [W]::OfClass([uint32]$procId, '#32768')
}

function WaitMenus([int]$procId, [int]$min, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ((Menus $procId).Count -ge $min) { return $true }
        Start-Sleep -Milliseconds 80
    }
    return $false
}

function Wait-HwndGone([IntPtr]$h, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (-not [W]::IsWindow($h)) { return $true }
        Start-Sleep -Milliseconds 80
    }
    return $false
}

# The menu owns keyboard capture while the deck process holds the foreground
# (FocusControl above), so REAL key injection reaches it exactly like a user's
# keypress. Two hard-won details: the events must carry real SCANCODES (a
# scancode-0 keybd_event never drove the menu - run 7), and the first key after
# the menu opens is swallowed (the sacrificial SHIFT below absorbs that; and
# never SetFocus the menu window - that dismisses it, diag v2).
function SendMenuKey([byte]$vk, [byte]$scan, [int]$settleMs) {
    [W]::keybd_event($vk, $scan, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [W]::keybd_event($vk, $scan, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $settleMs
}

# dismiss anything the previous attempt left open
function DismissMenus([int]$procId) {
    if ((Menus $procId).Count -gt 0) {
        SendMenuKey 0x1B 0x01 150   # ESC
        SendMenuKey 0x1B 0x01 150   # ESC again: submenu + top level
    }
}

# The shell grants the deck foreground rights after a real tray click; the gate
# has no click, so it builds the same state by hand: show the control window
# (16x16 tool window), join its thread's input queue, and foreground it.
# THE JOIN STAYS ON for the whole interaction - the diag proved that injected
# keys only drive the menu while the two threads share one input queue
# ("queues still joined"); detaching right after the hand-over (an earlier
# version) leaves the menu deaf to every key.
function FocusControl([IntPtr]$ctl) {
    [void][W]::ShowWindow($ctl, 5)   # SW_SHOW
    Start-Sleep -Milliseconds 100
    $dummyPid = [uint32]0
    $script:deckThreadId = [W]::GetWindowThreadProcessId($ctl, [ref]$dummyPid)
    $my = [W]::GetCurrentThreadId()
    [void][W]::AttachThreadInput($my, $script:deckThreadId, $true)
    [void][W]::BringWindowToTop($ctl)
    [void][W]::SetForegroundWindow($ctl)
    [void][W]::SetFocus($ctl)
    Start-Sleep -Milliseconds 100
}

function UnfocusControl([int]$procId) {
    if ($script:deckThreadId) {
        $my = [W]::GetCurrentThreadId()
        [void][W]::AttachThreadInput($my, $script:deckThreadId, $false)
        $script:deckThreadId = 0
    }
    HideControl $procId
}

function HideControl([int]$procId) {
    $ctl = FindControl $procId
    if ($ctl -ne [IntPtr]::Zero) { [void][W]::ShowWindow($ctl, 0) }  # SW_HIDE
}

# Hide the main window first (WM_CLOSE = hide to tray, m023): the tray menu is
# a tray thing - real users reach it with the window hidden, and a visible main
# window under the menu is a hover trap (the diag only worked because its
# cursor happened to sit off the window). The global hotkey still reaches the
# deck while hidden (m108), so the probe start is unaffected.
function HideMain([int]$procId) {
    foreach ($h in [W]::Titled([uint32]$procId)) {
        $r = New-Object 'W+RECT'
        [void][W]::GetWindowRect($h, [ref]$r)
        if ([W]::IsWindowVisible($h) -and ($r.R - $r.L) -gt 300) {
            [void][W]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
        }
    }
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        $visible = $false
        foreach ($h in [W]::Titled([uint32]$procId)) {
            $r = New-Object 'W+RECT'
            [void][W]::GetWindowRect($h, [ref]$r)
            if ([W]::IsWindowVisible($h) -and ($r.R - $r.L) -gt 300) { $visible = $true }
        }
        if (-not $visible) { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function SendStartHotkey {
    # ^!+F9, the probe's start binding (m015 lesson: let the modifiers settle)
    [W]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
    [W]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
    [W]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    [W]::keybd_event(0x78, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [W]::keybd_event(0x78, 0, 2, [UIntPtr]::Zero)
    [W]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero)
    [W]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [W]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
}

# the ONLY trustworthy "probe running" signal is an AutoHotkey-class window
# owned by the deck process: the in-process kernel has NO resident window of
# its own (micro-test: empty before the hotkey, one window after), so window
# presence ⇔ a script is really running. Markers cannot do this job - the
# probe's hwnd write came out EMPTY (A_ScriptHwnd is useless in embedded mode)
# and a bare file-exists test raced with stop/start retries.
function ProbeAlive([int]$procId) {
    return ([W]::OfClass([uint32]$procId, 'AutoHotkey')).Count -ge 1
}

function StartProbe([int]$procId) {
    if (ProbeAlive $procId) { return $true }
    for ($k = 1; $k -le 3; $k++) {
        SendStartHotkey
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline) {
            if (ProbeAlive $procId) { return $true }
            Start-Sleep -Milliseconds 80
        }
    }
    return $false
}

function OpenTrayMenu([IntPtr]$ctl) {
    $msg = [W]::RegisterWindowMessage('AHKDeckTrayIcon')
    return [W]::PostMessage($ctl, $msg, [IntPtr]::Zero, [IntPtr]0x0205)  # WM_RBUTTONUP
}

function WaitMarker([string]$path, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $path) { return $true }
        Start-Sleep -Milliseconds 80
    }
    return $false
}

function ClearMarkers {
    if (Test-Path $markAction) { Remove-Item -LiteralPath $markAction -Force }
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

# m133: a popup menu reserves a check-mark column even when nothing in it can be
# checked, which read as a ~38 px blank strip before the text (the deck now sets
# MNS_NOCHECK where no item carries a check or an image). Measure the first column
# that carries glyph pixels, skipping the window border at x<=8.
function TextStartX([string]$path) {
    $bmp = [System.Drawing.Bitmap]::FromFile($path)
    try {
        for ($x = 9; $x -lt $bmp.Width; $x++) {
            $dark = 0
            for ($y = 0; $y -lt $bmp.Height; $y++) {
                $c = $bmp.GetPixel($x, $y)
                if ($c.R -lt 200 -and $c.G -lt 200 -and $c.B -lt 200) { $dark++ }
            }
            if ($dark -ge 4) { return $x }
        }
        return -1
    } finally { $bmp.Dispose() }
}

# UIA-free arms only: the menu structure is recorded as a PNG for the record
# (PrintWindow on a popup; safe here because the menu is idle while we wait).

# --- common setup: stash user scripts, write the probe -----------------------
# a crashed earlier run can leave a probe (or the user's scripts) in the stash;
# clear probe leftovers and move with -Force so nothing collides
foreach ($n in @('m131-probe.ahk', 'm131-probe-action.marker')) {
    foreach ($d in @($scripts, $stash)) {
        $p = Join-Path $d $n
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Force }
    }
}
$stashed = @()
foreach ($f in @(Get-ChildItem (Join-Path $scripts '*.ahk') -ErrorAction SilentlyContinue)) {
    Move-Item -LiteralPath $f.FullName -Destination $stash -Force
    $stashed += (Join-Path $stash $f.Name)   # the path AFTER the move
}
$utf8 = New-Object System.Text.UTF8Encoding($false)

[System.IO.File]::WriteAllText($probe, @'
#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon
A_TrayMenu.Delete()
A_TrayMenu.Add("TRAYMENU-PROBE", OnProbe)
A_TrayMenu.Default := "TRAYMENU-PROBE"
OnProbe(aName, aPos, aMenu) {
    FileAppend("ok", A_ScriptDir "\m131-probe-action.marker")
}
Keep() {
}
SetTimer Keep, 500

'@, $utf8)

[System.IO.File]::WriteAllText($hotkeys, "m131-probe.ahk=^!+F9`r`n", $utf8)
$envs = @{ AHKDECK_SETTINGS = $settings; AHKDECK_HOTKEYS = $hotkeys;
    AHKDECK_LIBRARY = (Join-Path $work 'library') }

ClearMarkers
$pA = $null; $pB = $null; $pC = $null; $script:deckThreadId = 0

# Preflight: a leftover deck (crashed run, user's own instance) holds the
# single-instance mutex and bounces every launch we make - the arms would then
# "succeed" against the WRONG deck (m088 trap, seen again in m131). Fail loud.
$leftover = @(Get-Process -Name AHKDeck -ErrorAction SilentlyContinue)
if ($leftover.Count -gt 0) {
    $ids = ($leftover | ForEach-Object { $_.Id }) -join ','
    throw "another AHKDeck is already running (pid(s) $ids) - close it (tray Exit) or quit it before the gate"
}

try {
    if ($Arms -match 'A') {
    # --- arm A: nothing running -> the menu opens; grey state goes to PNG ----
    # The greyed 运行中 is asserted from the screenshot (m079 precedent: items
    # a machine gate cannot read honestly are recorded and judged by eye).
    $pA = Launch $envs
    Check (WaitMain $pA.Id 20) "A: deck launched (pid $($pA.Id))"
    Start-Sleep -Seconds 2
    Check (HideMain $pA.Id) "A: main window hidden to the tray (real tray usage)"
    $ctlA = FindControl $pA.Id
    Check ($ctlA -ne [IntPtr]::Zero) "A: control window found"
    $openOk = $false
    if ($ctlA -ne [IntPtr]::Zero) {
        for ($try = 1; $try -le 3 -and -not $openOk; $try++) {
            FocusControl $ctlA
                        if (-not (OpenTrayMenu $ctlA)) { break }
            if (-not (WaitMenus $pA.Id 1 3)) { continue }
            $openOk = $true
            $top = (Menus $pA.Id)[0]
                        Start-Sleep -Milliseconds 600   # well past the swallow-window, deterministic
            [void](Capture $top (Join-Path $shots 'm131_tray_menu_empty.png'))
            SendMenuKey 0x10 0x2A 100   # sacrificial SHIFT (first key is swallowed)
            SendMenuKey 0x1B 0x01 150   # ESC closes the menu
            DismissMenus $pA.Id
        }
        UnfocusControl $pA.Id
    }
    Check $openOk "A: the empty-deck tray menu opened (screenshot: .temp\ui_gate_shots\m131_tray_menu_empty.png)"
    Stop-Deck $pA
    }

    if ($Arms -match 'B') {
    # --- arm B: probe running -> full chain, the script's own menu acts ------
    $pB = Launch $envs
    Check (WaitMain $pB.Id 20) "B: deck launched (pid $($pB.Id))"
    Start-Sleep -Seconds 2
    Check (HideMain $pB.Id) "B: main window hidden to the tray"
    $startedB = StartProbe $pB.Id
    if (-not $startedB) {
        Write-Output ("     B dbg: probe alive=" + (ProbeAlive $pB.Id) +
            " ahk-class windows=" + ([W]::OfClass([uint32]$pB.Id, 'AutoHotkey')).Count)
    }
    Check $startedB "B: ^!+F9 started m131-probe.ahk"
    $ctlB = FindControl $pB.Id
    $subOk = $false; $ownOk = $false; $actOk = $false; $padOk = $false
    if ($ctlB -ne [IntPtr]::Zero) {
        for ($try = 1; $try -le 3 -and -not $actOk; $try++) {
            # clear the ACTION marker only - the start marker is the liveness
            # signal and must survive the retry loop
            if (Test-Path $markAction) { Remove-Item -LiteralPath $markAction -Force }
            FocusControl $ctlB
                        if (-not (OpenTrayMenu $ctlB)) { break }
            if (-not (WaitMenus $pB.Id 1 3)) { continue }
            $top = (Menus $pB.Id)[0]
            Start-Sleep -Milliseconds 600   # well past the swallow-window
            [void](Capture $top (Join-Path $shots 'm131_tray_menu_open.png'))
            $textX = TextStartX (Join-Path $shots 'm131_tray_menu_open.png')
            $padOk = ($textX -ge 9 -and $textX -le 22)
            Write-Output ("     B: first text column: " + $textX)
            SendMenuKey 0x10 0x2A 100   # sacrificial SHIFT (first key is swallowed)
            SendMenuKey 0x28 0x50 150   # DOWN -> Show main window
            SendMenuKey 0x28 0x50 150   # DOWN -> Running highlighted
            $subOpen = WaitMenus $pB.Id 2 2
            if (-not $subOpen) {
                Write-Output "     B: submenu by timer: False"
                SendMenuKey 0x27 0x4D 250   # RIGHT -> expand the Running submenu
                $subOpen = WaitMenus $pB.Id 2 2
            }
            if ($subOpen) {
                $subOk = $true
                if ($Capture) {
                    $ms = Menus $pB.Id
                    [void](Capture $ms[0] (Join-Path $shots 'm131_tray_menu_1.png'))
                    if ($ms.Count -gt 1) { [void](Capture $ms[1] (Join-Path $shots 'm131_tray_menu_2.png')) }
                }
                SendMenuKey 0x28 0x50 150   # DOWN -> the probe row
                SendMenuKey 0x27 0x4D 300   # RIGHT -> the row's own menu
                # m132: the script's menu is a real nested submenu now, not a
                # second popup after the pick: it opens to the side of the row.
                if (WaitMenus $pB.Id 3 3) {
                    $ownOk = $true
                    if ($Capture) {
                        $ms = @(Menus $pB.Id)
                        [void](Capture $ms[$ms.Count - 1] (Join-Path $shots 'm131_script_menu.png'))
                    }
                    SendMenuKey 0x0D 0x1C 250   # ENTER -> TRAYMENU-PROBE (only item)
                    $actOk = WaitMarker $markAction 4
                } else {
                    Write-Output "     B: the script's own menu never nested"
                }
                [void](Wait-HwndGone $top 2)
            } else {
                Write-Output ("     B: try " + $try + ": the submenu never opened")
            }
            DismissMenus $pB.Id
        }
        UnfocusControl $pB.Id
    } else {
        Check $false "B: control window not found"
    }
    Check $subOk "B: the Running submenu opened"
    Check $ownOk "B: the script's own menu nested in as a real submenu"
    Check $actOk "B: the script's own menu item ran (action marker written)"
    Check $padOk "B: no check-mark column is reserved before the item text"
    Stop-Deck $pB
    }

    if ($Arms -match 'C') {
    # --- arm C: probe dies behind the open menu -> the guard absorbs it ------
    $pC = Launch $envs
    Check (WaitMain $pC.Id 20) "C: deck launched (pid $($pC.Id))"
    Start-Sleep -Seconds 2
    Check (HideMain $pC.Id) "C: main window hidden to the tray"
    $startedC = StartProbe $pC.Id
    if (-not $startedC) {
        Write-Output ("     C dbg: probe alive=" + (ProbeAlive $pC.Id) +
            " ahk-class windows=" + ([W]::OfClass([uint32]$pC.Id, 'AutoHotkey')).Count)
    }
    Check $startedC "C: probe running"
    $ctlC = FindControl $pC.Id
    $subOk = $false; $staleOk = $false; $noPop = $false; $aliveOk = $false
    if ($ctlC -ne [IntPtr]::Zero) {
        for ($try = 1; $try -le 3 -and -not $aliveOk; $try++) {
            if (-not (StartProbe $pC.Id)) { break }   # a retry starts it again
            FocusControl $ctlC
            if (-not (OpenTrayMenu $ctlC)) { break }
            if (-not (WaitMenus $pC.Id 1 3)) { continue }
            $top = (Menus $pC.Id)[0]
            # The menu was built while the probe ran; kill it behind the menu.
            # The probe's own window comes from a class scan - the in-process
            # kernel has NO resident AutoHotkey window (micro-test), so the
            # first hit is the probe's main window.
            $ahkWin = ([W]::OfClass([uint32]$pC.Id, 'AutoHotkey'))[0]
            if ($ahkWin -eq [IntPtr]::Zero) { continue }
            [void][W]::PostMessage($ahkWin, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
            Start-Sleep -Milliseconds 700   # the script thread exits; the row stays
            SendMenuKey 0x10 0x2A 100   # sacrificial SHIFT (first key gets swallowed)
            SendMenuKey 0x28 0x50 150   # DOWN -> Show main window
            SendMenuKey 0x28 0x50 200   # DOWN -> Running highlighted
            SendMenuKey 0x27 0x4D 250   # RIGHT -> expand the Running submenu
            if (WaitMenus $pC.Id 2 3) {
                $subOk = $true
                SendMenuKey 0x28 0x50 150   # DOWN -> the stale probe row
                SendMenuKey 0x27 0x4D 300   # RIGHT -> its menu (mirrored while it was alive)
                if (WaitMenus $pC.Id 3 2) {
                    $staleOk = $true
                    SendMenuKey 0x0D 0x1C 250   # ENTER -> pick it; the script window is gone
                    $closed = Wait-HwndGone $top 2
                    Start-Sleep -Seconds 1
                    if ($closed -and (Menus $pC.Id).Count -eq 0) {
                        $noPop = $true
                        if (-not $pC.HasExited) { $aliveOk = $true }
                    }
                }
            } else {
                Write-Output ("     C: try " + $try + ": the submenu never opened")
            }
            DismissMenus $pC.Id
        }
        UnfocusControl $pC.Id
    } else {
        Check $false "C: control window not found"
    }
    Check $subOk "C: the stale row still sat in the submenu after the kill"
    Check $staleOk "C: the dead script's menu was still mirrored in the open menu"
    Check $noPop "C: the stale pick opened nothing"
    Check $aliveOk "C: the deck survived the stale pick (guard line, no crash)"
    Stop-Deck $pC
    }
} catch {
    Write-Output ("UNEXPECTED: " + $_.Exception.Message)
    $script:fails++
} finally {
    # no matter what: queues detached, decks down, user scripts back
    if ($script:deckThreadId) {
        [void][W]::AttachThreadInput([W]::GetCurrentThreadId(), $script:deckThreadId, $false)
        $script:deckThreadId = 0
    }
    foreach ($p in @($pA, $pB, $pC)) {
        if ($p -and -not $p.HasExited) {
            try { Stop-Deck $p } catch { $p.Kill() }
        }
    }
    ClearMarkers
    if (Test-Path $probe) { Remove-Item -LiteralPath $probe -Force }
    foreach ($old in $stashed) {
        if (Test-Path $old) { Move-Item -LiteralPath $old -Destination $scripts -Force }
    }
}

if ($fails -gt 0) { Write-Output ("m131 gate: " + $fails + " failure(s)"); exit 1 }
Write-Output "m131 gate: all checks passed"
exit 0
