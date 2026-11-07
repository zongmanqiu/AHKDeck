# 第三方组件声明（THIRD-PARTY-NOTICES）

本产品以 GPL-2.0 分发（见 [LICENSE](LICENSE)），并包含以下第三方组件。以下用相对路径指称仓库内位置。

## 1. AutoHotkey_H v2.0.25（进程内脚本引擎）

- 来源：https://github.com/thqby/AutoHotkey_H ，钉定 commit
  `fb9e48c3d326adebcfe080d1a4f9226045b51c4f`（2026-04-30，v2.0 稳定线）
- 许可：**GPL-2.0（only）**——上游 `license.txt` 为裸 GPLv2 全文，与本仓库 `LICENSE` 同文；
  GitHub license API 的 spdx 亦为 `GPL-2.0`（非 `GPL-2.0-or-later`）
- 落位：源码快照在 `.3rd\AutoHotkey_H-v2.0.25\`（只上游源码，无编译产物）
- 补丁：见 [内核补丁说明](patches/AutoHotkey_H-v2.0.25/README.md)，包含错误汇、脚本身份隔离与版本修正。
- 对本产品的影响：进程内嵌入＝衍生作品，故本产品按 GPL-2.0 发布源码。

## 2. FLTK 1.4.5（GUI 框架）

- 来源：https://github.com/fltk/fltk ，tag `release-1.4.5`
- 许可：**LGPL-2.0 ＋ FLTK 例外条款**（上游 `COPYING`，2001-12-11 版）。与我们相关的关键例外
  （原文第 3 条）：**静态链接 FLTK 不构成衍生作品**，不要求应用提供源码、不要求改用共享库。
- 落位：源码快照在 `.3rd\fltk-1.4.5\`；构建树在 `.temp\fltk-build\`（中间产物）
- 补丁：`.main\patches\fltk-1.4.5\`（6 个文件：`FL\Fl_Menu_.H`、`FL\Fl_Text_Buffer.H`、
  `src\Fl_Menu.cxx`、`src\Fl_Scrollbar.cxx`、`src\Fl_Text_Buffer.cxx`、`src\Fl_Text_Display.cxx`），
  逐条修改记录见 [FLTK 补丁说明](patches/fltk-1.4.5/FLTK_PATCHES.md)。补丁集源自 Pecia，
  阴影钩子带 `/alternatename` 空桩兜底；颜色全局仍由 AHKDeck 的 `ui/Theme.cpp` 定义，不能据此声称库可独立链接。
- 修改版 FLTK 本身的源码修改：按 LGPL 第 4 条随本仓库源码公开（即上述补丁归档）。

## 3. Microsoft Visual C++ 运行库（静态链接，无外部依赖）

宿主使用 **静态 CRT（/MT，CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded）**，避免要求用户单独安装 VC++ 运行库。
产品仍依赖 Windows 系统组件及随包的 `AutoHotkey64.dll`，不是仅依赖 USER32/KERNEL32。
链接进 exe 的第三方库（FLTK）必须用同一旗标构建（4_build 已传
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`）。

## 4. 构建期工具（不随产品分发）

CMake、MSVC、Windows SDK、curl、bsdtar 仅为构建工具，不进入产物、不参与分发。

## 5. AutoHotkey 应用图标（m030）

- 文件：`.main\image\autohotkey.ico`（由 `.3rd\AutoHotkey-v2.0.25-src\source\resources\icon_main.ico` 原样复制）
- 来源：AutoHotkey v2.0.25 源码（https://github.com/AutoHotkey/AutoHotkey ，tag `v2.0.25`）
- 许可：**GPL-2.0**——与本产品同许可，作为 `AHKDeck.exe` 的 ICON 资源
  （文件图标 / 任务栏 / 自绘标题栏 / 托盘）使用。
- 说明：图标仅作应用标识；AutoHotkey 的名称与标志相关权利归其权利人。
