#Requires AutoHotkey v2.0

; m011 probe: unhandled runtime error on the script's own thread. Official
; AutoHotkey answers with its error box; the deck must hand the same text to the
; sink and let the session finish with the official exit code.
throw Error("m011-boom", -1)
