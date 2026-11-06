#Requires AutoHotkey v2.0
; 演示：长跑脚本（在 AHKDeck 内运行，关宿主即停止）
; 输出文件与脚本同目录；显式 UTF-8，任何工具读它都不会乱码

outFile := A_ScriptDir "\示例-心跳输出.txt"
outHandle := FileOpen(outFile, "w", "UTF-8")
outHandle.Write("开始心跳（共 60 次，每 0.5 秒一次）`r`n")
outHandle.Write("AHK 版本: " A_AhkVersion "`r`n")
outHandle.Close()

Loop 60
{
    ; 每次追加后立即关闭：外部工具随时能看到新行，停止时也不丢缓冲
    outHandle := FileOpen(outFile, "a")
    outHandle.Write("心跳 " A_Index "/60  " FormatTime(, "HH:mm:ss") "`r`n")
    outHandle.Close()
    Sleep 500
}

outHandle := FileOpen(outFile, "a")
outHandle.Write("自然结束`r`n")
outHandle.Close()
ExitApp 0
