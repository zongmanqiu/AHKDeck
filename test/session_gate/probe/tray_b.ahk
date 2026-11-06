#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon

; m024 探针 B：与 A 同形状，只为拿到第二个会话的独立主窗口与独立菜单。
A_TrayMenu.Delete()
A_TrayMenu.Add("tray-b-item", (*) => 0)

Keep() {
}
SetTimer Keep, 500

; 菜单是脚本线程自己的模态循环，别的进程投 WM_CANCELMODE 收不掉（实测）。
; 门禁要收尾就投 0x8003，由本线程自己 EndMenu。
OnMessage(0x8003, Dismiss)
Dismiss(wParam, lParam, msg, hwnd) {
    DllCall("user32\EndMenu")
    return 0
}
