param(
    [string]$Exe = ".build\AHKDeck.exe",
    [string]$Screenshots = ".screenshots"
)

# m023 - the v1.3 workbench features, on the shipped exe.
#
# One deck launch, seven readings: the global start/stop hotkey, double-click as a
# switch, the menu bridge post, restart, the binding strip (save + clear), the tray
# close semantics and the explicit --quit. Everything is read from the product's own
# account (the probe's heartbeat/binding files), not from a screenshot.
#
# Not in ctest: it needs an interactive desktop. Run from the project root:
#   powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_workbench.ps1

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Stop-Deck.ps1")
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }

    public static void Click(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0201, (IntPtr)1, lp);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
        System.Threading.Thread.Sleep(120);
    }

    public static void DoubleClick(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0201, (IntPtr)1, lp);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
        System.Threading.Thread.Sleep(60);
        PostMessage(h, 0x0203, (IntPtr)1, lp);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
        System.Threading.Thread.Sleep(150);
    }

    // Modifiers through keybd_event (that is what updates the thread key state
    // FLTK reads), the letter through PostMessage (that is how the deck's window
    // gets the key without stealing the foreground). Same shape as CtrlS in
    // smoke_editor.ps1.
    public static void Combo(IntPtr h, byte vk, params byte[] mods) {
        foreach (byte m in mods) { keybd_event(m, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(30); }
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x001E0001);
        System.Threading.Thread.Sleep(30);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC01E0001);
        Array.Reverse(mods);
        foreach (byte m in mods) { keybd_event(m, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(30); }
        System.Threading.Thread.Sleep(200);
    }

    public static void TypeAscii(IntPtr h, string s) {
        foreach (char c in s)
            PostMessage(h, 0x0102, (IntPtr)c, (IntPtr)1);
        System.Threading.Thread.Sleep(150);
    }

    // A global hotkey is only fired by real input: RegisterHotKey sees the raw
    // stream, not a message posted to some window. So every part goes through
    // keybd_event.
    public static void GlobalKey(byte vk, params byte[] mods) {
        foreach (byte m in mods) { keybd_event(m, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(30); }
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(30);
        keybd_event(vk, 0, 2, UIntPtr.Zero);
        Array.Reverse(mods);
        foreach (byte m in mods) { keybd_event(m, 0, 2, UIntPtr.Zero); System.Threading.Thread.Sleep(30); }
        System.Threading.Thread.Sleep(200);
    }

    // PrintWindow renders the whole window (title bar and frame included), while
    // every widget coordinate in the product and in this gate is client-space.
    // The delta between the two is asked of the OS, not assumed.
    public static int[] ClientOffset(IntPtr h) {
        RECT wr;
        GetWindowRect(h, out wr);
        POINT p = new POINT();
        ClientToScreen(h, ref p);
        return new int[] { p.X - wr.Left, p.Y - wr.Top };
    }

    public static bool Capture(IntPtr h, string path) {
        RECT r;
        if (!GetWindowRect(h, out r)) return false;
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        using (var bmp = new System.Drawing.Bitmap(w, hh))
        using (var g = System.Drawing.Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(h, hdc, 2);
            g.ReleaseHdc(hdc);
            if (!ok) return false;
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
        return true;
    }

    public static void RightClick(IntPtr h, int x, int y) {
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0204, (IntPtr)0x0002, lp);   // WM_RBUTTONDOWN, MK_RBUTTON
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0205, IntPtr.Zero, lp);      // WM_RBUTTONUP
        System.Threading.Thread.Sleep(150);
    }

    public static IntPtr FirstWindowOfClass(uint pid, string cls) {
        string r = "";
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            var sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (sb.ToString() == cls) { r = h.ToString(); return false; }
            return true;
        }, IntPtr.Zero);
        return r.Length == 0 ? IntPtr.Zero : (IntPtr)Int64.Parse(r);
    }

    // A tracking menu belongs to the script thread's modal loop: WM_CANCELMODE
    // from another process does not close it (measured). The gate asks the session
    // itself, the same channel a host-side "close it" would need.
    public static void DismissScriptMenu(IntPtr scriptHwnd) {
        PostMessage(scriptHwnd, 0x8003, IntPtr.Zero, IntPtr.Zero);
    }

    public static void RightDownOnly(IntPtr h, int x, int y) {
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0204, (IntPtr)0x0002, lp);   // WM_RBUTTONDOWN, MK_RBUTTON
        // No UP: the deck blocks inside Fl_Menu_Item::popup() until the menu is
        // cancelled, which is exactly the state we want to measure.
    }

    // The deck's menu is an FLTK top-level window that is not the main window.
    public static string DeckMenuRect(uint pid, IntPtr main) {
        string found = "";
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || h == main || !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(32);
            GetClassName(h, sb, 32);
            if (sb.ToString() != "FLTK") return true;
            RECT r; GetWindowRect(h, out r);
            found = h + "|" + r.Left + "," + r.Top + ".." + r.Right + "," + r.Bottom;
            return false;
        }, IntPtr.Zero);
        return found;
    }

    public static void CancelFltkWindow(IntPtr h) {
        PostMessage(h, 0x0100, (IntPtr)0x1B, (IntPtr)0x00010001);   // WM_KEYDOWN VK_ESCAPE
    }

    // Any key into a specific window - used to drive an open Fl_Choice menu the
    // way a keyboard user does (down, then return).
    public static void KeyTo(IntPtr h, int vk) {
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x00010001);
        System.Threading.Thread.Sleep(30);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC0010001);
    }

    public static string MenuRect(uint pid) {
        string found = "";
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(32);
            GetClassName(h, sb, 32);
            if (sb.ToString() == "#32768") {
                RECT r; GetWindowRect(h, out r);
                found = r.Left + "," + r.Top + ".." + r.Right + "," + r.Bottom;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static int DialogsOf(uint pid) {
        int count = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            var sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (sb.ToString() == "#32770") count++;
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
'@

$root = Get-Location
$scripts = Join-Path $root ".build\Scripts"
$work = Join-Path $root ".temp\m023"
$settings = Join-Path $work "ui.ini"
$library = Join-Path $work "library.txt"
$hotkeys = Join-Path $work "hotkeys.txt"
$stash = Join-Path $work "scripts-stash"
$beat = Join-Path $scripts "m023-beat.txt"
$menuMark = Join-Path $scripts "m023-menu.txt"
$probeBeat = Join-Path $scripts "m023-beat.ahk"
$probeMenu = Join-Path $scripts "m023-menu.ahk"
$probeInclude = Join-Path $scripts "m023-include.ahk"
$probeTray = Join-Path $scripts "m023-tray.ahk"
$trayMark = Join-Path $scripts "m023-tray.txt"
$utf8 = New-Object System.Text.UTF8Encoding($false)
$failures = @()

Add-Type -AssemblyName System.Windows.Forms

# The console is the only place the deck talks. 复制 puts it on the clipboard, so
# a gate can read the product's own words instead of guessing from pixels.
function Get-DeckConsole($hwnd) {
    $had = $false
    $old = ""
    try { $had = [System.Windows.Forms.Clipboard]::ContainsText(); if ($had) { $old = [System.Windows.Forms.Clipboard]::GetText() } } catch {}
    [W]::KeyTo($hwnd, 0x78)           # 复制 (last toolbar button, m026)
    Start-Sleep -Milliseconds 500
    $text = ""
    try { if ([System.Windows.Forms.Clipboard]::ContainsText()) { $text = [System.Windows.Forms.Clipboard]::GetText() } } catch {}
    try { if ($had) { [System.Windows.Forms.Clipboard]::SetText($old) } else { [System.Windows.Forms.Clipboard]::Clear() } } catch {}
    return $text
}

function Last-Console($hwnd, $count) {
    $t = Get-DeckConsole $hwnd
    $lines = @($t -split "`r?`n" | Where-Object { $_ -ne "" })
    if ($lines.Count -le $count) { return ($lines -join " / ") }
    return (($lines | Select-Object -Last $count) -join " / ")
}

New-Item -ItemType Directory -Force $scripts, $work, $stash, $Screenshots | Out-Null

function Check($ok, $what) {
    if ($ok) { Write-Output "[PASS]  $what" }
    else { Write-Output "[FAIL]  $what"; $script:failures += $what }
}

function Assert-MenuAnchored([string]$label, $procId, $h) {
    # The property that matters: the menu is anchored at the pointer, not at some
    # window's origin. TrackPopupMenu flips the menu above the point when it does
    # not fit below, so either the top-left or the bottom-left is the anchor.
    $script:menuRect = ""
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 120
        $script:menuRect = [W]::MenuRect([uint32]$procId)
        if ($script:menuRect -ne "") { break }
    }
    if ($script:menuRect -eq "") {
        Check $false "$label  (no #32768 menu window appeared)"
        return
    }
    $parts = $script:menuRect -split "\.\."
    $tl = $parts[0] -split ","
    $br = $parts[1] -split ","
    $cur = New-Object W+POINT
    [void][W]::GetCursorPos([ref]$cur)
    $dx = [int]$tl[0] - $cur.X
    $dyTop = [int]$tl[1] - $cur.Y
    $dyBottom = [int]$br[1] - $cur.Y
    $ok = ($dx -ge -2 -and $dx -le 8 -and (($dyTop -ge -2 -and $dyTop -le 8) -or ($dyBottom -ge -2 -and $dyBottom -le 8)))
    $flip = if ($dyBottom -ge -2 -and $dyBottom -le 8) { ", flipped above" } else { "" }
    Check $ok ("$label  cursor ($($cur.X),$($cur.Y)) -> menu $script:menuRect, dx $dx, dyTop $dyTop, dyBottom $dyBottom$flip")
    # close it again so the next cell starts clean
    $src = [W]::FirstWindowOfClass([uint32]$procId, "AutoHotkey")
    if ($src -eq [IntPtr]::Zero) {
        Check $false "$label  (no AutoHotkey window to ask for dismissal)"
        return
    }
    [W]::DismissScriptMenu($src)
    $closed = $false
    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Milliseconds 120
        if ([W]::MenuRect([uint32]$procId) -eq "") { $closed = $true; break }
    }
    Check $closed "$label  / closed again by the session itself (EndMenu on its own thread)"
}

function Assert-DeckMenu([string]$label, $procId, $h, $x, $y) {
    # m026: right-click has ONE meaning - the deck's own script menu - whether or
    # not the row is running. So the witness is the FLTK menu window (not a
    # #32768), anchored at the point that was clicked, and dismissable again.
    $want = New-Object W+POINT
    $want.X = $x; $want.Y = $y + 32
    [void][W]::ClientToScreen($h, [ref]$want)
    [W]::RightDownOnly($h, $x, $y + 32)
    $deckMenu = ""
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 120
        $deckMenu = [W]::DeckMenuRect([uint32]$procId, $h)
        if ($deckMenu -ne "") { break }
    }
    if ($deckMenu -eq "") {
        Check $false "$label  (no deck menu window appeared)"
        return
    }
    $bits = $deckMenu -split "\|"
    $tl = ($bits[1] -split "\.\.")[0] -split ","
    $ddx = [int]$tl[0] - $want.X
    $ddy = [int]$tl[1] - $want.Y
    Check ($ddx -ge -8 -and $ddx -le 8 -and $ddy -ge -8 -and $ddy -le 8) `
        ("$label  click screen ($($want.X),$($want.Y)) -> deck menu $($bits[1]), delta $ddx,$ddy")
    # The script's own tray menu must NOT be what opened.
    $scriptMenu = [W]::MenuRect([uint32]$procId)
    Check ($scriptMenu -eq "") "$label  / and no #32768 (the script's own menu stays closed)"
    [W]::CancelFltkWindow([IntPtr]([int64]$bits[0]))
    $closed = $false
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 120
        if ([W]::DeckMenuRect([uint32]$procId, $h) -eq "") { $closed = $true; break }
    }
    Check $closed "$label  / and it is dismissed again (the deck thread was blocked inside popup())"
}

function Read-DotPixel($pngPath, $offX, $offY, $rowTop) {
    # The light is drawn, not written, so the only reading is the pixel - but the
    # reading has to be aimed: only the dot column is scanned (the name column
    # right after it carries ClearType fringes in every hue), the capture is
    # shifted into client space with the offset the OS reports, and a colour only
    # counts when a solid patch of it sits in the cell.
    if (-not (Test-Path $pngPath)) { return "missing png" }
    $want = @{ green = @(0x2E, 0x7D, 0x32); grey = @(0xB0, 0xB4, 0xB9); red = @(0xC6, 0x28, 0x28) }
    $count = @{ green = 0; grey = 0; red = 0 }
    $bmp = [System.Drawing.Bitmap]::new($pngPath)
    try {
        # The dot is 8 px tall, centred in a 22 px row whose top is the browser's
        # y plus Fl_Browser's 2 px item inset; the band is that row and no more.
        for ($y = $rowTop + 2; $y -le $rowTop + 20; $y++) {
            for ($x = 10; $x -le 40; $x++) {
                $px = $offX + $x; $py = $offY + $y
                if ($px -lt 0 -or $py -lt 0 -or $px -ge $bmp.Width -or $py -ge $bmp.Height) { continue }
                $c = $bmp.GetPixel($px, $py)
                foreach ($k in @($want.Keys)) {
                    $w = $want[$k]
                    if ([Math]::Abs($c.R - $w[0]) -le 25 -and [Math]::Abs($c.G - $w[1]) -le 25 -and [Math]::Abs($c.B - $w[2]) -le 25) {
                        $count[$k]++
                    }
                }
            }
        }
    } finally { $bmp.Dispose() }
    $best = $count.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
    if ($best.Value -lt 12) { return "none($($best.Value) px of $($best.Key))" }
    return $best.Key
}

function Size-Of($path) { if (Test-Path $path) { (Get-Item $path).Length } else { 0 } }
function Text-Of($path) { if (Test-Path $path) { [System.IO.File]::ReadAllText($path) } else { "" } }

$deck = $null
$stashed = @()
try {
    # ---- corpus: only our three probes in the scanned folder ------------------
    foreach ($f in @(Get-ChildItem (Join-Path $scripts "*.ahk") -ErrorAction SilentlyContinue)) {
        Move-Item $f.FullName (Join-Path $stash $f.Name) -Force
        $stashed += $f.FullName
    }
    Remove-Item $settings, $library, $hotkeys, $beat, $menuMark, $trayMark -ErrorAction SilentlyContinue
    # The window must NOT sit at (0,0): A_TrayMenu.Show(x, y) adds the foreground
    # window's origin to the point (Menu coord mode defaults to Client, and
    # g.CoordMode = 0). At (0,0) that offset is invisible and the position cells
    # would pass vacuously - which is exactly how the first version of this gate
    # missed the bug the user reported.
    [System.IO.File]::WriteAllText($settings, "x=320`r`ny=210`r`nw=1180`r`nh=720`r`n", $utf8)

    # beat: a resident heartbeat whose counter restarts on every session, so a
    # restart is visible as the file starting over. It never builds a tray menu,
    # so the deck must pop the kernel's DEFAULT one; the 0x8003 handler is the
    # gate's own dismissal channel (the menu loop is modal on the script thread,
    # so only the owner can EndMenu it) and which menu it shows is unaffected.
    [System.IO.File]::WriteAllText($probeBeat, @'
#Requires AutoHotkey v2.0
#SingleInstance Force
OnMessage(0x8003, Dismiss)
Dismiss(wParam, lParam, msg, hwnd) {
    DllCall("user32\EndMenu")
    return 0
}
Beat() {
    static n := 0
    n++
    FileAppend(n "|" A_TickCount "`n", A_ScriptDir "\m023-beat.txt", "UTF-8")
    if (n >= 200)
        ExitApp 0
}
if FileExist(A_ScriptDir "\m023-beat.txt")
    FileDelete(A_ScriptDir "\m023-beat.txt")
SetTimer Beat, 200

'@, $utf8)

    # menu: the shape of a bridged script. The recorder stands in for the shipped
    # bridge's A_TrayMenu.Show() so this gate never pops a visible menu on a shared
    # desktop; "AHKDeckBridge" in the source is what makes the deck offer the item,
    # and the shipped include itself is proven by the --run cell below.
    [System.IO.File]::WriteAllText($probeMenu, @'
#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon
; Same protocol as the shipped AHKDeckBridge.inc: OnMessage(0x8001, x, y).
gMark := A_ScriptDir "\m023-menu.txt"
if FileExist(gMark)
    FileDelete(gMark)
FileAppend("start|" DllCall("kernel32\GetCurrentThreadId") "`n", gMark, "UTF-8")
OnMessage(0x8003, Dismiss)
Dismiss(wParam, lParam, msg, hwnd) {
    DllCall("user32\EndMenu")
    return 0
}
OnMessage(0x8001, Hit)
Hit(wParam, lParam, msg, hwnd) {
    global gMark
    FileAppend("hit|" wParam "|" lParam "`n", gMark, "UTF-8")
    return 0
}
Keep() {
}
SetTimer Keep, 500

'@, $utf8)

    # tray: a bridged script with its own tray menu, so the takeover gesture can be
    # measured against the kernel's real A_TrayMenu.Show() rather than a recorder.
    [System.IO.File]::WriteAllText($probeTray, @'
#Requires AutoHotkey v2.0
#SingleInstance Force
#Include AHKDeckBridge.inc
FileAppend("tray-started`n", A_ScriptDir "\m023-tray.txt", "UTF-8")
A_TrayMenu.Delete()
A_TrayMenu.Add("m023-item", (*) => 0)
; The gate needs the menu closed again; only the owning thread can EndMenu it.
OnMessage(0x8003, Dismiss)
Dismiss(wParam, lParam, msg, hwnd) {
    DllCall("user32\EndMenu")
    return 0
}
Keep() {
}
SetTimer Keep, 500

'@, $utf8)

    # include: proves the file the deck ships resolves from the Scripts folder.
    [System.IO.File]::WriteAllText($probeInclude, @'
#Requires AutoHotkey v2.0
#Include AHKDeckBridge.inc
ExitApp 0

'@, $utf8)

    # a binding that already exists on disk, in v1.3's own spelling: no '+' anywhere
    [System.IO.File]::WriteAllText($hotkeys, "m023-beat.ahk=^!+F9`r`n", $utf8)

    Check (Test-Path (Join-Path $scripts "AHKDeckBridge.inc")) `
        "0  the menu bridge include ships next to the scripts"

    # ---- the shipped include must load in the kernel --------------------------
    # An explicit process with a real wait: AHKDeck.exe is a GUI-subsystem image,
    # so the call operator would return before it finished and $LASTEXITCODE would
    # describe nothing.
    $runPsi = New-Object System.Diagnostics.ProcessStartInfo
    $runPsi.FileName = (Resolve-Path $Exe).Path
    $runPsi.Arguments = "--run `"$probeInclude`""
    $runPsi.UseShellExecute = $false
    $runPsi.CreateNoWindow = $true
    $run = [System.Diagnostics.Process]::Start($runPsi)
    if (-not $run.WaitForExit(20000)) { $run.Kill(); throw "--run of the include probe never finished" }
    $includeRc = $run.ExitCode
    Check ($includeRc -eq 0) "1  AHKDeck.exe --run loads a script that #Includes AHKDeckBridge.inc (rc=$includeRc)"

    # ---- launch the deck ------------------------------------------------------
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["AHKDECK_SETTINGS"] = $settings
    $psi.EnvironmentVariables["AHKDECK_LIBRARY"] = $library
    $psi.EnvironmentVariables["AHKDECK_HOTKEYS"] = $hotkeys
    $deck = [System.Diagnostics.Process]::Start($psi)
    [void]$deck.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1800
    $deck.Refresh()
    $h = $deck.MainWindowHandle
$wr = New-Object W+RECT
[void][W]::GetWindowRect($h, [ref]$wr)
# The filter sits at the bottom of the ACTUAL client (the 1180x720 request may be clamped).
$filterY = ($wr.Bottom - $wr.Top) - 72
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-start.png"))

    # rows are sorted by name: 1 m023-beat, 2 m023-include, 3 m023-menu.
    # m024/m026: the list pane grew a header row (filter + column captions, 24px + 6),
    # the browser now starts 30px lower than the pane top.
    $ListTop = 58
    $rowBeat = $ListTop + 10
    $rowMenu = $ListTop + (2 * 26) + 10

    # ---- 2: the global hotkey starts, and starts again to stop ----------------
    [W]::GlobalKey(0x78, 0x11, 0x12, 0x10)   # Ctrl+Alt+Shift+F9
    Start-Sleep -Milliseconds 1200
    $len1 = Size-Of $beat
    Start-Sleep -Milliseconds 600
    $len2 = Size-Of $beat
    Check ($len2 -gt 0) "2  Ctrl+Alt+Shift+F9 (read from hotkeys.txt) started m023-beat.ahk"
    [W]::GlobalKey(0x78, 0x11, 0x12, 0x10)
    Start-Sleep -Milliseconds 700
    $len3 = Size-Of $beat
    Start-Sleep -Milliseconds 800
    $len4 = Size-Of $beat
    Check ($len3 -gt 0 -and $len4 -eq $len3) `
        "3  the same hotkey stopped it again ($len1 -> $len2 -> $len3 -> $len4 bytes)"

    # ---- 4: double-click a row = switch, and the bridge post ------------------
    [W]::DoubleClick($h, 120, $rowMenu)
    Start-Sleep -Milliseconds 900
    $started = Text-Of $menuMark
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-dbl.png"))
    Check ($started -match "start\|") "4  double-click started m023-menu.ahk"

    $cursor = New-Object W+POINT
    [void][W]::GetCursorPos([ref]$cursor)
    [W]::Click($h, 120, $rowMenu)          # single click selects, starts nothing
    Start-Sleep -Milliseconds 200
    [W]::Combo($h, 0x4D, 0x11, 0x10)      # Ctrl+Shift+M = the bridge action
    Start-Sleep -Milliseconds 1200
    # The keyboard route must do what the right-click does, and the witness is the
    # menu window itself - not a script-side recorder, because the product no longer
    # asks the script for cooperation.
    Assert-MenuAnchored "5  Ctrl+Shift+M popped that script's own tray menu at the pointer" $deck.Id $h
    $starts = @(Select-String -Path $menuMark -Pattern "^start\|" -ErrorAction SilentlyContinue)
    Check ($starts.Count -eq 1) "7  the single click did not start a second session ($($starts.Count) starts)"

    # ---- 14/15: right-click is ONE gesture now (m026) ------------------------
    # Measured while the layout is the default one: the hotkey strip shifts the
    # panes down by 30px, so this cell must run before the strip opens.
    # m023 used to hand the right-click over to the script's own tray menu when the
    # row was running. That made one gesture mean two things, so it is gone: the
    # script's menu now lives in the row's 菜单 cell, and right-click always shows
    # the deck's menu - running row included.
    $rowTray = $ListTop + (3 * 26) + 10               # row 4 = m023-tray.ahk
    [W]::Click($h, 120, $rowTray)                      # select row 4
    Start-Sleep -Milliseconds 200
    [W]::KeyTo($h, 0x74)                            # 运行
    Start-Sleep -Milliseconds 1500
    # Read the product's own account before judging the gesture: if the marker is
    # missing, the click did not land on this row and the rest of the cell is void.
    Check (Test-Path $trayMark) "14  row 4 is the tray probe and it is running (its own marker exists)"
    Assert-DeckMenu "15  right-click on a RUNNING row shows the deck menu (not the script's)" $deck.Id $h 80 $rowTray
    [void][W]::Capture($h, (Join-Path $Screenshots "m026-rightclick-running.png"))

    # ---- 17: and the same on a row that never started -------------------------
    # The anchoring itself is the m023 §11 criterion: Fl_Menu_Item::popup() takes
    # WIDGET-relative coordinates and adds the event's root offset itself, so
    # passing event_x_root() double-adds the window origin.
    $rowIdle = $ListTop + 26 + 10                      # row 2 = m023-include.ahk
    Assert-DeckMenu "17  the deck menu on an idle row is anchored at the pointer" $deck.Id $h 80 $rowIdle

    # ---- 8: restart = new session, counter starts over ------------------------
    Start-Sleep -Milliseconds 1200        # let the stopped session be reaped first
    [W]::Click($h, 120, $rowBeat)          # select the beat row
    [W]::GlobalKey(0x78, 0x11, 0x12, 0x10)  # start it with the hotkey
    Start-Sleep -Milliseconds 1800
    $beforeRestart = @(Get-Content $beat -ErrorAction SilentlyContinue)
    [W]::KeyTo($h, 0x75)               # 重启
    Start-Sleep -Milliseconds 1800
    $afterRestart = @(Get-Content $beat -ErrorAction SilentlyContinue)
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-restart.png"))
    # A new session rewrites the file from n=1, and A_TickCount only ever moves
    # forward in one process: the first line's tick must be LATER than the old
    # file's first line while n starts at 1 again.
    $tickBefore = 0
    $tickAfter = 0
    if ($beforeRestart.Count -ge 1) { [void][int]::TryParse(($beforeRestart[0] -split '\|')[1], [ref]$tickBefore) }
    if ($afterRestart.Count -ge 1) { [void][int]::TryParse(($afterRestart[0] -split '\|')[1], [ref]$tickAfter) }
    $restarted = ($beforeRestart.Count -ge 4 -and $afterRestart.Count -ge 1 `
        -and ($afterRestart[0] -split '\|')[0] -eq '1' -and $tickAfter -gt $tickBefore)
    Check $restarted `
        "8  重启 replaced the session: heartbeat restarted from 1 at a later tick ($($beforeRestart[0]) -> $($afterRestart[0]))"

    # ---- 9/10: the binding strip ---------------------------------------------
    [W]::Click($h, 120, $rowMenu)
    [W]::KeyTo($h, 0x77)               # 快捷键
    Start-Sleep -Milliseconds 400
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-hotkey-strip.png"))
    [W]::Combo($h, 0x76, 0x11, 0x10)           # the spelling a user types
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-strip-open.png"))
    [W]::KeyTo($h, 0x1B)               # 保存
    Start-Sleep -Milliseconds 600
    $savedBindings = Text-Of $hotkeys
    [void][W]::Capture($h, (Join-Path $Screenshots "m023-strip-save.png"))
    $bindingLines = @($savedBindings -split "`r`n")
    Check ($bindingLines -contains "m023-menu.ahk=^+F7") `
        ("9  the strip saved and canonicalised it (^!+f7 -> ^!+F7 in hotkeys.txt) [file: " `
            + ($savedBindings -replace "`r`n", " / ") + "]")

    [W]::KeyTo($h, 0x77)               # 快捷键 again
    Start-Sleep -Milliseconds 300
    [W]::KeyTo($h, 0x1B)               # 清除
    Start-Sleep -Milliseconds 600
    $afterClear = Text-Of $hotkeys
    Check ($afterClear -match "m023-menu\.ahk=\^\+F7") "10  Esc cancel kept the existing binding"

    # ---- 19..24: m026 the per-row 菜单 cell and the traffic light -------------
    # One session at a time, so the menu window that appears can only belong to it.
    # The cell's x is the product's own arithmetic: the list column is
    # (window - 2*pad) wide, and the columns are dot 18 | name | hotkey 104 | menu 76.
    $MenuCellX = 8 + 1 + 36 + 18
    [W]::KeyTo($h, 0x76)               # 全部停止
    Start-Sleep -Milliseconds 3500
    Remove-Item $beat, $menuMark, $trayMark -ErrorAction SilentlyContinue
    [W]::Click($h, 120, $ListTop + (3 * 26) + 10)   # row 4 = m023-tray.ahk
    Start-Sleep -Milliseconds 200
    [W]::KeyTo($h, 0x74)                # 运行
    Start-Sleep -Milliseconds 1600
    Check (Test-Path $trayMark) "19  clean slate: row 4 is the tray probe and it is running (its marker exists)"

    # 20: the row's own 菜单 cell pops THAT script's tray menu, anchored at the
    # pointer (the kernel reads GetCursorPos itself, so CoordMode cannot interfere).
    [W]::Click($h, $MenuCellX, $ListTop + (3 * 26) + 10)
    Start-Sleep -Milliseconds 800
    $account20 = Last-Console $h 1
    Assert-MenuAnchored "20  the row's 菜单 cell popped that script's own menu [console: $account20]" $deck.Id $h

    # 21/22: the same cell on a script that never built a tray menu - the kernel's
    # default menu is still the script's own menu, and the cell only exists while
    # the row is running.
    [W]::KeyTo($h, 0x76)               # 全部停止 again, one session at a time
    Start-Sleep -Milliseconds 2000
    Remove-Item $beat -ErrorAction SilentlyContinue
    [W]::Click($h, 120, $ListTop + 10)     # row 1 = m023-beat
    Start-Sleep -Milliseconds 200
    [W]::KeyTo($h, 0x74)
    Start-Sleep -Milliseconds 1600
    $beatStartedBytes = Size-Of $beat
    Check ($beatStartedBytes -gt 0) "21  row 1 is m023-beat and it is running ($beatStartedBytes bytes)"
    [W]::Click($h, $MenuCellX, $ListTop + 10)
    Start-Sleep -Milliseconds 800
    $account22 = Last-Console $h 1
    Assert-MenuAnchored "22  the same cell also pops the DEFAULT tray menu of a script that never built one [console: $account22]" $deck.Id $h

    # 23: the traffic light is drawn, not written - so the reading is the pixel.
    # Row 1 (running) must be red, row 2 (never started since launch) must be a
    # green play arrow, and the two must not be the same colour, or the column is
    # decoration. The third hue in Read-DotPixel is m068's grey tick: a row whose
    # last run finished cleanly.
    $png = Join-Path $Screenshots "m026-columns.png"
    [void][W]::Capture($h, $png)
    $off = [W]::ClientOffset($h)
    $dotRunning = Read-DotPixel $png $off[0] $off[1] ($ListTop + 32)
    $dotIdle = Read-DotPixel $png $off[0] $off[1] ($ListTop + 26 + 32)
    Check ($dotRunning -eq "red" -and $dotIdle -eq "green") `
        "23  the state column reads as a light: running row = $dotRunning, idle row = $dotIdle"

    # 23b (m077 rewrote this cell): a row whose script ran and exited CLEANLY
    # reads as the idle green play arrow again - by design. The cell is a button,
    # so it answers "what does clicking do", never "what happened last time"; the
    # m068 grey tick made users think the row was in a special mode and stop
    # clicking it. The last FAILURE is what survives, as a 2px red strip on the
    # row's left edge - measured by .temp/m077_light.ps1, not here (this fixture
    # has no failing script, and this gate moves .build\Scripts, so it needs a nod).
    [W]::Click($h, 120, $ListTop + 26 + 10)   # row 2 = m023-include.ahk
    Start-Sleep -Milliseconds 200
    [W]::KeyTo($h, 0x74)                      # run
    Start-Sleep -Milliseconds 1500
    [void][W]::Capture($h, $png)
    $dotFinished = Read-DotPixel $png $off[0] $off[1] ($ListTop + 26 + 32)
    Check ($dotFinished -eq "green") `
        "23b a row that exited rc=0 is clickable again: same green arrow as a never-run row (read: $dotFinished)"

    # ---- 24/25: the filter, measured on what a click actually starts ----------
    # A filter that hides rows must not change what a row MEANS: the witness is
    # which script starts when the only visible row is double-clicked, and which
    # one stays absent. m023-menu.ahk deletes its marker on start, so "no marker"
    # proves the other script was not touched.
    [W]::KeyTo($h, 0x76)               # 全部停止
    Start-Sleep -Milliseconds 3500
    $stopAccount = Last-Console $h 2
    Write-Output "stop-all account: $stopAccount"
    Remove-Item $beat, $menuMark -ErrorAction SilentlyContinue
    [W]::Click($h, 300, $filterY)               # the filter input (list header row)
    Start-Sleep -Milliseconds 200
    [W]::TypeAscii($h, "m023-menu")
    Start-Sleep -Milliseconds 800
    [void][W]::Capture($h, (Join-Path $Screenshots "m024-filter.png"))
    [W]::DoubleClick($h, 120, $ListTop + 10)
    Start-Sleep -Milliseconds 1600
    $filteredMenu = Test-Path $menuMark
    $filteredBeat = Test-Path $beat
    Check ($filteredMenu -and -not $filteredBeat) `
        ("24  filtered to one row: the click started m023-menu (marker: $filteredMenu) " `
            + "and NOT m023-beat (marker: $filteredBeat)")

    # 25: clearing the filter brings the whole list back, and row 1 is m023-beat
    # again - this is the cell that starts the script cells 12/13 watch.
    [W]::Click($h, 300, $filterY)
    Start-Sleep -Milliseconds 150
    [W]::KeyTo($h, 0x23)                # VK_END: a click puts the caret at the start
    Start-Sleep -Milliseconds 150
    [W]::TypeAscii($h, "`b`b`b`b`b`b`b`b`b`b`b`b`b")
    Start-Sleep -Milliseconds 800
    [W]::DoubleClick($h, 120, $ListTop + 10)
    Start-Sleep -Milliseconds 1600
    $beatUnfiltered = Size-Of $beat
    Check ($beatUnfiltered -gt 0) "25  the filter cleared: row 1 is m023-beat again and it started ($beatUnfiltered bytes)"

    # The deck's own console is the account of what 19-25 did (it is the only place
    # the deck talks).
    [void][W]::Capture($h, (Join-Path $Screenshots "m024-console-account.png"))

    # ---- 11/12: close hides, --quit really leaves ----------------------------
    $dialogsBefore = [W]::DialogsOf([uint32]$deck.Id)
    [void][W]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    Start-Sleep -Milliseconds 1200
    $hidden = -not [W]::IsWindowVisible($h)
    $alive = -not $deck.HasExited
    $beatBefore = Size-Of $beat
    Start-Sleep -Milliseconds 900
    $beatAfter = Size-Of $beat
    Check ($hidden -and $alive) "11  the close box hid the deck to the tray instead of killing it"
    Check ($beatAfter -gt $beatBefore) "12  the script kept running while hidden ($beatBefore -> $beatAfter bytes)"

    $rc = Stop-Deck -Process $deck -Exe $Exe
    # Sample only after the process is gone: the deck waits up to 3 s for the
    # kernel threads to unwind, and a tick inside that window is not a leak.
    Start-Sleep -Milliseconds 500
    $beatStopped = Size-Of $beat
    Start-Sleep -Milliseconds 900
    $beatLater = Size-Of $beat
    Check ($beatLater -eq $beatStopped) "13  --quit stopped every session ($beatAfter -> $beatStopped -> $beatLater bytes, rc=$rc)"
    Check ($dialogsBefore -eq 0) "14  no modal dialog appeared on the way"

}
finally {
    foreach ($path in @($probeBeat, $probeMenu, $probeInclude, $probeTray, $beat, $menuMark, $trayMark, $settings, $library, $hotkeys)) {
        Remove-Item $path -ErrorAction SilentlyContinue
    }
    foreach ($path in $stashed) {
        $leaf = Split-Path $path -Leaf
        if (Test-Path (Join-Path $stash $leaf)) { Move-Item (Join-Path $stash $leaf) $path -Force }
    }
    if ($deck -and -not $deck.HasExited) { $deck.Kill() }
}

Write-Output ""
if ($failures.Count) {
    Write-Output "workbench: $($failures.Count) failure(s)"
    foreach ($f in $failures) { Write-Output "  - $f" }
    exit 1
}
Write-Output "workbench: all cells passed"
