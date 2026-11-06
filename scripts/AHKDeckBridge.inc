#Requires AutoHotkey v2.0
; AHKDeck 菜单桥：被管理脚本 #Include 本文件后，AHKDeck 里右键该脚本即弹出它自己的 A_TrayMenu
; 协议：PostMessage(0x8001, x, y) 到脚本主窗口；桥接在该坐标异步弹出 A_TrayMenu
OnMessage(0x8001, AHKDeckBridge_ShowMenu)

AHKDeckBridge_ShowMenu(wParam, lParam, msg, hwnd) {
    if !A_TrayMenu
        return 0
    SetTimer () => A_TrayMenu.Show(wParam, lParam), -1
    return 0
}
