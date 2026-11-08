# scripts/ — 随产品分发的脚本源

这里放 AHKDeck 随包分发的用户脚本（AHK v2）与随脚本分发的 include。构建时 `*.ahk` 和 `*.inc`
都会复制到 `.build/Scripts`（CMake 的 `deck_scripts` 自定义目标，每次构建都会同步，不依赖 exe
是否重链）；其余文件不随包分发。

规则：

1. 只放脚本本体与随脚本必需的资源，不放密钥、不放本地绝对路径（AGENTS 第 9 条）。
2. 脚本头部一律 `#Requires AutoHotkey v2.0`；只用官方 v2.0 语义（AHK_H 独有名按 m004 门禁禁用）。
3. 运行期生成的 `.ini` 配置文件是用户数据，不回写进本目录。

当前脚本（m109 起发行版只保留这一个）：

| 文件 | 作用 |
|---|---|
| `鼠标左键双击最小间隔.ahk` | 用鼠标钩子忽略过快的重复左键按下（默认间隔 50 ms，可在脚本自带界面里调），界面文案与配置写在脚本旁的 `鼠标左键双击最小间隔.ini`（运行期生成，不随包） |
| `AHKDeckBridge.inc` | **不是演示，是契约**：被管脚本 `#Include` 它之后，deck 里对该行按
  「脚本菜单」（或 Ctrl+Shift+M）即 `PostMessage(0x8001, x, y)`，脚本在该坐标弹自己的
  `A_TrayMenu`。`#NoTrayIcon` 的脚本靠这一条进自己的设置界面（m023） |

相对 `#Include name.inc` 由内核按**脚本自己的目录**解析（`.main\patches\AutoHotkey_H-v2.0.25\README.md`
§补丁 2 就是为了让这条在进程内也成立）。所以随包 `.inc` 必须和脚本落在同一个 `Scripts\` 目录。

脚本写文件一律显式 UTF-8（`FileOpen(..., "UTF-8")`）：`FileAppend` 默认跟随系统 ANSI 代码页，
其它工具/AI 读会乱码。`鼠标左键双击最小间隔.ahk` 随发行版分发。
