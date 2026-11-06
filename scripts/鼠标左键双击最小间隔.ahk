#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon
#Include AHKDeckBridge.inc

gIni := A_ScriptDir "\鼠标左键双击最小间隔.ini"
gInterval := Integer(IniRead(gIni, "Settings", "Interval", 50))
gLast := 0
gBlocked := false

MouseProc(nCode, wParam, lParam) {
    global gInterval, gLast, gBlocked
    if nCode >= 0 && (wParam = 0x0201 || wParam = 0x0202) {
        if wParam = 0x0202 {
            if gBlocked {
                gBlocked := false
                return 1
            }
        } else if !(NumGet(lParam, 12, "UInt") & 0x03) {
            now := A_TickCount
            diff := now - gLast
            if diff < 0
                diff += 4294967296
            if gLast && diff < gInterval {
                gLast := now
                gBlocked := true
                return 1
            }
            gLast := now
        }
    }
    return DllCall("CallNextHookEx", "Ptr", 0, "Int", nCode, "Ptr", wParam, "Ptr", lParam, "Ptr")
}

gCb := CallbackCreate(MouseProc, "F", 3)
gHook := DllCall("SetWindowsHookEx", "Int", 14, "Ptr", gCb, "Ptr", 0, "UInt", 0, "Ptr")
if !gHook {
    MsgBox "鼠标钩子安装失败，错误码 " A_LastError, "鼠标左键双击最小间隔", "Iconx"
    ExitApp
}
OnExit OnScriptExit

OnScriptExit(*) {
    DllCall("UnhookWindowsHookEx", "Ptr", gHook)
    return 0
}
SetTimer KeepAlive, 5000

KeepAlive() {
}

ShowSettings() {
    global gInterval, gIni
    g := Gui(, "鼠标左键双击最小间隔")
    g.SetFont("s10", "Microsoft YaHei UI")
    g.MarginX := 14, g.MarginY := 14
    g.OnEvent("Close", (*) => g.Destroy())
    g.Add("Text",, "连击最小间隔（ms）：")
    ed := g.Add("Edit", "w120", gInterval)
    g.Add("Button", "xm y+14 w90 Default", "保存").OnEvent("Click", (*) => SaveSettings(g, ed))
    g.Add("Button", "x+12 w90", "取消").OnEvent("Click", (*) => g.Destroy())
    g.Show()
}

SaveSettings(g, ed) {
    global gInterval, gIni
    v := Integer(ed.Value)
    if v < 10 || v > 500 {
        MsgBox "请输入 10~500 之间的毫秒数。", "设置", "Iconx T1.5"
        return
    }
    gInterval := v
    IniWrite v, gIni, "Settings", "Interval"
    g.Destroy()
}

A_TrayMenu.Delete()
A_TrayMenu.Add("设置", (*) => ShowSettings())
A_TrayMenu.Add("退出", (*) => ExitApp())
A_TrayMenu.Default := "设置"
A_IconTip := "鼠标左键双击最小间隔"
