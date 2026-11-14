# patches/AutoHotkey-v2.0.31 — 官方内核补丁归档

本目录是 AHKDeck 对工作区 `.3rd/AutoHotkey-v2.0.31-src/`（官方 AutoHotkey tag `v2.0.31`，
sha256 `7ed1c04d26b60ea2b4c645768ec9d60d6831d34d53db3d926e451165bd3f467f`）源码修改的权威归档。
上游是官方 AutoHotkey（不再是 AHK_H 分叉）；升级 = 换 tag + 重放本目录补丁，
`guard_patch.ps1` 会校验目标文件为原始版本或当前补丁版本。

所有补丁由**环境变量门控**：未设置对应变量时行为与上游逐字节一致（宿主不部署时内核就是原版解释器）。
这也是「编译为 EXE」产物等价性的基础——同一个二进制，双击就是原版 AHK。

| 归档实现 | 内容 |
|---|---|
| [source/ahkversion.h](source/ahkversion.h) | 版本回落值钉定为 2.0.31（源码树无 `.git`，`git describe` 不跑） |
| [source/error.cpp](source/error.cpp) | `AHKDECK_ERROR_FILE`：`Script::ShowError` + `Script::ScriptWarning` 把错误/警告写成一行 UTF-8 JSON，不弹框 |
| [source/AutoHotkey.cpp](source/AutoHotkey.cpp) | `AHKDECK_NO_TRAY=1`：`EarlyAppInit` 里置 `g_NoTrayIcon`，子进程启动时不装托盘图标（合并托盘由宿主接管） |
| [source/script2.cpp](source/script2.cpp) | 托盘桥：宿主发 `WM_APP+0x1DE0`（`AHKDECK_DUMP_MENU`）→ 把 `mTrayMenu`（含子菜单/勾选/禁用/默认项）序列化成 JSON 写 `AHKDECK_MENU_FILE` |
| [source/script.cpp](source/script.cpp) | **m194**：`AHKDECK_HOST_EXE=<宿主 exe 路径>` 时把 `mOurEXE`/`mOurEXEDir` 改指宿主，令 `A_AhkPath` 恢复进程内 DLL 时代的语义。不加这一条，官方脚本的「独立运行」判定会误判并 `A_IconHidden := false`，`ShowTrayIcon(true)` 重新建图标——`AHKDECK_NO_TRAY` 会被脚本自己推翻 |

## 错误 JSON 形状（m192）

```json
{"kind":"error|warn","phase":"load|runtime","file":"绝对路径","line":行号,"message":"标题","extra":"Specifically"}
```

每行一条，追加写。`CRITICAL_ERROR` 在运行期仍会 `ExitApp(EXIT_CRITICAL)`，与对话框路径一致。

## 菜单 JSON 形状（m192）

```json
{"items":[{"id":菜单ID,"text":"...","checked":b,"enabled":b,"radio":b,"default":b,"items":[...]}]}
```

`{"separator":true}` 表示分隔线。`id` 是 AHK 内部分配的菜单命令 ID，宿主点击时以
`PostMessage(g_hWnd, WM_COMMAND, id, 0)` 送回——与 AHK 自己触发默认菜单项的路径
（script2.cpp `PostMessage(hWnd, WM_COMMAND, ...->mMenuID, 0)`）完全相同，接收侧零改动。

## 为什么要两处接管错误

`ShowError` 收拢所有会弹 `IDD_ERRORBOX` 的错误与 MsgBox 模式警告；`ScriptWarning` 处理其余
警告模式（默认只发 OutputDebugString，子进程宿主永远看不见）。两处都接管后，宿主看到的
警告/错误集合与 AHK_H 错误汇时代一致。

## 构建与验证

- `build_kernel.bat`（P1 起）编 `Release|x64` → `.temp\kernel_official\AutoHotkey64.exe`；
  P4 切换后这个文件就是唯一内核与编译底座。
- 验证探针（P0，已归档在 .plan/m192）：正常脚本 rc=0 且 `A_AhkVersion=2.0.31`；运行时错误/语法错误/
  载入期警告各自落一行 JSON；警告不终止脚本。
- **m194 回归验证**：宿主变量 `AHKDECK_HOST_EXE` 传入时，`A_AhkPath` 指向 `AHKDeck.exe`、
  `A_IconHidden` 在脚本执行「独立运行」判定后仍为 1；不传时 `A_AhkPath` 指向内核 exe、
  脚本照原意把图标显示出来。两者都实测过，见 `.plan/m194`。
- 升级注意：上游若改写 `ShowError`/`ScriptWarning`/`EarlyAppInit`/`MainWindowProc`/`Script::Init` 的锚点，
  `guard_patch.ps1` 会先拦住；重放补丁后必须重跑 P0 四项 + 新旧错误对拍 + m194 的 `A_AhkPath` 探针。
