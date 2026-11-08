# AHKDeck 构建说明（build/）

[full.bat](full.bat) 依次执行 1 体检 → 2 下载 → 3 补丁 → 4 构建 → 5 测试，任一步返回失败即停。
各步可单独重跑；`4_build.bat` 包含 FLTK、AHK_H 内核 DLL 和 AHKDeck 主程序构建。
路径变量集中在 [_common.bat](_common.bat)，依赖来源与版本固定在 [deps.txt](deps.txt)。
下文 `.main/`、`.3rd/`、`.temp/`、`.build/` 均为工作区相对路径，不是本机固定位置。

## 快速上手

```bat
cd .main\build
full.bat                 :: 一键全链路
1_check_env.bat          :: 只读体检（新机器先跑这个，看缺什么）
4_build.bat              :: 只重编译（不重下载）
5_test.bat               :: 只跑测试
```

有用的开关（环境变量）：

| 开关 | 作用 |
|---|---|
| `AHKDECK_FORCE_DOWNLOAD=1` | 无视缓存与在位检查，全部重下重解压 |
| `AHKDECK_FORCE_FLTK=1` | 清掉 `.temp\fltk-build` 重编 FLTK |
| `AHKDECK_FORCE_KERNEL=1` | 强制重建内核 DLL |
| `AHKDECK_SKIP_PATCH=1` | 4_build 里跳过补丁重放；full.bat 本身不自动设置此变量 |
| `AHKDECK_GATE_UI=1` | 5_test 额外运行 GUI 标签测试；默认只跑非 GUI CTest 及静态检查 |
| `AHKDECK_CHAINED=1` | 抑制脚本结尾的 pause（full.bat 自动设置） |
| `AHKDECK_MSVC_ROOT` / `AHKDECK_SDK_ROOT` | 显式指定工具链路径（msvc_env.bat 三级探测的第一级） |

## 目录契约

| 东西 | 去处 |
|---|---|
| 第三方源码 | `.3rd\<库名>\`（永不放编译产物） |
| 下载缓存 | `.temp\download_cache\` |
| FLTK 构建树 | `.temp\fltk-build\` |
| 内核构建树及 DLL 暂存 | `.temp\kernel_build\`、`.temp\kernel\` |
| CMake 构建树 | `.temp\cmake_build\` |
| 测试 exe | `.temp\bin\` |
| 成品 exe 与内核 | `.build\AHKDeck.exe`、`.build\AutoHotkey64.dll` |
| 随包脚本 | `.build\Scripts\`（CMake `deck_scripts ALL` 目标从 `.main/scripts/` 同步 `*.ahk`、`*.inc`，不依赖 exe 重链接） |

## 构建约束

1. **脚本编码**：`.bat` 使用 ASCII＋CRLF，`deps.txt` 使用 ASCII，避免 cmd 的 OEM 代码页解析问题。
   修改批处理后使用 [tools/lint_bats.ps1](tools/lint_bats.ps1) 检查编码、行尾、括号和重定向等问题。
2. **下载与解包**：GitHub 源码归档使用 `codeload.github.com` 直链，具体 URL 以
   [deps.txt](deps.txt) 为准；tar 使用 `_common.bat` 从 `%SystemRoot%\System32` 解析的版本。
3. **单向补丁同步**：[3_patch.bat](3_patch.bat) 以 `fc /b` 比较后按需复制，避免无条件刷新时间戳。
   内核文件先过 [guard_patch.ps1](guard_patch.ps1) 基线校验；补丁归档是权威来源。
4. **FLTK 钩子核验**：[check_fltk_patch.ps1](check_fltk_patch.ps1) 从补丁归档提取实际 `deck*`
   外部钩子名，在 `fltk.lib` 中检查；缺失强制重编，编完仍缺失则失败。
   这不是完整行为测试，也不应将这些实际符号盲改为其他前缀。
5. **NMake 构建树**：[4_build.bat](4_build.bat) 检查缓存生成器，避免混用生成器。
   复用缓存前仍应确认生成规则完整；当前脚本没有检查 `build.make` 完整性的门禁。
6. **宿主编译选项**：[CMakeLists.txt](../CMakeLists.txt) 使用 C++17、静态 CRT，并对 MSVC 开启
   `/utf-8`；`/W4 /WX` 按自有目标设置，不全局施加给第三方目标。FLTK 配置同时传入
   `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` 和 `FLTK_MSVC_RUNTIME_DLL=OFF`。
7. **内核 DLL**：[build_kernel.bat](build_kernel.bat) 构建 `AutoHotkeyx.sln` 的 `MT_ReleaseDll` / `x64`。
   `OutDir`、`IntDir` 分别指向 `.temp/kernel_build/bin/`、`.temp/kernel_build/obj/`，当前未按项目追加子目录。
   成功构建后递归清理第三方树中的 `temp` 暂存目录；失败会提前退出，可能需要清理残留。
   详见 [内核补丁说明](../patches/AutoHotkey_H-v2.0.25/README.md)。
8. **自动化调用**：从 PowerShell 用 `cmd /c 'full.bat'`；单步调用先设置
   `$env:AHKDECK_CHAINED='1'` 避免 pause。`full.bat` 自行设置链式标志，子调用应保存并恢复它。
9. **批处理重定向**：echo 文本中的未转义 `<`、`>` 会被 cmd 当成重定向，展示路径使用 `:` 分隔，
   不使用裸 `->`；必须输出尖括号时正确引用或转义，避免覆盖产物。

## 测试范围

[5_test.bat](5_test.bat) 先检查 AHK_H 专有名称、控制台归属、下拉项分隔及窗口装饰约束，
再运行 `ctest --output-on-failure -LE gui`。设置 `AHKDECK_GATE_UI=1` 后追加 GUI 标签测试。
测试注册以 [CMakeLists.txt](../CMakeLists.txt) 为准；Python 不可用时 MCP 测试不会注册。
默认流水线并不运行所有 `test/` 脚本；手动 UI 冒烟和成品对拍需单独调用并满足各自前置条件。
