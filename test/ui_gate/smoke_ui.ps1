param(
    [string]$Exe = ".build\AHKDeck.exe",
    [string]$Probe = ".main\test\ui_gate\probe",
    [ValidateSet("run", "boom")][string]$Case = "run",
    [string]$Screenshots = ".temp\ui_gate_shots"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "Stop-Deck.ps1")
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    // Posted to the deck's own client area: a real cursor move would land on
    // whatever window happens to be on top of it on this desktop.
    public static void Click(IntPtr h, int x, int y) {
        y += 32; // m027: custom caption bar
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, 0x0200, IntPtr.Zero, lp);
        PostMessage(h, 0x0201, (IntPtr)1, lp);
        System.Threading.Thread.Sleep(40);
        PostMessage(h, 0x0202, IntPtr.Zero, lp);
        System.Threading.Thread.Sleep(120);
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

# Row height assumed by the fixed layout: toolbar 28 + pad 8, then the list.
# The row index is computed from the actual .ahk files so an extra script in the
# folder cannot silently shift the click target.
function RowOf([string]$name) {
    $names = @(Get-ChildItem (Join-Path $scripts "*.ahk") | Sort-Object Name | ForEach-Object { $_.Name })
    $i = [array]::IndexOf($names, $name)
    if ($i -lt 0) { throw "probe $name not in $scripts" }
    return $i + 1
}

$work = Join-Path (Get-Location) ".temp\ui_gate"
New-Item -ItemType Directory -Force $work | Out-Null
$settingsPath = Join-Path $work "ui-gate.ini"
$scripts = Join-Path (Get-Location) ".build\Scripts"
$stash = Join-Path $work "stash"
New-Item -ItemType Directory -Force $stash | Out-Null
$stashed = @()
$stashDir = Join-Path $work "scripts-stash"
Remove-Item $stashDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stashDir | Out-Null
foreach ($f in @(Get-ChildItem (Join-Path $scripts "*.ahk") -ErrorAction SilentlyContinue)) {
    Move-Item $f.FullName (Join-Path $stashDir $f.Name) -Force
    $stashed += $f.Name
}
$heartbeat = Join-Path $scripts "ui-smoke.txt"
New-Item -ItemType Directory -Force $scripts, $Screenshots | Out-Null
Remove-Item $heartbeat -ErrorAction SilentlyContinue
Copy-Item (Join-Path $Probe "*.ahk") $scripts -Force

try {
    $conhostBefore = @(Get-Process conhost -ErrorAction SilentlyContinue).Count
    [System.IO.File]::WriteAllText($settingsPath, "x=0`r`ny=0`r`nw=1180`r`nh=720`r`n", (New-Object System.Text.UTF8Encoding($false)))
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables["AHKDECK_SETTINGS"] = $settingsPath
    $p = [System.Diagnostics.Process]::Start($psi)
    Start-Sleep -Milliseconds 1500
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { throw "no main window" }
    $rect = New-Object W+RECT
    [void][W]::GetWindowRect($h, [ref]$rect)
    $w = $rect.Right - $rect.Left
    $hh = $rect.Bottom - $rect.Top
    [void][W]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 300

    $target = if ($Case -eq "boom") { "ui-boom.ahk" } else { "ui-smoke.ahk" }
    $row = RowOf $target
    [W]::Click($h, 120, 58 + (($row - 1) * 26) + 10)   # select the probe row (m035: row pitch 26)
    Start-Sleep -Milliseconds 250
    [W]::Click($h, 30, 58 + (($row - 1) * 26) + 10)   # play glyph = run
    Start-Sleep -Milliseconds 1500

    $shot = Join-Path $Screenshots ("m014-ui-$Case.png")
    [void][W]::Capture($h, (Join-Path (Get-Location) $shot))
    Write-Output "Window class = FLTK ; title = '$($p.MainWindowTitle)' ; shot = $shot"
    Write-Output "Modal dialogs (#32770) owned by host = $([W]::DialogsOf([uint32]$p.Id))"
    Write-Output "Processes: AHKDeck = $(@(Get-Process AHKDeck -ErrorAction SilentlyContinue).Count) ; conhost $conhostBefore -> $(@(Get-Process conhost -ErrorAction SilentlyContinue).Count)"

    if ($Case -eq "run") {
        if (-not (Test-Path $heartbeat)) { throw "script did not start (no heartbeat)" }
        $len1 = (Get-Item $heartbeat).Length
        Start-Sleep -Milliseconds 700
        $len2 = (Get-Item $heartbeat).Length
        Write-Output "Heartbeat while host alive: $len1 -> $len2 (growing = $($len2 -gt $len1))"
        if ($len2 -le $len1) { throw "heartbeat did not grow" }
    }
    else {
        # m083/m177: the logs live under the exe's temp\ root (the deck is
        # portable), not in the project root.
        $logDir = Join-Path (Split-Path -Parent (Resolve-Path $Exe).Path) "temp\logs"
        $log = Get-ChildItem $logDir -Filter "ui-boom-*.jsonl" |
            Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $log) { throw "no JSONL for ui-boom in $logDir" }
        $record = Get-Content $log.FullName | Select-Object -Last 1 | ConvertFrom-Json
        Write-Output "JSONL record: severity=$($record.severity) phase=$($record.phase) line=$($record.line) message=$($record.message)"
        if ($record.severity -ne "fail") { throw "expected severity=fail" }
    }

    Stop-Deck -Process $p -Exe $Exe | Out-Null
    Write-Output "Host exit after --quit: rc=$($p.ExitCode)"

    if ($Case -eq "run") {
        $len3 = (Get-Item $heartbeat).Length
        Start-Sleep -Milliseconds 800
        $len4 = (Get-Item $heartbeat).Length
        Write-Output "Heartbeat after host close: $len3 -> $len4 (stopped = $($len4 -eq $len3))"
        if ($len4 -ne $len3) { throw "script kept running after the host closed" }
    }
}
finally {
    if ($p -and -not $p.HasExited) { $p.Kill() }
    foreach ($name in $stashed) {
        $src = Join-Path $stashDir $name
        if (Test-Path $src) { Move-Item $src (Join-Path $scripts $name) -Force }
    }
    Remove-Item (Join-Path $scripts "ui-smoke.ahk"), (Join-Path $scripts "ui-boom.ahk"), $heartbeat -ErrorAction SilentlyContinue
}
