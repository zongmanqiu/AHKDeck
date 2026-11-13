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
| `ScriptCatalog.{h,cpp}` | m146 官方脚本清单与安全安装：`scripts.json` 解析（耐受 BOM、坏项跳过并计数、`name` 路径白名单、只收 https）、本地 `@version` 解析与三态判定、随包清单读写，以及 `.part → 校验 → .old → 原子替换 → 失败还原`。校验（m160）按用户口径只看头部 `@name` + `@version` 与清单严格相等（复用 `ScriptHeader` 解析器，与信息窗同一口径；官方脚本不加哈希、不比大小，旧的 `size_kb` 容差段已删除，`size_kb` 仅留在清单里供信息窗显示）。不依赖 FLTK 与线程 |
| `ScriptHeader.{h,cpp}` | m151 脚本自述：`==UserScript==` 块解析（`; @键 值`，值到行尾全部保留）、文件字节/修改时间/行数，以及 `42.8 KB（43,788 字节）` 口径。`ReadScriptVersion` 是清单与信息窗共用的唯一入口 |
| `AiPolicy.{h,cpp}` | 工具文件操作路径校验与备份，不是脚本执行沙箱。m160 起拒 UNC/`\\?\` 设备路径，并逐级检查 reparse point（junction/符号链接混入白名单目录也出不去） |
| `ResourceStats.{h,cpp}` | 宿主自身占用（CPU% / 内存＝专用工作集（任务管理器口径）/ 线程 / 句柄 / GPU，m107）。只做测量（psapi、Toolhelp、PDH），不碰窗口；状态栏与 `--stats` 共用同一份格式化 |
| `Housekeeping.{h,cpp}` | 启动清理（m109）：`settings.json` 的 `housekeeping.max_age_days`（默认 30，`0`＝关闭）驱动 `.temp\`/`Backups\`/`Logs\` 的按时间清理；递归删文件后删空目录，不跟目录联接，占用中的文件只记数 |
| `ExeCompiler.{h,cpp}` | m172 脚本编译为 EXE：对随包官方编译器（`tools\Ahk2Exe.exe` + `tools\AutoHotkey64.exe`）的短命子进程封装（同 m103 validate 的文件捕获形状，不走管道）。rc 语义实测钉死（0 成 / 17 语法错 stderr 载原文 / 50 include 缺失），`/silent` 保证永不弹窗；覆盖契约＝旧目标先改名 `.old`（同下载器），`backedUp` 只报本轮事实；工具缺失 fail-closed 报「请用完整安装包」。无 FLTK、无窗口 API，阻塞式——由调用方放 worker 线程 |

依赖方向见 [.main/目录结构说明.md](../目录结构说明.md)：`core ← engine ← ui`，反向 include 一律不许。
