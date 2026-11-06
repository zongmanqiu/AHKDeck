#Requires AutoHotkey v2.0
FileAppend "m014 ui smoke: started`n", A_ScriptDir "\ui-smoke.txt"
Loop 30 {
    FileAppend "beat`n", A_ScriptDir "\ui-smoke.txt"
    Sleep 200
}
