param(
    [string]$Exe = ".build\AHKDeck.exe",
    [string]$Screenshots = ".temp\m083_shots",
    # Screenshots are a by-product, not a judgement: PrintWindow waits for a
    # window that is drawing, and on a busy desktop that wait never ends.
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
    public struct RECT { public int Left, Top, Right, Bottom; }

    // PostMessage-based input: deterministic and immune to focus stealing.
    // A launcher overlay grabbed the foreground during an earlier attempt and
    // every real mouse/key event went there - these never do.
    public static void PressF2(IntPtr h) {
        PostMessage(h, 0x0100, (IntPtr)0x71, (IntPtr)0x003C0001);
        PostMessage(h, 0x0101, (IntPtr)0x71, (IntPtr)0xC03C0001);
        System.Threading.Thread.Sleep(120);
    }

    public static void Click(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);          // WM_MOUSEMOVE
        PostMessage(h, 0x0201, (IntPtr)1, lp);            // WM_LBUTTONDOWN
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);          // WM_LBUTTONUP
        System.Threading.Thread.Sleep(120);
    }

    public static void TypeAscii(IntPtr h, string s) {
        foreach (char c in s) {
            PostMessage(h, 0x0102, (IntPtr)c, (IntPtr)1); // WM_CHAR
            System.Threading.Thread.Sleep(8);
        }
        System.Threading.Thread.Sleep(120);
    }

    // Ctrl is pressed with keybd_event (so GetKeyState reports it), while the
    // WM_KEYDOWN itself is posted to our window, which is what FLTK reads.
    public static void CtrlS(IntPtr h) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);
        // m083: the Ctrl press travels through the system input queue; the app
        // only learns about it when it pumps messages. A short pause used to
        // race, and then Ctrl+S arrived without Ctrl and did nothing.
        System.Threading.Thread.Sleep(250);
        PostMessage(h, 0x0100, (IntPtr)0x53, (IntPtr)0x001F0001); // WM_KEYDOWN 'S'
        System.Threading.Thread.Sleep(120);
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
            bool ok = PrintWindow(h, hdc, 2); // PW_RENDERFULLCONTENT
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

$work = Join-Path (Get-Location) ".temp\ui_gate"
New-Item -ItemType Directory -Force $work | Out-Null
$settingsPath = Join-Path $work "editor-ui.ini"
$scripts = Join-Path (Get-Location) ".build\Scripts"
$target = Join-Path $scripts "ui-editor.ahk"
New-Item -ItemType Directory -Force $scripts, $Screenshots | Out-Null
$original = "#Requires AutoHotkey v2.0`r`nFileAppend ""editor probe``n"", A_ScriptDir ""\editor-out.txt""`r`nSleep 100`r`n"
$stashDir = Join-Path $work "scripts-stash"
Remove-Item $stashDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stashDir | Out-Null
$stashed = @()
foreach ($f in @(Get-ChildItem (Join-Path $scripts "*.ahk") -ErrorAction SilentlyContinue)) {
    Move-Item $f.FullName (Join-Path $stashDir $f.Name) -Force
    $stashed += $f.Name
}
[System.IO.File]::WriteAllText($target, $original, (New-Object System.Text.UTF8Encoding($false)))
Remove-Item (Join-Path $scripts "editor-out.txt") -ErrorAction SilentlyContinue

try {
    [System.IO.File]::WriteAllText($settingsPath, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["AHKDECK_SETTINGS"] = $settingsPath
    $p = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 5000
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }

    # Select the probe row (list starts at y=86 since m024, item height 22).
    $names = @(Get-ChildItem (Join-Path $scripts "*.ahk") | Sort-Object Name | ForEach-Object { $_.Name })
    $row = [array]::IndexOf($names, "ui-editor.ahk") + 1
    if ($row -lt 1) { throw "probe row not found" }
    [W]::Click($h, 120, 58 + (($row - 1) * 26) + 18)   # m024: the list gained a 30px header row
    Start-Sleep -Milliseconds 400
    [W]::PressF2($h)   # m022: selection no longer loads; F2 == right-click 编辑
    Start-Sleep -Milliseconds 600

    # The synthetic click occasionally lands before the list is ready; retry
    # selection/F2 once. Opening the same file twice is a no-op.
    [W]::Click($h, 120, 58 + (($row - 1) * 26) + 18)
    Start-Sleep -Milliseconds 300
    [W]::PressF2($h)
    Start-Sleep -Milliseconds 600

    # m083: no click into the editor any more. F2 already loads the script and
    # puts the caret in it (LoadIntoEditor takes the focus), so the keys land
    # where the user would type them. The old click at (520,200) landed in the
    # LIST pane - the list got much wider in m037 - and stole the focus back,
    # which is why the marker never reached the disk for weeks.
    Start-Sleep -Milliseconds 300
    [W]::TypeAscii($h, "; m015-editor-mark")
    Start-Sleep -Milliseconds 300

    [W]::CtrlS($h)
    Start-Sleep -Milliseconds 700
    $method = "Ctrl+S"
    $saved = [System.IO.File]::ReadAllText($target).Contains("m015-editor-mark")

    $shot = Join-Path (Get-Location) (Join-Path $Screenshots "m015-editor.png")
    $captured = $false
    if ($Capture) { $captured = [W]::Capture($h, $shot) }

    $bytes = [System.IO.File]::ReadAllBytes($target)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $hasCrlf = ($bytes -contains 13)

    # Test loop: 保存并运�?must save and then execute inside this host.
    [W]::CtrlS($h)
    [W]::Click($h, 30, 58 + (($row - 1) * 26) + 18)
    Start-Sleep -Milliseconds 5000
    $ran = Test-Path (Join-Path $scripts "editor-out.txt")
    $shot2 = Join-Path (Get-Location) (Join-Path $Screenshots "m015-editor-run.png")
    if ($Capture) { [void][W]::Capture($h, $shot2) }

    Write-Output "Saved via: $method"
    Write-Output "Editor screenshot (PrintWindow): $captured -> $shot"
    Write-Output "On-disk format: BOM=$hasBom CRLF=$hasCrlf marker=$saved"
    Write-Output "Save-and-run executed the saved script: $ran (shot: $shot2)"
    Write-Output "Modal dialogs (#32770): $([W]::DialogsOf([uint32]$p.Id))"

    Stop-Deck -Process $p -Exe $Exe | Out-Null
    Write-Output "Host exit after --quit: rc=$($p.ExitCode)"

    if (-not $saved) { throw "edit marker never reached the disk" }
    if ($hasBom) { throw "BOM was added unexpectedly" }
    if (-not $hasCrlf) { throw "CRLF line endings were not preserved" }
    if (-not $ran) { throw "save-and-run did not execute the script" }
}
finally {
    if ($p -and -not $p.HasExited) { $p.Kill() }
    foreach ($name in $stashed) {
        $src = Join-Path $stashDir $name
        if (Test-Path $src) { Move-Item $src (Join-Path $scripts $name) -Force }
    }
    Remove-Item $target, (Join-Path $scripts "editor-out.txt") -ErrorAction SilentlyContinue
}
