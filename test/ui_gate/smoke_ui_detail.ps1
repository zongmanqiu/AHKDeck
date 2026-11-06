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
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public struct RECT { public int Left, Top, Right, Bottom; }

    public static void Click(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0201, (IntPtr)1, lp);
        System.Threading.Thread.Sleep(20);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
    }

    public static void CtrlShiftL(IntPtr h) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        keybd_event(0x10, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0100, (IntPtr)0x4C, (IntPtr)0x00260001);
        PostMessage(h, 0x0101, (IntPtr)0x4C, (IntPtr)0xC0260001);
        System.Threading.Thread.Sleep(80);
        keybd_event(0x10, 0, 2, UIntPtr.Zero);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
        System.Threading.Thread.Sleep(250);
    }

    public static void KeyTo(IntPtr h, int vk) {
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x00010001);
        System.Threading.Thread.Sleep(30);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC0010001);
        System.Threading.Thread.Sleep(200);
    }

    public static void DoubleClick(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);  // WM_MOUSEMOVE
        PostMessage(h, 0x0201, (IntPtr)1, lp);    // WM_LBUTTONDOWN
        PostMessage(h, 0x0202, IntPtr.Zero, lp);  // WM_LBUTTONUP
        System.Threading.Thread.Sleep(60);
        // The system only synthesizes WM_LBUTTONDBLCLK for hardware input;
        // a posted click sequence must send it explicitly for FLTK to see
        // event_clicks()==1.
        PostMessage(h, 0x0203, (IntPtr)1, lp);    // WM_LBUTTONDBLCLK
        PostMessage(h, 0x0202, IntPtr.Zero, lp);  // WM_LBUTTONUP
        System.Threading.Thread.Sleep(150);
    }

    public static void Resize(IntPtr h, int x, int y, int w, int hh) {
        SetWindowPos(h, IntPtr.Zero, x, y, w, hh, 0x0004 | 0x0010); // NOZORDER | NOACTIVATE
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
$work = Join-Path $root ".temp\m017"
$settings = Join-Path $work "ui.ini"
$library = Join-Path $work "library.txt"
$probe = Join-Path $scripts "ui-dbl.ahk"
$heartbeat = Join-Path $scripts "ui-dbl.txt"
$stash = Join-Path $work "scripts-stash"
New-Item -ItemType Directory -Force $scripts, $work, $stash, $Screenshots | Out-Null

function Start-Deck {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["AHKDECK_SETTINGS"] = $settings
    $psi.EnvironmentVariables["AHKDECK_LIBRARY"] = $library
    return [System.Diagnostics.Process]::Start($psi)
}

function Wait-Window($p) {
    [void]$p.WaitForInputIdle(5000)
    Start-Sleep -Milliseconds 1500
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }
    return $h
}

$stashed = @()
foreach ($f in @(Get-ChildItem (Join-Path $scripts "*.ahk") -ErrorAction SilentlyContinue)) {
    Move-Item $f.FullName (Join-Path $stash $f.Name) -Force
    $stashed += $f.FullName
}

try {
    Remove-Item $settings, $library, $heartbeat -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($settings, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $probeText = "#Requires AutoHotkey v2.0`r`nLoop 40 {`r`n    FileAppend ""beat``n"", A_ScriptDir ""\ui-dbl.txt""`r`n    Sleep 200`r`n}`r`n"
    [System.IO.File]::WriteAllText($probe, $probeText, (New-Object System.Text.UTF8Encoding($false)))

    $explorersBefore = @(Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })

    # ---- session 1: double-click run, logs button, resize, close ----
    $p = Start-Deck
    $h = Wait-Window $p
    [W]::Click($h, 120, 69)          # select row 1 (m033: list top 58 + 10)
    Start-Sleep -Milliseconds 80
    [W]::Click($h, 120, 69)          # second press within the double-click time runs it
    Start-Sleep -Milliseconds 1500
    [void][W]::Capture($h, (Join-Path $root ".screenshots\m017-dbl.png"))

    $started = Test-Path $heartbeat
    $len1 = if ($started) { (Get-Item $heartbeat).Length } else { 0 }
    Start-Sleep -Milliseconds 700
    $len2 = if ($started) { (Get-Item $heartbeat).Length } else { 0 }
    Write-Output "Double-click started the script: $started (heartbeat $len1 -> $len2)"

    [W]::Resize($h, 150, 120, 940, 620)
    Start-Sleep -Milliseconds 600
    $rect1 = New-Object W+RECT
    [void][W]::GetWindowRect($h, [ref]$rect1)
    Write-Output "Resized outer rect: $($rect1.Left),$($rect1.Top) $($rect1.Right - $rect1.Left)x$($rect1.Bottom - $rect1.Top)"

    [W]::KeyTo($h, 0x7A)  # 日志 (m026 dropped the toolbar menu button again)
    Start-Sleep -Milliseconds 900
    [void][W]::Capture($h, (Join-Path $root ".screenshots\m017-detail.png"))
    $dialogsA = [W]::DialogsOf([uint32]$p.Id)

    $newExplorers = @(Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object { $explorersBefore -notcontains $_.Id })
    foreach ($e in $newExplorers) { Stop-Process -Id $e.Id -Force -ErrorAction SilentlyContinue }
    Write-Output "Logs button spawned explorer processes (closed by test): $($newExplorers.Count)"

    Stop-Deck -Process $p -Exe $Exe | Out-Null
    Write-Output "Session 1: dialogs=$dialogsA rc=$($p.ExitCode)"

    $heartbeatAfter = if (Test-Path $heartbeat) { (Get-Item $heartbeat).Length } else { 0 }
    Start-Sleep -Milliseconds 700
    $heartbeatLater = if (Test-Path $heartbeat) { (Get-Item $heartbeat).Length } else { 0 }
    Write-Output "Heartbeat after close: $heartbeatAfter -> $heartbeatLater (stopped=$($heartbeatLater -eq $heartbeatAfter))"

    $savedIni = if (Test-Path $settings) { [System.IO.File]::ReadAllText($settings) } else { "" }
    Write-Output "Settings file: $($savedIni.Trim() -replace "`r`n", " | ")"

    # ---- session 2: geometry restored ----
    $p2 = Start-Deck
    $h2 = Wait-Window $p2
    $rect2 = New-Object W+RECT
    [void][W]::GetWindowRect($h2, [ref]$rect2)
    $dw = [math]::Abs(($rect2.Right - $rect2.Left) - ($rect1.Right - $rect1.Left))
    $dh = [math]::Abs(($rect2.Bottom - $rect2.Top) - ($rect1.Bottom - $rect1.Top))
    Write-Output "Restored outer rect: $($rect2.Left),$($rect2.Top) $($rect2.Right - $rect2.Left)x$($rect2.Bottom - $rect2.Top) (delta ${dw}x${dh})"
    $dialogsB = [W]::DialogsOf([uint32]$p2.Id)
    Stop-Deck -Process $p2 -Exe $Exe | Out-Null
    Write-Output "Session 2: dialogs=$dialogsB rc=$($p2.ExitCode)"

    if (-not $started -or $len2 -le $len1) { throw "double-click did not run the script" }
    if ($heartbeatLater -ne $heartbeatAfter) { throw "script kept running after the host closed" }
    if ($dw -gt 4 -or $dh -gt 4) { throw "window size was not restored" }
    if ($dialogsA -ne 0 -or $dialogsB -ne 0) { throw "a modal dialog appeared" }
}
finally {
    foreach ($proc in @($p, $p2)) { if ($proc -and -not $proc.HasExited) { $proc.Kill() } }
    Remove-Item $probe, $heartbeat, $settings, $library -ErrorAction SilentlyContinue
    foreach ($path in $stashed) {
        $leaf = Split-Path $path -Leaf
        if (Test-Path (Join-Path $stash $leaf)) {
            Move-Item (Join-Path $stash $leaf) $path -Force
        }
    }
}
