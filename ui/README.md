# ui/ — 工作台界面层

负责 FLTK 工作台、编辑器、控制台、对话框及 Win32 集成。依赖 `core`、`engine`、`agent`；下层不反向包含 UI。

## 文件职责

| 文件 | 职责 |
|---|---|
| `DeckUI.{h,cpp}` | UI 入口、脚本列表、编辑器、查找替换、控制台、菜单、配置接线、会话轮询与 MCP 工具任务队列 |
| `Theme.{h,cpp}` | 色板、主题初始化、`Style*` 与滚动条等绘制函数；定义 FLTK 补丁的颜色钩子 |
| `ThemeWidgets.h` | `HoverButton`、`FitButtonRow`、`PeciaScroll` 共享控件与按钮布局，使用 Theme 的颜色和绘制函数 |
| `DeckDialog.{h,cpp}` | 对话框公共骨架、内容起点、居中显示与显示后的窗框安装 |
| `DeckTitleBar.{h,cpp}` / `DeckFrame.{h,cpp}` | 自绘标题栏、窗口按钮及工作台窗框接线、缩放与最大化 |
| `WindowFrame.{h,cpp}` / `WindowShadow.{h,cpp}` | 原生窗框、圆角与分层阴影 |
| `NativeHooks.{h,cpp}` | 托盘、全局热键、隐藏控制窗口与退出通知；原生事件转成 Intent |
| `HoverMenuBar.h` | 菜单栏悬停交互 |
| `BlinkInput.h` / `CaretBlink.h` / `ImeInPlace.{h,cpp}` | 输入控件、光标闪烁与 IME 就地组合 |
| `HotkeyCapture.h` | 快捷键录制支持 |
| `NativeSubclass.h` / `BandColourSource.h` | 原生子类化与条带颜色接口 |
| `DeckIcon.h` | 图标标识；资源源文件在 `../image/autohotkey.ico` |

## 当前工作台

- 菜单为「文件 / 视图 / 脚本 / 设置」。两块面板为「脚本列表」「编辑器与控制台」，至少显示一块；支持 Ctrl+1/2 和 `AHKDECK_START_VIEW=list,editor`（或 `all`）。没有内建 AI 面板。
- 脚本来自程序旁的 `Scripts/`，支持新建、重扫、过滤与排序。单击行选中，双击启停；编辑需显式触发。快捷键格可录制组合键，齿轮格用于运行脚本的菜单桥。
- 首列只表示动作：绿色播放启动、红色暂停停止。正在编辑的脚本名为红色，未保存时附 `*`；最近运行失败用行左侧红条提示，不再用钢笔覆盖启停动作。
- 编辑器中部条带提供保存、关闭、清空与运行/停止。运行同一脚本前先保存未保存内容，保存失败不运行。脚本运行回执和内核错误只按当前编辑文件归属路由，运行其他行不认领控制台；工作台级操作提示仍可能写入该区域。
- 读取编码由 `core/TextFile` 处理；UI 打开非规范文件时尝试统一为 UTF-8 无 BOM、CRLF，保存也使用此格式。打开文件可能改写磁盘，不能当作只读预览。
- 标题栏关闭通常隐藏到托盘；托盘退出、文件退出或 `--quit` 才真正结束宿主。托盘安装失败时关闭退回退出。程序、标题栏与托盘使用已有图标资源。
- 界面配置默认在程序旁 `settings.json`，快捷键在 `hotkeys.txt`；配置路径支持 `AHKDECK_CONFIG` / `AHKDECK_SETTINGS`，快捷键路径支持 `AHKDECK_HOTKEYS`。日志由 engine 写到程序旁 `Logs/`。

## 线程与复用边界

`OnTick` 消费 engine 记录、原生 Intent 和 `ToolJob`。MCP 线程通过编辑区快照读取内容，通过 sink 排队请求 UI 更新，不直接操作控件；没有另一套 AI 事件队列。

新增对话框复用 `DeckDialog`，内容放在 `ContentTop()` 以下，经 `ShowCentered()` 显示；按钮和滚动容器复用 `ThemeWidgets.h`，不要复制控件实现。`Theme.h` 不承担控件类定义。

菜单桥契约见 [脚本说明](../scripts/README.md)，MCP 行为见 [工具说明](../agent/README.md)。验证范围见 [开发指南](../开发指南.md) 与 [UI 门禁说明](../test/ui_gate/README.md)。
