# FLTK 源码修改记录

本目录归档 AHKDeck 使用的 FLTK 1.4.5 修改文件，由 [3_patch.bat](../../build/3_patch.bat)
单向同步到工作区的 `.3rd/fltk-1.4.5/`。上游为 `fltk/fltk` 的 `release-1.4.5`，
下载来源见 [deps.txt](../../build/deps.txt)。补丁为本项目自研维护；来源注释与实际 `deck*` 符号一致，
不代表 AHKDeck 存在原项目的构建目标。升级时以归档源码为准重放并核验修改。

## Fl_Menu_.H — check_color() 属性 (2026-09-07)

在 `Fl_Menu_` 类中新增 `check_color_` 成员和 `check_color()` / `check_color(Fl_Color)` 方法。
用于分离菜单勾选框填充色与行悬停高亮色：行悬停仍用 `selection_color()`，
勾选框填充用 `check_color()`（AHKDeck 在 [ui/DeckUI.cpp](../../ui/DeckUI.cpp) 中为菜单栏设为 `colors.highlight2`）。
默认值 0 表示回退到 `selection_color()`。

**修改文件：** `FL/Fl_Menu_.H`

## Fl_Menu.cxx — 勾选框填充使用 check_color() (2026-09-07)

`Fl_Menu_Item::draw()` 中，FL_MENU_RADIO 和 FL_MENU_TOGGLE 的勾选框填充色
从 `m->selection_color()` 改为 `m->check_color()`（为 0 时回退到 `selection_color()`）。

**修改文件：** `src/Fl_Menu.cxx`

## Fl_Menu.cxx — g_popupCheckColor 回退 (2026-09-10)

`Fl_Menu_Item::draw()` 中，勾选框填充色先取非零的 `check_color()`，再取非零的
`g_popupCheckColor`；否则使用菜单的 `selection_color()`，无菜单对象时使用 `FL_SELECTION_COLOR`。

全局回退使弹出菜单不必依赖当前菜单对象的勾选色。AHKDeck 在
[ui/Theme.cpp](../../ui/Theme.cpp) 定义 `g_popupCheckColor`，由 `ApplyDeckTheme()`
设置为 `kLight.highlight2`，不是由编辑器在每次 popup 前后设置、清零。

**修改文件：** `src/Fl_Menu.cxx`

## Fl_Text_Display.cxx — 下划线厚度 1px (2026-09-10)

`draw_string()` 中 `ATTR_UNDERLINE` 分支的下划线厚度从 `pitch = fsize/7`（16px 字体
下为 2px）改为固定 `pitch = 1`（1px）。使 URL 等下划线标记更纤细精致。

**修改文件：** `src/Fl_Text_Display.cxx`

## Fl_Menu.cxx — 禁用内部 fakemenu 冗余窗口 (legacy Patch 2)

归档补丁禁用冗余标题窗口及菜单栏按钮的 `fakemenu` 创建路径，
并处理对应 `title` 状态，避免额外覆盖窗口和重绘；并非取消正常子菜单窗口。

**修改文件：** `src/Fl_Menu.cxx`

## Fl_Menu.cxx — 菜单高亮切换强制双缓冲 (legacy Patch 5)

菜单项高亮（hover / 当前项）切换时，强制走双缓冲重绘路径，避免高亮残影/闪烁。

**修改文件：** `src/Fl_Menu.cxx`

## Fl_Text_Buffer(.H/.cxx) — 撤销事务分组 (legacy Patch 6)

为 `Fl_Text_Buffer` 增加**撤销事务（Undo Transaction）**机制，使一次程序化多步
编辑（如脚本替换选区、批量插入）在撤销栈中合并为**单个 undo 步骤**：

- 实际方法为 `undo_begin()` / `undo_end()`，由 `mUndoGrouping` 计数支持嵌套；
- `insert_()` / `remove_()` 以 `group` 标记事务状态，限制跨事务状态的事件合并；
- `undo()` / `redo()` 按分组回放，并非在最外层结束时才生成一条记录。

**修改文件：** `src/Fl_Text_Buffer.cxx`、`FL/Fl_Text_Buffer.H`

## Fl_Text_Display.cxx — 垂直滚动共享上限

可见行数按完整行计算，`DECK_BLANK_ROWS_PAST_END = 3` 允许末行后保留空白行。
`scroll_()` 经 `vscroll_bottom_line()` 计算并保存实际滚动上限，
`update_v_scrollbar()` 优先复用 `vscroll_applied_limit`，无有效值时才重新计算。
当前实现不是简单封顶到 `mTopLineNum`，以免在文档中途压缩滚动范围。

**修改文件：** `src/Fl_Text_Display.cxx`

## Fl_Scrollbar.cxx — 滚动条单一整轨道（去箭头区）(2026-10-13)

`Fl_Scrollbar::handle()` 与 `draw()` 移除两端箭头区裁剪，使用完整轨道。
AHKDeck 的 [ui/Theme.cpp](../../ui/Theme.cpp) 使用 `ThumbGeometry()` 计算自绘滑块，
由实际函数 `DeckDrawScrollbar()` / `DeckDrawScrollbarCorner()` 绘制，使可见几何与命中区一致。
这些来源标识仍是实际符号，不应仅为统一文案而改名。

**修改文件：** `src/Fl_Scrollbar.cxx`

## Fl_Menu.cxx — 弹出菜单分隔线扁平化（2026-10-22）

`Fl_Menu_Item::draw()` 中，`FL_MENU_DIVIDER` 分隔线由双色立体线改为
**一条 1px 纯色扁平线**，颜色取 `extern Fl_Color g_borderColour`。
该全局由 AHKDeck 的 [ui/Theme.cpp](../../ui/Theme.cpp) 定义，应用主题时赋值为
`ThemeBorderLine()`，即 `kLight.text1` 与 `kLight.background1` 按 `0.55f` 混合，
不是直接取 `text1`，也不表示已实现暗色主题切换。使用此菜单绘制路径的弹层共用该颜色。
`g_borderColour` 和 `g_popupCheckColor` 均需要宿主定义，不能假定所有 FLTK 工具都可独立链接。

**修改文件：** `src/Fl_Menu.cxx`

## Fl_Menu.cxx — 弹出菜单外发光钩子（2026-10-22）

`menuwindow::show()` / `hide()` 调用实际符号 `deckMenuShadowShown` / `deckMenuShadowHidden`。
AHKDeck 的 [ui/WindowShadow.cpp](../../ui/WindowShadow.cpp) 提供实现：启用阴影时注册
`g_borderColour` 并附着阴影；关闭菜单时先分离阴影，再隐藏菜单窗口。

[CMakeLists.txt](../../CMakeLists.txt) 将 `ui/Theme.cpp` 和 `ui/WindowShadow.cpp` 编入
`deckui`，由 `AHKDeck` 链接。MSVC 下补丁通过 `/alternatename` 为两个阴影钩子提供空桩，
但不替代两个颜色全局的宿主定义。当前构建关闭 FLTK 的 fluid、fltk-options、示例及测试目标。

**修改文件：** src/Fl_Menu.cxx

## Fl_Text_Display.cxx — 中英混排 / 超长 token 换行（2026-10-24）

在原有“像素贪心 + CJK 断点 + 标点禁则”基础上做最小增强，不做 K-P、不做全局回看：

- 新增断点：CJK ↔ 拉丁/数字边界（UAX #14 允许；解决“中文English”整段不可断）。
- 单 token 前瞻：破行前先看下一个不可断 token 的宽度；若 token 本身比整行还宽
  （URL、长路径、无空格长串），不再把它整段挪到下一行，而是在当前行内继续填充。
- 弱断点：仅在超长 token 内部、或整行没有任何强断点时，允许在 `- _ / \ ? & = #`
  之后断开；且只有几乎不损失填充（≥ 90% 行宽）才采用，避免 URL 被切得七零八落。
- 禁则兜底：字符级回退时向前回退，避免下一行以 `。，、；：！？）》】` 开头；
  并把漏掉的起始括号 `〈《〖〘〚〝`（U+3008/300A/3016/3018/301A/301D）
  补进“禁止在行尾断开”的列表（旧表只有 「『【〔，导致出现“行尾留下《”）。
- 右侧留白：原实现为防测量漂移预留“一个字宽”的 slack；旧 CJK 断点的
  retPos 偏移又把这一格吃掉，所以旧版看起来每行都少一个字。修好偏移后
  slack 收紧为 `textsize()/3`（约 1/3 em），右端与旧版视觉宽度对齐。
- 修正原有 CJK 断点的 retPos 偏移：之前 `retPos = next(b)` 会让下一行从
  断点字符之后开始（导致收尾标点跑到行首）；现在 `retPos` 与 `newLineStart` 一致，
  同时 `retLineEnd` 按“是否跳过断行空白”计算。

**修改文件：** [src/Fl_Text_Display.cxx](src/Fl_Text_Display.cxx)。
这里描述归档算法，不将仓外 A/B 产物或历史性能数字作为 AHKDeck 的验证结果。

## Fl_Text_Display.cxx — 滚动上限不得把文末推出视口（2026-11-07, m084）

`vscroll_bottom_line()` 原本固定用 `DECK_BLANK_ROWS_PAST_END = 3` 行空白，
公式 `nbuf + 2 + blank - nvis` 在**可见行数少于 blank + 1** 时会落到文末之后：
目标行根本没有文字，其行起点是 -1，`offset_line_starts()` 由此算出负的
`mFirstChar`，此后每次查询 `position_to_line()` 都命中失败分支，FLTK 每一行
控制台输出都触发一次 `Fl::error()`（在 m084 之前是一次弹窗）。

现在空白行数随视口收缩：`blank = min(DECK_BLANK_ROWS_PAST_END, nvis - 1)`，
保证文档最后一行始终留在视口内。可见行数足够时行为与原来完全一致。

**修改文件：** `src/Fl_Text_Display.cxx`

## Fl_Text_Display.cxx — mFirstChar 必须是有效缓冲区位置（2026-11-07, m084）

`offset_line_starts()` 的四个分支都从"最近的已知行起点"出发，而**没有文字的
可见行**行起点是 -1，`skip_lines()` / `rewind_lines()` 于是也返回 -1。
负的 `mFirstChar` 会污染之后所有查询：`position_to_line()` 的一致性检查失败，
`Fl::error()` 每次追加一行就响一次；而错误信息又被写回同一个控制台，
形成"报错 → 写控制台 → 再报错"的递归（m084 的 MCP 门禁就是这样崩的）。

修正：算出负值时按滚动方向回退到文本末尾（向下）或文本开头（向上），并
**重建整张行起点表**——旧表是过期的，只做位移会留下 `mFirstChar > mLastChar`
的矛盾状态。

**修改文件：** `src/Fl_Text_Display.cxx`

## 构建核验

[4_build.bat](../../build/4_build.bat) 调用 [check_fltk_patch.ps1](../../build/check_fltk_patch.ps1)，
从补丁归档提取 `deck*` 外部钩子名并检查 `fltk.lib`；缺失时重编，重编后仍缺失则失败。
此检查只证明钩子名称存在，不替代菜单阴影、滚动及换行的运行时验证。
