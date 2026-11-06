# core/ — 纯逻辑层

**规则：禁止 include 任何 `ui/`、`engine/`、FLTK 头；不用 Windows GUI API。** 这是可测试性的根基，
本模块的单元测试不依赖 GUI；项目另有真实会话、协议与 UI 门禁。

| 文件 | 职责 |
|---|---|
| `Version.{h,cpp}` | 版本常量与字符串（与 CMakeLists 的 `project(... VERSION)` 成对修改，test_version 钉住） |
| `TextFile.{h,cpp}` | 文本编码、换行与文件读写 |
| `Library.{h,cpp}` | 路径清单读写、去重；GUI 当前扫描程序旁的 Scripts |
| `Settings.{h,cpp}` | 窗口几何与旧 INI 读写；JSON 配置接线在 ui/DeckUI.cpp |
| `Hotkeys.{h,cpp}` | 快捷键语法解析与绑定存储 |
| `Json.{h,cpp}` | JSON 数据结构、解析与输出 |
| `AiPolicy.{h,cpp}` | 工具文件操作路径校验与备份，不是脚本执行沙箱 |

依赖方向见 [.main/目录结构说明.md](../目录结构说明.md)：`core ← engine ← ui`，反向 include 一律不许。
