#Requires AutoHotkey v2.0

; m011 probe: the failure happens inside a timer callback. A timer tick is an
; AHK thread, not an OS thread, so its record must still carry this session's id
; and the script thread's id - that is what makes per-session logs trustworthy.
Boom() {
	throw Error("m011-timer-boom", -1)
}
SetTimer Boom, 150
