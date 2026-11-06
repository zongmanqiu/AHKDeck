# m013 check: launching the shipped exe with no console must not create a
# terminal. conhost.exe exists only when a console is allocated, so counting it
# across the launch is an objective reading - not "I looked and saw no black box".
# The dialog it does open (bare launch) is closed again here, and we verify it is
# really gone: leaving a modal box on a shared desktop is not cleanup.
param([string]$Exe = (Join-Path (Get-Location).Path '.build\AHKDeck.exe'))
$ErrorActionPreference = 'Stop'

Add-Type -Namespace M013 -Name U -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
public const uint WM_CLOSE = 0x0010;
'@

function Count-Procs([string]$name) {
    return @(Get-Process -Name $name -ErrorAction SilentlyContinue).Count
}

$exePath = (Resolve-Path $Exe).Path
$before = Count-Procs 'conhost'
Write-Output ('conhost before = ' + $before)

$p = Start-Process -FilePath $exePath -PassThru
Start-Sleep -Milliseconds 1200

$midConhost = Count-Procs 'conhost'
$alive = -not $p.HasExited
Write-Output ('child alive = ' + $alive + ' ; conhost during = ' + $midConhost)

# The bare launch is supposed to show exactly one thing: the version dialog (#32770).
$h = [M013.U]::FindWindowW('#32770', 'AHKDeck')
$dialogPid = 0
if ($h -ne [IntPtr]::Zero) { [void][M013.U]::GetWindowThreadProcessId($h, [ref]$dialogPid) }
Write-Output ('dialog #32770 "AHKDeck" found = ' + ($h -ne [IntPtr]::Zero) + ' pid=' + $dialogPid + ' (child pid=' + $p.Id + ')')

$verdictConsole = ($midConhost -le $before)
$verdictDialog = ($h -ne [IntPtr]::Zero) -and ($dialogPid -eq $p.Id)

# Close it and make sure the process is really gone (a swallowed first WM_CLOSE is
# a known failure mode here, so resend once).
if ($h -ne [IntPtr]::Zero) {
    [void][M013.U]::PostMessageW($h, [M013.U]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
    if (-not $p.HasExited) {
        [void][M013.U]::PostMessageW($h, [M013.U]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
    }
}
if (-not $p.HasExited) {
    try { $p.Kill(); [void]$p.WaitForExit(2000) } catch { }
    Write-Output 'NOTE: dialog close did not take; killed instead'
}
$stillWindow = [M013.U]::IsWindow($h)
Write-Output ('dialog window still alive = ' + $stillWindow + ' ; child exited = ' + $p.HasExited)

if ($verdictConsole -and $verdictDialog -and (-not $stillWindow)) {
    Write-Output 'OK: no console created on launch, only the dialog, and it closed'
    exit 0
}
Write-Output ('FAIL: consoleFree=' + $verdictConsole + ' dialogOwnedByChild=' + $verdictDialog + ' windowGone=' + (-not $stillWindow))
exit 1
