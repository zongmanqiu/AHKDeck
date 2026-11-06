param(
    [string]$Exe = ".build\AHKDeck.exe",
    [string]$Screenshots = ".screenshots"
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
    public struct RECT { public int Left, Top, Right, Bottom; }

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
            PostMessage(h, 0x0102, (IntPtr)c, (IntPtr)1);
            System.Threading.Thread.Sleep(8);
        }
        System.Threading.Thread.Sleep(150);
    }

    public static void CtrlS(IntPtr h) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0100, (IntPtr)0x53, (IntPtr)0x001F0001);
        System.Threading.Thread.Sleep(40);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
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
    [void]$p.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }

    [W]::Click($h, 120, 69)           # the only row (m024: 86 + 11)
    Start-Sleep -Milliseconds 400
    [W]::PressF2($h)                 # m022: open it in the editor
    Start-Sleep -Milliseconds 600
    [W]::Click($h, 424, 68)          # find box
    [W]::TypeAscii($h, "needle")
    [W]::Click($h, 535, 68)          # 下一�?    Start-Sleep -Milliseconds 500
    [void][W]::Capture($h, (Join-Path $root ".screenshots\m016-find.png"))

    [W]::Click($h, 641, 68)          # replace box
    [W]::TypeAscii($h, "NEEDLE")
    [W]::Click($h, 705, 68)          # 全部替换
    Start-Sleep -Milliseconds 600
    [W]::CtrlS($h)
    Start-Sleep -Milliseconds 700

    $saved = [System.IO.File]::ReadAllText($findProbe)
    $replaced = (-not $saved.Contains("needle")) -and ($saved.Contains("NEEDLE first")) -and ($saved.Contains("NEEDLE second"))
    $dialogsA = [W]::DialogsOf([uint32]$p.Id)
    $rcA = Stop-DeckLocal $p
    Write-Output "Find/replace-all applied to disk: $replaced"
    Write-Output "Session A: dialogs=$dialogsA rc=$rcA ; shot=.screenshots\m016-find.png"

    # ---- session B: library restore + remove ----
    Remove-Item $findProbe -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($external, "#Requires AutoHotkey v2.0`r`n; external probe`r`n", $utf8)
    [System.IO.File]::WriteAllText($libPath, $external + "`r`n", $utf8)

    [System.IO.File]::WriteAllText($settingsFile, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $p2 = Start-Deck $libPath
    [void]$p2.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $p2.Refresh()
    $h2 = $p2.MainWindowHandle
    if ($h2 -eq [IntPtr]::Zero) { throw "no main window (library session)" }

    [W]::Click($h2, 120, 69)          # row 1 = restored lib-probe.ahk (m024: 86 + 11)
    Start-Sleep -Milliseconds 500
    [W]::KeyTo($h2, 0x79)         # 移除
    Start-Sleep -Milliseconds 800
    $libraryAfter = [System.IO.File]::ReadAllText($libPath)
    $removed = -not $libraryAfter.Contains("lib-probe.ahk")
    [void][W]::Capture($h2, (Join-Path $root ".screenshots\m016-library.png"))
    $dialogsB = [W]::DialogsOf([uint32]$p2.Id)
    $rcB = Stop-DeckLocal $p2
    Write-Output "Library row removed and persisted: $removed"
    Write-Output "External probe still exists on disk: $(Test-Path $external)"
    Write-Output "Session B: dialogs=$dialogsB rc=$rcB ; shot=.screenshots\m016-library.png"

    if (-not $replaced) { throw "replace-all did not reach the disk" }
    if (-not $removed) { throw "removal was not persisted to the library" }
    if ($dialogsA -ne 0 -or $dialogsB -ne 0) { throw "a modal dialog appeared" }
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
