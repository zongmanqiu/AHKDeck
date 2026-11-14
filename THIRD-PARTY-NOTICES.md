# 第三方组件声明（THIRD-PARTY-NOTICES）

本产品以 GPL-2.0 分发（见 [LICENSE](LICENSE)），并包含以下第三方组件。以下用相对路径指称仓库内位置。

**上游许可原文随产品分发**，放在程序旁的 `licenses\`（源目录 `licenses/`）：
`GPL-2.0.txt`（官方 AutoHotkey 的 `license.txt` 原样；AutoHotkey_H 的 `license.txt` 与它同文）、
`LGPL-2.0-with-FLTK-exception.txt`（FLTK 的 `COPYING` 原样），说明见
[licenses/README.md](licenses/README.md)。本文件是组件摘要，不替代原文。

## 1. 官方 AutoHotkey v2.0.31（唯一内核，进程外）

- 来源：https://github.com/AutoHotkey/AutoHotkey ，tag `v2.0.31`；源码包 sha256
  `7ed1c04d26b60ea2b4c645768ec9d60d6831d34d53db3d926e451165bd3f467f`
- 许可：**GPL-2.0（only）**——上游 `license.txt` 为裸 GPLv2 全文，与本仓库 `LICENSE`、
  随包 `licenses\GPL-2.0.txt` 逐字节相同（sha256
  `2f37ec8a6e912402a7d79ea03e5e33eacf54d1bf1fc7e3b0eab3a69bd8b23252`）。
- 落位：源码快照在 `.3rd\AutoHotkey-v2.0.31-src\`（只上游源码，无编译产物）；补丁归档在
  `.main\patches\AutoHotkey-v2.0.31\`；构建产物在 `.temp\kernel_official\AutoHotkey64.exe`，
  再复制到程序旁 `AutoHotkey64.exe`。
- 补丁：四处由环境变量门控的修改（版本回落、JSON 错误/警告汇、`NO_TRAY` ＋托盘菜单快照、
  启动钩子），未设置 `AHKDECK_*` 环境变量时与上游行为一致。理由与验证见
  [补丁说明](patches/AutoHotkey-v2.0.31/README.md)。
- 对本产品的影响：宿主**按会话启动该 EXE 为独立子进程**，不 LoadLibrary、不在进程内嵌入；
  双方通过环境变量、JSON 文件、stdout 管道与 Win32 消息通信。本产品源码按 GPL-2.0 公开。
- m192 P4 起**一个 PE 两用**：这同一个 `AutoHotkey64.exe` 既是编辑器运行脚本的解释器，也是
  「脚本/编译为 EXE」复制并写入脚本资源的底座。旧的进程内 `AutoHotkey64.dll`（AutoHotkey_H
  v2.0.25）与 `tools\AutoHotkey64.exe` 编译底座已退场；其源码与补丁仅作历史归档，不再构建或分发。

## 2. FLTK 1.4.5（GUI 框架）

- 来源：https://github.com/fltk/fltk ，tag `release-1.4.5`
- 许可：**LGPL-2.0 ＋ FLTK 例外条款**（上游 `COPYING`，2001-12-11 版）。与我们相关的关键例外
  （原文第 3 条）：**静态链接 FLTK 不构成衍生作品**，不要求应用提供源码、不要求改用共享库。
- 落位：源码快照在 `.3rd\fltk-1.4.5\`；构建树在 `.temp\fltk-build\`（中间产物）
- 补丁：`.main\patches\fltk-1.4.5\`（6 个文件：`FL\Fl_Menu_.H`、`FL\Fl_Text_Buffer.H`、
  `src\Fl_Menu.cxx`、`src\Fl_Scrollbar.cxx`、`src\Fl_Text_Buffer.cxx`、`src\Fl_Text_Display.cxx`），
  逐条修改记录见 [FLTK 补丁说明](patches/fltk-1.4.5/FLTK_PATCHES.md)。补丁集为本项目自研维护，随源码分发，
  阴影钩子带 `/alternatename` 空桩兜底；颜色全局仍由 AHKDeck 的 `ui/Theme.cpp` 定义，不能据此声称库可独立链接。
- 修改版 FLTK 本身的源码修改：按 LGPL 第 4 条随本仓库源码公开（即上述补丁归档）。

## 3. Microsoft Visual C++ 运行库（静态链接，无外部依赖）

宿主使用 **静态 CRT（/MT，CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded）**，避免要求用户单独安装 VC++ 运行库。
产品仍依赖 Windows 系统组件及随包的 `AutoHotkey64.exe`，不是仅依赖 USER32/KERNEL32。
链接进 exe 的第三方库（FLTK）必须用同一旗标构建（4_build 已传
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`）。

## 4. 单内核布局（m192 P4，替代旧「编译底座 `tools\AutoHotkey64.exe`」一节）

- 文件：程序旁唯一一个 `AutoHotkey64.exe`，即第 1 节的内核；包内不再有第二个 AHK PE。
- 用途一：`engine\DeckEngine` 每个会话 spawn 一次，`AHKDECK_NO_TRAY` 收起子进程的托盘图标，
  `AHKDECK_HOST_EXE` 让 `A_AhkPath` 仍指向宿主（否则脚本会误判为独立运行、自己把图标显示回来，m194），
  菜单快照与 `WM_COMMAND` 回传让宿主的合并托盘菜单保持 m132 行为。
- 用途二：`core\ExeCompiler` 复制该 EXE 并替换 RCDATA id 1 写入脚本（图标、版本资源同样自行写入），
  产物与官方解释器同源；打包不依赖任何外部编译器程序。
- 不变量：`ctest single_kernel` 断言包内除 `AHKDeck.exe` 外恰好一个 PE、无 DLL、无 `tools\`，
  且与 `.temp\kernel_official` 逐字节相同；`single_kernel_selftest` 用污染样本反证该门禁会变红。
- **构建期工具**（口径不变）：CMake、MSVC、Windows SDK、curl、bsdtar 仅为构建工具，
  不进入产物、不参与分发。

## 5. AutoHotkey 应用图标（m030）

- 文件：`.main\image\autohotkey.ico`（由 `.3rd\AutoHotkey-v2.0.25-src\source\resources\icon_main.ico` 原样复制）
- 来源：AutoHotkey v2.0.25 源码（https://github.com/AutoHotkey/AutoHotkey ，tag `v2.0.25`）
- 许可：**GPL-2.0**——与本产品同许可，作为 `AHKDeck.exe` 的 ICON 资源
  （文件图标 / 任务栏 / 自绘标题栏 / 托盘）使用。
- 说明：图标仅作应用标识；AutoHotkey 的名称与标志相关权利归其权利人。
