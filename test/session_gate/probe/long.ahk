#Requires AutoHotkey v2.0

; m011 probe: stays resident and appends a heartbeat line every 200 ms, so the
; gate can watch it die when the host calls StopAll. Each line carries the OS
; process id of whoever wrote it (m192: the session IS a child process; a timer
; tick runs on the child's script thread, so the pid is the stable identity).
Beat() {
	static n := 0
	n++
	FileAppend(n "|" A_TickCount "|" DllCall("kernel32\GetCurrentProcessId") "`n", A_Args[1], "UTF-8")
	if (n >= 40)
		ExitApp 0
}
; A_WorkingDir is process-wide in AutoHotkey, so A_Args[1] arrives as an absolute
; path: a concurrent run must not be able to move the host's own files.
if FileExist(A_Args[1])
    FileDelete(A_Args[1])
SetTimer Beat, 200
