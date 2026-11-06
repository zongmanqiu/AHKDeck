#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon

; m024 探针 A：常驻，带一个自己的托盘菜单项。宿主不投任何脚本侧约定的消息，
; 而是投内核自己的 AHK_NOTIFYICON —— 所以这里什么都不用写就能被拉出菜单。
A_TrayMenu.Delete()
A_TrayMenu.Add("tray-a-item", (*) => 0)

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
