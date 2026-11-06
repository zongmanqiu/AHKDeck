#Requires AutoHotkey v2.0

; m011 probe: stays resident and appends a heartbeat line every 200 ms, so the
; gate can watch it die when the host calls StopAll. Each line carries the OS
; thread id of whoever wrote it; the timer ticks run on the script's own thread.
Beat() {
	static n := 0
	n++
	FileAppend(n "|" A_TickCount "|" DllCall("kernel32\GetCurrentThreadId") "`n", A_Args[1], "UTF-8")
	if (n >= 40)
		ExitApp 0
}
; A_WorkingDir is process-wide in AutoHotkey, so A_Args[1] arrives as an absolute
; path: a concurrent run must not be able to move the host's own files.
if FileExist(A_Args[1])
    FileDelete(A_Args[1])
SetTimer Beat, 200
