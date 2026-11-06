#Requires AutoHotkey v2.0

; m011 probe: the script never parses, so no script thread ever reports ready.
; Its record has no thread id to route on - the session must still get it.
if (true {
	MsgBox "never"
}
