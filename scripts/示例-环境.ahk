#Requires AutoHotkey v2.0
; 演示：脚本身份与环境与官方一致（A_ScriptDir / A_ScriptName / A_LineFile / A_Args）
; 运行后在本目录生成 UTF-8 的「示例-环境输出.txt」

outFile := A_ScriptDir "\示例-环境输出.txt"
report := ""
report .= "AHK 版本     : " A_AhkVersion "`r`n"
report .= "脚本目录     : " A_ScriptDir "`r`n"
report .= "脚本名       : " A_ScriptName "`r`n"
report .= "完整路径     : " A_ScriptFullPath "`r`n"
report .= "当前行文件   : " A_LineFile "`r`n"
report .= "工作目录     : " A_WorkingDir "`r`n"
report .= "参数个数     : " A_Args.Length "`r`n"

for index, value in A_Args
    report .= "参数 " index "       : " value "`r`n"

outHandle := FileOpen(outFile, "w", "UTF-8")
outHandle.Write(report)
outHandle.Close()
ExitApp 0
