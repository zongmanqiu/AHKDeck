# ui/ — 工作台界面层

负责 FLTK 工作台、编辑器、控制台、对话框及 Win32 集成。依赖 `core`、`engine`、`agent`；下层不反向包含 UI。

## 文件职责

| 文件 | 职责 |
|---|---|
| `DeckUI.{h,cpp}` | UI 入口、脚本列表、编辑器、查找替换、控制台、菜单、配置接线、会话轮询与 MCP 工具任务队列 |
| `Theme.{h,cpp}` | 色板、主题初始化、`Style*` 与滚动条等绘制函数；定义 FLTK 补丁的颜色钩子 |
| `ThemeWidgets.h` | `HoverButton`、`DeckCheckButton`（画法同菜单栏的勾，未勾/勾上同宽）、`FitButtonRow`、`DeckScroll` 共享控件与按钮布局，使用 Theme 的颜色和绘制函数 |
| `DeckDialog.{h,cpp}` | 对话框公共骨架、内容起点、居中显示与显示后的窗框安装 |
| `AboutDialog.{h,cpp}` | 「关于」对话框：整列自绘面板、双仓库可点击行、内嵌二维码 |
| `UpdateDialog.{h,cpp}` | m092「帮助 > 检查更新」：状态行、更新源、当前/最新版本、说明滚动区、下载进度与 `更新`/`关闭`；检查与下载在工作线程，`Fl::awake` 回 UI |
| `DeckTitleBar.{h,cpp}` / `DeckFrame.{h,cpp}` | 自绘标题栏、窗口按钮及工作台窗框接线、缩放与最大化 |
| `WindowFrame.{h,cpp}` / `WindowShadow.{h,cpp}` | 原生窗框、圆角与分层阴影 |
| `NativeHooks.{h,cpp}` | 托盘、全局热键、隐藏控制窗口与退出通知；原生事件转成 Intent。托盘右键菜单含「运行中」：右键瞬间向 deck 问一次运行中清单（同线程 provider），每个脚本建成**真子菜单镜像**——`DeckTrayMenu(threadId)` 取脚本线程的托盘 HMENU，`CopyScriptMenu` 只读复制并逐项重编号，悬停即向右展开脚本自己的菜单，选中按记录的原始 id 回投 `WM_COMMAND` 给脚本主窗口；读不到菜单的脚本退回 m131 的两段弹出（点选后原位弹出）。无脚本运行时「运行中」灰化；「退出」前加分隔线。**m133：不含勾选/位图的菜单设 `MNS_NOCHECK`**（`fMask` 用 `MIM_STYLE`），去掉左侧勾选列留白；含勾选或 item 图标的层保留标准列，因为 NOCHECK 会连勾选标记与位图一起不画 |
| `HoverMenuBar.h` | 菜单栏悬停交互 |
| `BlinkInput.h` / `CaretBlink.h` / `ImeInPlace.{h,cpp}` | 输入控件（含框内灰色提示字）、光标闪烁与 IME 就地组合 |
| `FltkErrorChannel.{h,cpp}` | 接管 FLTK 的 `Fl::error` / `Fl::warning`：不弹窗，进 stderr 与控制台，带重入保护 |
| `HotkeyCapture.h` | 快捷键录制与显示（**m092：显示层把单个字母统一转大写**，存储仍是 AHK 语法） |
| `NativeSubclass.h` / `BandColourSource.h` | 原生子类化与条带颜色接口 |
| `DeckIcon.h` | exe 资源标识（图标与关于框二维码）；资源源文件在 `../image/` |

## 当前工作台

- 菜单为「文件 / 编辑 / 视图 / 脚本 / 设置 / 帮助」。**m092：关于从「设置」移到「帮助」**，同组新增「检查更新」；`menu.about` 的键名不变，老的重绑继续有效，`menu.checkUpdate` 是新命令。**m112：脚本列表常驻**，「视图/编辑器与控制台」只折叠编辑器列（m118 起不再默认绑 `Ctrl+2`，需要时在快捷键对话框绑定）；`AHKDECK_START_VIEW=editor`（或 `all`）启动即展开，`list` 折叠。列表与编辑器列、编辑器与控制台之间各有一条 6px 分隔条可拖拽，每个区域保留至少 20% 尺寸，占比记在 `settings.json` 的 `ui.list_pct` / `ui.console_pct`。没有内建 AI 面板。
- **m097/m098：MCP 接入不开窗口。**「帮助 > MCP 接入」把 exe 旁的 `docs\MCP接入.md` 交给系统默认程序打开（不强制记事本）。文档自身就是配置示例，不再另放 `mcp.json.example`。`docs\` 由 CMake POST_BUILD 随 exe 落到 `.build`，所以开发目录、发行包与菜单打开的是同一份文件；文件缺失或没有关联程序时向控制台写一行提示。m117：`--mcp-stdio` 启动后主窗口直接隐藏到托盘（托盘图标不可用时保持可见），AI 高频调用不再把窗口弹到用户面前；托盘图标或已绑定的「窗口 > 显示主窗口」热键随时唤回。
- **m092：自动更新。** `core/Updater.{h,cpp}`（FLTK-free）：清单 `update.json`（`version` / `published` / `notes` / `notes_en` / `url_gitee` / `url_github`），双源回退后读本地缓存 `update.cache.json`（**只有本次联网成功才能说「已是最新」**，缓存只报「上次检查结果」）；下载 WinHTTP 手跟 3xx、宽字符写盘、有长度就校验；升级脚本 `ahkdeck_update.bat`（UTF-8 BOM + `chcp 65001`，只杀本安装目录的 `AHKDeck.exe`，`robocopy` 覆盖失败保留包与 `ahkdeck_update.log`）。默认清单在 Gitee/GitHub 的 `main` 分支，可由 `settings.json` 的 `update.urlGitee` / `update.urlGithub` 覆盖（显式空串＝停用该源），`update.source` 记住选中的源。仓库尚未发布 `update.json` 时，检查会如实报「联网检查失败」，不会假称最新。
- **m088：菜单与命令合为一张表。** `DeckWindow::CommandSpec { id, key, path, group, label, shortcut, flags, editorOnly }`（`DeckUI.cpp` 的 `DeckWindow::Commands()`）是**唯一真相**：菜单由它生成，快捷键对话框的菜单行由它列出，硬编码按键全部改成经它派发。`path` 非空 ⇒ 有菜单项；`path` 为空 ⇒ 只有按钮（中部条带的清空/运行、编辑区重新载入、控制台复制）。m090：中部条带里与文件有关的「保存」「关闭脚本」也进了 `文件` 菜单（`Ctrl+S` 仍在），按钮与菜单项是同一个命令 id。
- **「编辑」菜单**：撤销 `Ctrl+Z` / 重做 `Ctrl+Y` / 剪切 `Ctrl+X` / 复制 `Ctrl+C` / 粘贴 `Ctrl+V` / 删除 `Delete`（有选区删选区，无选区删光标后一个字符）/ 全选 `Ctrl+A` / 查找… `Ctrl+F` / 查找下一个 `F3` / 查找上一个 `Shift+F3` / 替换… `Ctrl+H`。编辑命令只作用于编辑器（`editorOnly`）：焦点在查找框/过滤框时不会把粘贴塞进代码。剪贴板由 FLTK 自己做（`Fl::copy` / `Fl::paste`），`EditorBox::handle()` 处理 `FL_PASTE`。
- **m118：默认快捷键只留记事本那一套。** `新建 Ctrl+N`、`保存 Ctrl+S`、`撤销 Ctrl+Z`、`重做 Ctrl+Y`、`剪切 Ctrl+X`、`复制 Ctrl+C`、`粘贴 Ctrl+V`、`删除 Del`、`全选 Ctrl+A`、`查找… Ctrl+F`、`查找下一个 F3`、`查找上一个 Shift+F3`、`替换… Ctrl+H`。其余命令（退出、重新扫描、视图切换、清空控制台、编辑区重新载入、控制台复制、打开日志文件夹）默认一律不绑定；菜单、按钮与托盘仍能触达，「恢复默认」恢复的就是这一小套。
- **快捷键对话框有三族行**：菜单命令行名 **「菜单 > 文件 > 新建」**（group ＋ path，`PathLabel()` 把 `/` 换成 " > "），只有按钮的命令是「窗口 > 中部条带 > 清空」这类；窗口级动作是 **「窗口 > 显示主窗口」**（m108，保留键 `window.show`，系统级热键，托盘隐藏时也生效，无内置默认）；脚本开关行是 **「开关 > 脚本名」**。三族在同一张表里，冲突检测跨族。**m090：组合键唯一，冲突时"后写的赢"**——录入的新键会把原持有者（菜单项或另一个脚本）置空，不弹"已属于"、不拒绝；保存路径再用 `core/MakeShortcutsUnique()` 兜底（手改文件留下的重复项，前一条清空、后一条保留）；空条目只在"某个内置默认键被显式解绑"时写，本来就无默认键的命令不写行（否则 `hotkeys.txt` 会被 `menu.about=` 这类空行塞满）。m116/m117：**系统级的两族（`窗口 > 显示主窗口`、`开关 > 脚本名`）名称一律红色**（已绑定与否都红），列表顶部有一行红字「红色为全局快捷键」；组合键文字只在已绑定时为红色。
- **作用域分层**：菜单/命令快捷键是**窗口内**的（`Fl::test_shortcut()`，在 `EditorBox::handle()` 与 `DeckWindow::handle()` 里派发）；脚本开关仍是**系统全局**（`RegisterHotKey`，托盘时也生效，行为未改）。因此 `core/Hotkeys.cpp` 有两个作用域：`ParseAhkHotkey()`（System，**必须带修饰键，但裸功能键 F1–F24 例外**，m117）与 `ParseMenuShortcut()`（Menu，允许裸命名键 F1–F24/Del，但裸字母数字仍然拒绝）。派发前先与 `Fl::compose()` 对齐：事件带可打印字符时就是文本，不走快捷键——`Fl::event_key()` 是粘滞的（字符输入不改 `e_key`），不加这条判据的话按过 `Delete`／`F3` 之后打的每个字都会被当成快捷键吃掉。
- **持久化只存覆盖项**：`hotkeys.txt` 里 `menu.<key>=<AHK 组合>` 与脚本热键同一文件同一语法。缺条目＝内置默认；等于默认＝删掉条目（"恢复默认"因此不必把默认值写进文件）；空值＝显式解绑。保存后立刻 `ApplyMenuShortcuts()`，菜单显示的就是当前生效的键。
- 脚本来自程序旁的 `Scripts/`，支持新建、重扫、过滤与排序。单击行选中，双击启停；编辑需显式触发。快捷键格可录制组合键，齿轮格用于弹出运行中脚本自己的托盘菜单。列表与托盘「运行中」显示脚本名时去掉 `.ahk` 后缀（`DisplayNameOf`，只改显示层，文件名与 `hotkeys.txt` 的键不变）。列表为空时按「一个脚本都没有」与「筛选后没有匹配」分别提示（判据是脚本总数，不是可见行数）；`Scripts/` 缺失时在扫描前自建（m125）。
- 首列只表示动作：绿色播放启动、红色暂停停止。正在编辑的脚本名为红色，未保存时附 `*`；最近运行失败用行左侧红条提示，不再用钢笔覆盖启停动作。
- 编辑器中部条带提供保存、关闭、清空与运行/停止。运行同一脚本前先保存未保存内容，保存失败不运行。脚本运行回执和内核错误只按当前编辑文件归属路由，运行其他行不认领控制台；工作台级操作提示仍可能写入该区域。
- 查找替换条在**编辑器正上方**，整体遵循查找条统一样式：**一条背景带**（与脚本列表顶部表头同色 `background3`）上放两行，**条高 32**（`kFindRowH`），控件高 26（`kFindFieldH`），第一行 `查找`（固定 120px 输入框，框内有灰色提示字）＋`下一个`＋`上一个`＋`区分大小写`＋`全词匹配`＋`循环`三个开关＋**居中的匹配计数**（`k/N`）＋行尾的收起开关 `X`，第二行 `替换为`＋`替换`＋`全部替换`。三个开关是 `ui/ThemeWidgets.h` 的 `DeckCheckButton`：14px 方框，**两态同宽**（未勾 = 单层 1px 空心框，勾上 = 同样一个方框填主题强调色），不再用 FLTK 自带的 `Fl_Check_Button`（`gtk+` 方案下它的勾是黑的，与菜单不配）。列太窄时按固定让位顺序依次让出：`上一个` → `全词匹配` → `循环` → 大小写框缩成 `Aa` → `下一个`。按钮宽度一律按自己的文字（统一宽度规则），输入框**不拉伸**。`Ctrl+F`、`Ctrl+H`、`F3`、`Shift+F3`、`Ctrl+S` 由**窗口**认领，**与焦点无关**；查找/替换完成后焦点交回编辑器。
- **查找条是开关**：`Ctrl+F`／`Ctrl+H`／「编辑」菜单的查找·替换都是 **toggle** —— 栏在就收起（编辑区收回 64px），不在就展开并聚焦；菜单项按可见态打勾（`SyncFindMenuState()`），**默认折叠**（`mFindBarVisible = false`）。`F3`／`Shift+F3` 只展开不收起；行尾 `X`（`mFindClose`，右边距 6px，与查找条布局的 `GAP` 常量一致）同样收起。展开/收起都走 `LayoutFindBar()`（`ApplyView()` 会把编辑器面板的子控件全部显示出来，所以收起必须在布局里落实）。已知副作用：`Ctrl+F` 之后按 `Ctrl+H` 会收起（toggle 的定义，不是 bug）。
- **编辑区右键菜单**：按命令项的**实时** `shortcut()` 挑出编辑命令（撤销/重做/剪切/复制/粘贴/删除/全选），平铺 ＋ 分隔线 ＋「视图 ▶」逐项同款（含翻译/快捷键/勾选态）；项的 `user_data` 就是命令 id，走 `HandleMenu()`。入口在 `EditorBox::handle()` 的右键分支。
- 尺寸只有三档，全局统一：**行**（列表行、列标题）26，**控件**（按钮、输入框、选框）26，**条**（查找条的行、编辑器中部的操作带 `kPaneBarH`）32 —— 条 = 控件 26 + 上下各 3px 空气，控件的框线因此永远不会贴到条边。字号统一 16px，没有例外。
- 查找语义：`下一个` / `上一个` 在**匹配表**上走，计数 `k/N` 与选中项永远一致；`循环` 打开时到头回到开头，关闭时到头停下并说明（控制台）；`全词匹配` 要求匹配前后不是字母/数字/下划线/汉字（FLTK 的 `search_forward` 没有这个选项，由 `IsWholeWordMatch()` 自己判），`全部替换` 同样遵守这三个开关。
- FLTK 自己的报错（`Fl::error` / `Fl::warning`）**不弹窗**，走 `ui/FltkErrorChannel`：进 stderr（带 `[fltk]` 前缀）与控制台，并带重入保护（控制台本身是文本显示控件，写它可能再触发错误）。`Fl::fatal` 不接管。
- 载入脚本后**编辑器立刻拿到键盘焦点**（点已打开的行、`F2` 打开同样如此）⇒ 打开即能直接打字。
- 读取编码由 `core/TextFile` 处理；UI 打开非规范文件时尝试统一为 UTF-8 无 BOM、CRLF，保存也使用此格式。打开文件可能改写磁盘，不能当作只读预览。
- 标题栏关闭通常隐藏到托盘；托盘退出、文件退出或 `--quit` 才真正结束宿主。托盘安装失败时关闭退回退出。程序、标题栏与托盘使用已有图标资源。
- 界面配置默认在程序旁 `settings.json`，快捷键在 `hotkeys.txt`；配置路径支持 `AHKDECK_CONFIG` / `AHKDECK_SETTINGS`，快捷键路径支持 `AHKDECK_HOTKEYS`。日志由 engine 写到程序旁 `Logs/`。

## 线程与复用边界

`OnTick` 消费 engine 记录、原生 Intent 和 `ToolJob`。MCP 线程通过编辑区快照读取内容，通过 sink 排队请求 UI 更新，不直接操作控件；没有另一套 AI 事件队列。

新增对话框复用 `DeckDialog`，内容放在 `ContentTop()` 以下，经 `ShowCentered()` 显示；按钮和滚动容器复用 `ThemeWidgets.h`，不要复制控件实现。`Theme.h` 不承担控件类定义。

FLTK 的 `FL_MOVE` 派发给**最后创建**且包含指针的子控件：覆盖整面板的 `BorderOnlyBox`（`mEditorBorder`）必须建在需要悬停的子控件**之前**，否则 `Fl::belowmouse()` 永远是它，按钮收不到 `FL_ENTER`（查找栏按钮的悬停变色即由此修好）。

MCP 行为见 [工具说明](../agent/README.md)。验证范围见 [开发指南](../开发指南.md) 与 [UI 门禁说明](../test/ui_gate/README.md)。
