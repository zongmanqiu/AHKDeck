#Requires AutoHotkey v2.0
#SingleInstance Force
#NoTrayIcon

; m160 探针：常驻，但把自己的托盘菜单删空——正是 m158 登记的「无托盘菜单脚本」
; （窗口型、A_TrayMenu.Delete() 之后不 Add）。内核仍会给出托盘菜单句柄，但
; GetMenuItemCount == 0；DeckUI::OpenScriptMenu 的新分支读的就是这个计数。
; 对照：tray_a/tray_b 删完还 Add 一项，计数 >= 1，守卫不得触发。
A_TrayMenu.Delete()

Keep() {
}
SetTimer Keep, 500
