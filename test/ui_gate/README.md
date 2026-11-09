# ui_gate — UI 冒烟（m014/m015）

这两个脚本会真的启动 `.build\AHKDeck.exe` 并操作窗口，**需要可交互桌面**（不是无头环境），
所以它们不在 ctest 默认门禁里；构建门禁（`5_test.bat`）只跑 version/textfile/session/journal/no_console。

| 脚本 | 验证 |
|---|---|
| `smoke_ui.ps1 -Case run` | 选中探针→运行：心跳增长、同进程、关宿主后心跳停止、无 `#32770` |
| `smoke_ui.ps1 -Case boom` | 抛错脚本：控制台/JSONL 有 `severity=fail`、无弹窗、宿主正常退出 |
| `smoke_editor.ps1` | 选中即载入、追加注释、Ctrl+S 落盘（BOM/CRLF 不变）、保存并运行、无弹窗 |
| `smoke_editor_find.ps1` | 行号显示、查找选中、**全部替换落盘**、清单行仍在且 deck 不动用户脚本、无弹窗（m083：改成纯键盘驱动；session B 原来按 `y` 要求"移除清单行"，产品里既没有这个键也没有这个菜单项，已改成断言真实契约；m089 起查找栏是开关且默认折叠，Ctrl+H 因此按两次＝收起再展开，删除默认键 `Delete`） |
| `smoke_ui_detail.ps1` | 双击行运行、关宿主即停（走 `--quit`）、日志按钮回执、窗口几何重启恢复、无弹窗 |
| `smoke_workbench.ps1` | **m023 工作台四件事**：`Scripts\` 目录存在且 `--run` 能加载相对 `#Include`（`.inc` 由门禁写在探针旁）、全局快捷键启/停、双击=切换、Ctrl+Shift+M 走菜单桥（坐标原样到达）、重启换会话、快捷键条保存/清除、关闭=隐藏到托盘且脚本仍在跑、`--quit` 才真退出、无弹窗；**m024**：过滤框（过滤后点唯一可见行启动的是那个脚本而不是清单第 1 行）、清空后行号恢复；**m026**：右键一律 deck 菜单（在跑的行也一样，且此时不得出现 `#32768`）、每行「菜单」格弹脚本自己的托盘菜单（含从不定制菜单的脚本的默认菜单）、红绿灯是**像素**判据（在跑＝绿、没跑＝灰） |
| `show_hotkey.ps1` | **m108 显示主窗口热键**：隔离的 `hotkeys.txt` 写入 `window.show=^!D` → 最小化的窗口按热键恢复；关闭按钮隐藏到托盘后按热键回到窗口并置前；快捷键对话框顶行是「窗口 > 显示主窗口」并有截图。**注意**：隐藏后 `show()` 会重建 HWND（FLTK 把隐藏窗口移出窗口表），所以每次判据都按 pid 重新找窗口，不能拿旧句柄 |
| `check_choice_labels.ps1` | 文本门禁：`Fl_Choice::add()` 的分隔符必须是 `\|`（用 `\n` 只会得到一个项）。带 `-SelfTest`；扫不到 `add()` 调用即判失败 |
| `check_empty_state.ps1` | **m125 空列表提示**：文本门禁，四条规则——`ScriptBrowser::draw` 必须在 `size() == 0` 时调 `DrawEmptyHint(mOwner->ScriptCount() == 0)`（`size()` 分不清"没有脚本"和"被筛掉"）、两句文案（`还没有脚本` / `没有匹配的脚本`）都在、`ScanScripts` 扫描前建 `Scripts\`。带 `-SelfTest`（好样本干净、三个坏样本各被一条规则抓住）；扫不到 `DrawEmptyHint` 即判失败 |

`smoke_workbench.ps1` 还会用产品自己的「复制」按钮把控制台读回来（`Get-DeckConsole`），
这样判断依据是 deck 说了什么，而不是我猜它做了什么；剪贴板属于用户，读之前先存、读完还原。

`Stop-Deck.ps1` 是共用的收尾通道：m023 之后关闭按钮＝隐藏到托盘，所以门禁要真退出必须调
`AHKDeck.exe --quit`，不再拿 WM_CLOSE 当"关宿主"。

用法（项目根执行）：

```powershell
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_editor.ps1
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_editor_find.ps1
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_ui_detail.ps1
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_ui.ps1 -Case run
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\smoke_workbench.ps1
powershell -STA -NoProfile -ExecutionPolicy Bypass -File .main\test\ui_gate\show_hotkey.ps1
```

要点（踩过的坑，见 m015 §5）：输入一律用 `PostMessage`（`WM_CHAR`/鼠标消息）＋ `keybd_event`，
不依赖前台焦点；**注入的 `Ctrl` 要留 250 ms 让 app 泵到消息**
（`GetKeyState(VK_CONTROL)` 读不到就是没按住，`Ctrl+F` 会变成打一个字母 "f"）。
截图默认**不做**（`-Capture` 才做）：`PrintWindow` 在忙的桌面上会一直等一个不绘图的窗口，
把门禁挂在图片上没有意义；`CopyFromScreen` 可行但要先把窗口拉到前台。
截图落点在 `.temp\ui_gate_shots`，**不写项目根**。
探针脚本临时复制到 `.build\Scripts`，结束时连同产物一起删除。
清单类用例通过 `AHKDECK_LIBRARY` 指向 `.temp`，不碰真实用户目录；
需要单行清单时，先把 `.build\Scripts\*.ahk` 临时挪到 `.temp\m016\scripts-stash`，结束后再移回。
**点击坐标会随布局变**：m037 之后列表占了 `x=0..575`、编辑区从 `x≈578` 才开始；
旧的 `(520,200)` 那一击现在打在列表栏上（会抢走焦点）。
能用键盘就别用坐标（m083 的 `smoke_editor_find.ps1` 已全部改成键盘）。
