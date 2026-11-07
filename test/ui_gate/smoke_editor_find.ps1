param(
    [string]$Exe = ".build\AHKDeck.exe",
    [string]$Screenshots = ".temp\ui_gate_shots",
    # Screenshots are a by-product, not a judgement. PrintWindow waits for a
    # window that is drawing, and on a busy desktop that wait never ends - a
    # gate that hangs on a picture teaches nobody anything. Ask for it.
    [switch]$Capture
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Stop-Deck.ps1")
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    // m084: SendMessage, not PostMessage. The Ctrl+key gesture depends on the
    // real Ctrl key still being down when the app reads the modifier state.
    // PostMessage only queues the key, so on a busy desktop the app can pick
    // it up after Ctrl was already released - the shortcut then arrives as a
    // plain letter and the gate fails for a reason that has nothing to do
    // with the deck. SendMessage returns when the app has processed the key,
    // so the gesture is deterministic.
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }

    // The injected Ctrl key only reaches the app while the deck is the
    // foreground window; otherwise it lands in whatever else has the focus.
    public static void Foreground(IntPtr h) {
        SetForegroundWindow(h);
        System.Threading.Thread.Sleep(300);
    }

    public static void PressF2(IntPtr h) {
        PostMessage(h, 0x0100, (IntPtr)0x71, (IntPtr)0x003C0001);
        PostMessage(h, 0x0101, (IntPtr)0x71, (IntPtr)0xC03C0001);
        System.Threading.Thread.Sleep(120);
    }

    public static void Click(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0201, (IntPtr)1, lp);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
        System.Threading.Thread.Sleep(150);
    }

    public static void TypeAscii(IntPtr h, string s) {
        foreach (char c in s) {
            SendMessage(h, 0x0102, (IntPtr)c, (IntPtr)1);
            System.Threading.Thread.Sleep(8);
        }
        System.Threading.Thread.Sleep(150);
    }

    public static void CtrlS(IntPtr h) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(250);
        PostMessage(h, 0x0100, (IntPtr)0x53, (IntPtr)0x001F0001);
        System.Threading.Thread.Sleep(120);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
        System.Threading.Thread.Sleep(250);
    }

    // Ctrl+<key>: the Ctrl key is really held (so GetKeyState reports it) while
    // the WM_KEYDOWN goes to our window - that is what FLTK reads. The find
    // bar is driven from the keyboard this way, so the gate needs no pixel
    // coordinates that the layout could invalidate.
    public static void CtrlKey(IntPtr h, int vk) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        // m083: the Ctrl press goes through the system input queue, so the app
        // learns about it only when it pumps messages. A short pause here used
        // to race: sometimes the key arrived before the app knew that Ctrl was
        // down, and then Ctrl+F typed a plain "f" into the editor instead of
        // opening the find bar.
        System.Threading.Thread.Sleep(250);
        // m084: SendMessage, not PostMessage - the key must be handled while
        // Ctrl is still down, or the shortcut arrives as a plain letter.
        // Only the key-down is sent: the matching WM_CHAR would type the
        // letter into whatever has the focus if the shortcut misses.
        SendMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x001F0001);
        System.Threading.Thread.Sleep(120);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
        System.Threading.Thread.Sleep(200);
    }

    public static void Enter(IntPtr h) {
        // m084: only the key-down - FLTK turns it into FL_Enter, and the
        // matching WM_CHAR would push a newline into the field.
        SendMessage(h, 0x0100, (IntPtr)0x0D, (IntPtr)0x001C0001);
        System.Threading.Thread.Sleep(250);
    }

    public static void KeyTo(IntPtr h, int vk) {
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x00010001);
        System.Threading.Thread.Sleep(30);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC0010001);
        System.Threading.Thread.Sleep(200);
    }
    public static void CtrlShiftDelete(IntPtr h) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        keybd_event(0x10, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0100, (IntPtr)0x2E, (IntPtr)0x00530001);
        PostMessage(h, 0x0101, (IntPtr)0x2E, (IntPtr)0xC0530001);
        System.Threading.Thread.Sleep(80);
        keybd_event(0x10, 0, 2, UIntPtr.Zero);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
        System.Threading.Thread.Sleep(250);
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

    // m084: FLTK's own message window is NOT a #32770 dialog - it is a window
    // of class "FLTK", so the dialog counter above never saw it. That is how
    // a popup storm (one per console line) could hide behind a green gate.
    // The deck's own window is a class "FLTK" window too, so the main
    // window is excluded and only VISIBLE ones count.
    public static int FltkWindowsOf(uint pid, IntPtr main) {
        int count = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid) return true;
            if (h == main) return true;
            var sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (sb.ToString() != "FLTK") return true;
            if (!IsWindowVisible(h)) return true;
            count++;
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
"@

$root = Get-Location
$scripts = Join-Path $root ".build\Scripts"
$work = Join-Path $root ".temp\m016"
$libPath = Join-Path $work "library.txt"
$external = Join-Path $work "lib-probe.ahk"
$findProbe = Join-Path $scripts "ui-find.ahk"
$stash = Join-Path $work "scripts-stash"
$settingsFile = Join-Path $work "ui.ini"
New-Item -ItemType Directory -Force $scripts, $work, $stash, $Screenshots | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding($false)
$findText = "#Requires AutoHotkey v2.0`r`n; needle first`r`nFileAppend ""needle``n"", A_ScriptDir ""\find-out.txt""`r`n; needle second`r`n"
# The replacement is deliberately not the needle in another case: if the keys
# ever land in the editor instead of the find bar, the file would show the
# needle next to the replacement and the gate must fail.
$replacement = "REPLACED"

# Deterministic single-row lists: stash any pre-existing scripts for the run.
$stashed = @()
foreach ($f in @(Get-ChildItem (Join-Path $scripts "*.ahk") -ErrorAction SilentlyContinue)) {
    Move-Item $f.FullName (Join-Path $stash $f.Name) -Force
    $stashed += $f.FullName
}

function Start-Deck([string]$library) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    # m084: FLTK's errors are written to stderr by the error channel. Reading
    # them here is what turns "a popup nobody saw" into a red cell.
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables["AHKDECK_LIBRARY"] = $library
    $psi.EnvironmentVariables["AHKDECK_SETTINGS"] = $settingsFile
    return [System.Diagnostics.Process]::Start($psi)
}

# The close box now hides to the tray (m023), so leaving needs the explicit quit.
function Stop-DeckLocal($p) {
    Stop-Deck -Process $p -Exe $Exe
}

try {
    # ---- session A: line numbers + find/replace on the only script ----
    [System.IO.File]::WriteAllText($findProbe, $findText, $utf8)
    Remove-Item $libPath -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($settingsFile, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))

    $p = Start-Deck $libPath
    # m084: drain the error channel in the background, so a FLTK error can
    # never block the app on a full pipe.
    $errA = $p.StandardError.ReadToEndAsync()
    [void]$p.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }

    [W]::Foreground($h)
    [W]::Click($h, 120, 69)           # the only row (m024: 86 + 11)
    Start-Sleep -Milliseconds 400
    [W]::PressF2($h)                 # m022: open it in the editor
    Start-Sleep -Milliseconds 600
    # The find/replace bar sits ABOVE the editor now (m084, Pecia's place), so
    # the gate drives it from the keyboard and needs no pixel coordinates:
    # Ctrl+F focuses the needle field, Enter finds and hands the focus back to
    # the editor, Ctrl+H focuses the replacement field, Enter replaces
    # everywhere.
    Write-Output ("[{0}] session A: keyboard find/replace" -f (Get-Date -Format HH:mm:ss))
    # m089: the find bar is a SWITCH now, and it starts folded (Pecia's
    # model): the first Ctrl+F brings it up and focuses the needle field,
    # the second takes it away. Ctrl+H is the same switch, so while the bar
    # is up the first Ctrl+H folds it and the second brings it back with the
    # focus on the replacement field.
    [W]::CtrlKey($h, 0x46)           # Ctrl+F - the bar comes up
    Start-Sleep -Milliseconds 300
    [W]::TypeAscii($h, "needle")
    [W]::Enter($h)                   # find the next match
    Start-Sleep -Milliseconds 500

    [W]::CtrlKey($h, 0x48)           # Ctrl+H - folds the bar (the switch)
    Start-Sleep -Milliseconds 300
    [W]::CtrlKey($h, 0x48)           # Ctrl+H again - the bar comes back on the
    Start-Sleep -Milliseconds 300    # replacement field
    [W]::TypeAscii($h, $replacement)
    [W]::Enter($h)                   # replace everywhere
    Start-Sleep -Milliseconds 600
    [W]::CtrlS($h)
    Start-Sleep -Milliseconds 700
    # The capture is opt-in (see the -Capture switch): PrintWindow can wait
    # forever for a window that is not drawing.
    if ($Capture) { [void][W]::Capture($h, (Join-Path $Screenshots "m083-find.png")) }
    Write-Output ("[{0}] session A: capture done" -f (Get-Date -Format HH:mm:ss))

    $saved = [System.IO.File]::ReadAllText($findProbe)
    $replaced = (-not $saved.Contains("needle")) -and ($saved.Contains("$replacement first")) -and ($saved.Contains("$replacement second"))
    $dialogsA = [W]::DialogsOf([uint32]$p.Id)
    # m084: FLTK's message window is not a #32770 dialog, so it needs its own
    # counter - that class is how the popup storm of m084 hid from the gate.
    $fltkA = [W]::FltkWindowsOf([uint32]$p.Id, $h)
    $rcA = Stop-DeckLocal $p
    $stderrA = ""
    try { $stderrA = $errA.Result } catch { $stderrA = "" }
    $fltkLinesA = @($stderrA -split "`n" | Where-Object { $_ -like "[fltk]*" })
    Write-Output "Find/replace-all applied to disk: $replaced"
    Write-Output "Session A: dialogs=$dialogsA fltk-windows=$fltkA fltk-errors=$($fltkLinesA.Count) rc=$rcA ; shot=$Screenshots\m083-find.png"

    # ---- session B: the library row comes back and the deck touches nothing ----
    Remove-Item $findProbe -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($external, "#Requires AutoHotkey v2.0`r`n; external probe`r`n", $utf8)
    [System.IO.File]::WriteAllText($libPath, $external + "`r`n", $utf8)

    [System.IO.File]::WriteAllText($settingsFile, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $p2 = Start-Deck $libPath
    $errB = $p2.StandardError.ReadToEndAsync()
    [void]$p2.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $p2.Refresh()
    $h2 = $p2.MainWindowHandle
    if ($h2 -eq [IntPtr]::Zero) { throw "no main window (library session)" }

    [W]::Foreground($h2)
    [W]::Click($h2, 120, 69)          # row 1 = restored lib-probe.ahk (m024: 86 + 11)
    Start-Sleep -Milliseconds 800
    # m083: the old cell pressed "y" and demanded that the row be removed from
    # the library. The deck has no such key and no such menu item (the row
    # menu is 运行/重启/编辑/快捷键/重命名 - there is no 移除), so the cell was
    # asserting a feature that never existed. What the deck DOES promise: it
    # never touches a script it did not create. The library file and the
    # external file must come out of the session untouched.
    $libraryAfter = [System.IO.File]::ReadAllText($libPath)
    $libraryKept = $libraryAfter.Contains("lib-probe.ahk")
    $externalKept = Test-Path $external
    if ($Capture) { [void][W]::Capture($h2, (Join-Path $Screenshots "m083-library.png")) }
    $dialogsB = [W]::DialogsOf([uint32]$p2.Id)
    $fltkB = [W]::FltkWindowsOf([uint32]$p2.Id, $h2)
    $rcB = Stop-DeckLocal $p2
    $stderrB = ""
    try { $stderrB = $errB.Result } catch { $stderrB = "" }
    $fltkLinesB = @($stderrB -split "`n" | Where-Object { $_ -like "[fltk]*" })
    Write-Output "Library row still listed after the session: $libraryKept"
    Write-Output "External probe still exists on disk: $externalKept"
    Write-Output "Session B: dialogs=$dialogsB fltk-windows=$fltkB fltk-errors=$($fltkLinesB.Count) rc=$rcB ; shot=$Screenshots\m083-library.png"

    if (-not $replaced) { throw "replace-all did not reach the disk" }
    if (-not $libraryKept) { throw "the deck rewrote the library file behind the user's back" }
    if (-not $externalKept) { throw "the deck deleted a script it does not own" }
    if ($dialogsA -ne 0 -or $dialogsB -ne 0) { throw "a modal dialog appeared" }
    # m084: no FLTK popup, no FLTK error. The popup was class "FLTK" (not
    # #32770) and the error channel writes the same message to stderr.
    if ($fltkA -ne 0 -or $fltkB -ne 0) { throw "an FLTK message window appeared" }
    if ($fltkLinesA.Count -ne 0 -or $fltkLinesB.Count -ne 0) {
        throw ("FLTK reported an error: " + (($fltkLinesA + $fltkLinesB) -join " | "))
    }
}
finally {
    Remove-Item $findProbe, (Join-Path $scripts "find-out.txt"), $libPath, $external -ErrorAction SilentlyContinue
    foreach ($path in $stashed) {
        $leaf = Split-Path $path -Leaf
        if (Test-Path (Join-Path $stash $leaf)) {
            Move-Item (Join-Path $stash $leaf) $path -Force
        }
    }
}
