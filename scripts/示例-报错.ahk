#Requires AutoHotkey v2.0
; 演示：运行时错误只进控制台，不弹窗；宿主退出码为 2
; 在 AHKDeck 里运行它，看右侧日志的 fail 行与 Specifically 行

throw Error(
    "这是示例错误：脚本会以退出码 2 结束",
    "发生位置：示例-报错.ahk",
    "extra：这条会显示在 Specifically 后面"
)
