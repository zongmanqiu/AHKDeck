# Shared by the UI gates (m023).
#
# The close box now hides the deck to the tray with every script still running,
# so "WM_CLOSE and wait for the process" is no longer a way to leave. A gate that
# needs the host gone sends the explicit quit and waits for the exit it causes.
function Stop-Deck {
    param(
        [Parameter(Mandatory = $true)] $Process,
        [Parameter(Mandatory = $true)] [string]$Exe
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $Exe).Path
    # m053: target only the process this gate started, never a user's deck.
    $psi.Arguments = '--quit ' + $Process.Id
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    [void][System.Diagnostics.Process]::Start($psi)
    if (-not $Process.WaitForExit(8000)) {
        throw "deck (pid $($Process.Id)) did not exit within 8 s of --quit"
    }
    return $Process.ExitCode
}
