# AHKDeck 更新历史（HISTORY.md）

> **增量原则**：每个版本只在文件**顶部**追加新版本小节，旧内容一律保持原样、不修改不删除。
> 每节记：版本号、日期，以及新增 / 修复 / 变更 / 门禁要点。发布说明直接取对应小节。

---

## 0.2.1（2026-11-10）

**变更**

- 不再随包示例脚本与菜单桥 `AHKDeckBridge.inc`：`Scripts\` 随包为空，脚本改由独立仓库
  （`ahk-v2-scripts`）维护。`CMakeLists.txt` 的 `deck_scripts` 目标从"复制脚本"改为
  "无条件创建 `.build\Scripts`"，发行包白名单不变。
- 脚本右键的齿轮格改走 AHK 原生的 `AHK_NOTIFYICON`（`0x0404` + `WM_RBUTTONUP`）：脚本不需要
  `OnMessage`、不需要带任何 include 文件，就能弹出自己的托盘菜单；旧的 `0x8001` 桥接协议降级为
  负向控制（门禁断言它不再被投递）。
- 打包不再复制 `.build\Scripts`，包内 `Scripts\` 由 `6_package.ps1` 现建为空目录：原来整个目录
  照搬，开发机上正在试的脚本会跟着发到每个用户那里。

**新增**

- 脚本列表为空时直接说清楚：`还没有脚本` / `把 .ahk 文件放进 Scripts 文件夹`；筛选后没有匹配
  项时另显示 `没有匹配的脚本`（`ScriptBrowser::DrawEmptyHint`，正文色 `text2`）。
- `Scripts\` 缺失时在扫描前自动重建（`ScanScripts`）。解压工具丢掉空目录也不会让列表永远为空，
  `文件 > 打开脚本文件夹` 不再无声失败。

**门禁**

- `smoke_workbench.ps1`：第 0 格改为断言 `Scripts\` 目录存在；第 1 格用门禁自写的
  `m023-inc.inc` 验证相对 `#Include` 仍从脚本所在目录解析；托盘探针不再 `#Include` 桥接文件。

## 0.2.0（2026-11-08）

**新增**

- **MCP 后台接入**：`--mcp-stdio` 启动后主窗口直接隐入托盘（托盘不可用时保持可见），
  AI 高频调用不再把窗口弹到用户面前；stdin 断开即退出，不留孤儿进程。托盘图标或
  已绑定的「窗口 > 显示主窗口」热键随时唤回。
- **MCP 工具面成型**：`list_scripts` / `read_script` / `edit_editor` / `check_editor` /
  `validate_editor` / `patch_editor` / `run_editor` / `run_script` / `get_session_output` /
  `stop_script`。写入回执（`changed`、首末改动行）、有界输出（`max_bytes` / `truncated`）、
  只读静态检查、唯一命中才生效的小补丁、`--validate` 语法预检、脚本传参都在这一版落地。
- **工作台布局可调**：脚本列表常驻；列表↔编辑器、编辑器↔控制台各有一条 6px 分隔条可拖拽，
  占比记入 `settings.json` 的 `ui.list_pct` / `ui.console_pct`。
- **状态栏资源读数**：CPU、内存、GPU 占用随时可见。
- **启动清理**：启动时收拾上次遗留的临时脚本与过期缓存。
- **快捷键体系**：全局快捷键（`窗口 > 显示主窗口`、`开关 > 脚本名`）名称常红，列表顶部
  有一行红字「红色为全局快捷键」；全局允许裸 `F1`–`F24`，菜单快捷键允许裸功能键与 `Del`；
  默认键只保留与 Windows 记事本重合的 13 条（新建/保存/撤销/重做/剪切/复制/粘贴/删除/
  全选/查找/查找下一个/查找上一个/替换）。

**修复**

- 点击菜单栏标题即崩溃退出：空回调进了消息循环。
- 标题栏、分隔条悬停后残留高亮；分屏顶部边框出现缺口。
- 托盘唤回后窗口框架退化，标题栏与边框不再自绘。
- 快捷键对话框字体粗细不一：行控件重绘未清背景，透明抗锯齿文字反复叠画。

**变更**

- 随包脚本精简为一个示例（`鼠标左键双击最小间隔.ahk`）加菜单桥 `AHKDeckBridge.inc`。
- 发行包改由助手用 **7z** 打 zip，白名单固定在 [build/打包清单.md](build/打包清单.md)：
  `AHKDeck.exe`、`AutoHotkey64.dll`、`Scripts\`、`docs\`、`licenses\`、`LICENSE`、
  `THIRD-PARTY-NOTICES.md`；`settings.json`、`Logs\`、`hotkeys.txt`、`update.cache.json`
  等运行期文件不入包。
- `LICENSE` 与 `THIRD-PARTY-NOTICES.md` 随构建落到安装目录，上游许可原件（GPL-2.0 /
  LGPL-2.0-with-FLTK-exception）随包分发。
- 版本号唯一来源为 `CMakeLists.txt` 的 `project(AHKDeck VERSION ...)`，README 与
  `update.json` 随之同步。

**门禁**

- ctest 16/16；启动清理、资源统计、源码树、无终端等门禁随功能一起落地。

---

## 0.1.0（2026-11-07）

首个对外测试版：脚本工作台（脚本列表、编辑器、查找替换、控制台、托盘与全局热键）、
随包示例脚本与菜单桥，以及完整的自动更新链路。

**新增**

- 「帮助 > 检查更新」：Gitee / GitHub 双源回退，联网失败读本地缓存，WinHTTP 下载后由
  升级脚本覆盖安装；「关于」从「设置」移到「帮助」。
- 快捷键唯一契约：一个组合键只属于一个命令或一个脚本，录入新键会把原持有者置空，
  保存时再统一去重。

**修复**

- 视图菜单未勾选框由 2px 双线改为 1px 细线，与其余复选框同源。

**变更**

- 「关于」对话框整列自绘，仓库入口拆成 Gitee / GitHub 两行。
- 快捷键设置窗口只高亮捕获组合键的单元格，不再整行变色。

**门禁**

- ctest 11/11。
