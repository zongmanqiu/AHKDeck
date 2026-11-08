# core/ — 纯逻辑层

**规则：禁止 include 任何 `ui/`、`engine/`、FLTK 头；不用 Windows GUI API。** 这是可测试性的根基，
本模块的单元测试不依赖 GUI；项目另有真实会话、协议与 UI 门禁。

| 文件 | 职责 |
|---|---|
| `Version.{h,cpp}` | 版本常量与字符串（与 CMakeLists 的 `project(... VERSION)` 成对修改，test_version 钉住） |
| `TextFile.{h,cpp}` | 文本编码、换行与文件读写 |
| `Library.{h,cpp}` | 路径清单读写、去重；GUI 当前扫描程序旁的 Scripts |
| `Settings.{h,cpp}` | 窗口几何与旧 INI 读写；JSON 配置接线在 ui/DeckUI.cpp |
| `Hotkeys.{h,cpp}` | 快捷键语法解析与绑定存储。**两个作用域**：`ParseAhkHotkey()` 是系统全局热键（RegisterHotKey，必须至少一个修饰键，**或裸功能键 F1–F24**，m117）；`ParseMenuShortcut()` 是菜单/命令快捷键（窗口内，允许裸命名键 F1–F24/Del/Enter，裸字母数字仍拒绝） |
| `Json.{h,cpp}` | JSON 数据结构、解析与输出 |
| `AiPolicy.{h,cpp}` | 工具文件操作路径校验与备份，不是脚本执行沙箱 |
| `ResourceStats.{h,cpp}` | 宿主自身占用（CPU% / 内存＝专用工作集（任务管理器口径）/ 线程 / 句柄 / GPU，m107）。只做测量（psapi、Toolhelp、PDH），不碰窗口；状态栏与 `--stats` 共用同一份格式化 |
| `Housekeeping.{h,cpp}` | 启动清理（m109）：`settings.json` 的 `housekeeping.max_age_days`（默认 30，`0`＝关闭）驱动 `.temp\`/`Backups\`/`Logs\` 的按时间清理；递归删文件后删空目录，不跟目录联接，占用中的文件只记数 |

依赖方向见 [.main/目录结构说明.md](../目录结构说明.md)：`core ← engine ← ui`，反向 include 一律不许。
