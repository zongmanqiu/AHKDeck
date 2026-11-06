# AHKDeck 构建说明（build/）

编号流水线照 pecia 纪律：`full.bat` ＝ 1 体检 → 2 下载 → 3 补丁 → 4 构建 → 5 测试。
任一步失败即停；每步都可单独重跑（幂等）。

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
| `AHKDECK_SKIP_PATCH=1` | 4_build 里跳过补丁重放（full.bat 已跑过时） |
| `AHKDECK_GATE_UI=1` | 5_test 额外运行 GUI 标签测试；默认只跑非 GUI CTest 及静态检查 |
| `AHKDECK_CHAINED=1` | 抑制脚本结尾的 pause（full.bat 自动设置） |
| `AHKDECK_MSVC_ROOT` / `AHKDECK_SDK_ROOT` | 显式指定工具链路径（msvc_env.bat 三级探测的第一级） |

## 目录去向（硬规则，源自项目 AGENTS.md）

| 东西 | 去处 |
|---|---|
| 第三方源码 | `.3rd\<库名>\`（永不放编译产物） |
| 下载缓存 | `.temp\download_cache\` |
| FLTK 构建树 | `.temp\fltk-build\`（与 pecia 的"库内 build/"不同——我们要 .3rd 纯净） |
| CMake 构建树 | `.temp\cmake_build\` |
| 测试 exe | `.temp\bin\` |
| 成品 exe | `.build\AHKDeck.exe` |
| 随包脚本 | `.build\Scripts\`（CMake `deck_scripts ALL` 目标从 `.main/scripts/` 同步 `*.ahk`、`*.inc`，不依赖 exe 重链接） |

## 已知坑（每条都真实踩过，别再踩）

1. **`.bat` 必须 ASCII＋CRLF**：cmd 在 OEM 代码页解析中文注释会出错；LF 行尾偶发解析异常。
   本目录所有 .bat 由构建者保证 ASCII；改完用 `unix2dos` 或 PowerShell 转成 CRLF。
2. **`deps.txt` 纯 ASCII**：`for /f` 按 OEM 代码页逐行读，中文注释可能被吞并连坐下一行依赖。
3. **codeload 直链**：`https://codeload.github.com/<owner>/<repo>/tar.gz/<ref>`，
   别用 `github.com/.../archive/...`（受限代理下 502）。
4. **tar 用 System32 的 bsdtar**：Git Bash 的 GNU tar 可能把 Windows 盘符路径当远程主机。
   `_common.bat` 已固定从 `%SystemRoot%\System32` 解析。
5. **补丁同步必须"内容不同才复制"**：`copy /Y` 无条件刷 mtime，之后"补丁比库新"永远成立也永远
   不成立——分不清刚打过补丁还是只是被碰过。3_patch 用 `fc /b` 比较后按需复制。
6. **库补丁校验查归档不查时间戳**：`check_fltk_patch.ps1` 从补丁归档提取每个 `pecia*` 外部钩子名，
   在 `fltk.lib` 字节流里找；缺钩子强制重编，编完再验，仍缺则构建失败。
   （典故：库从打过补丁的源码编出、补丁源码被还原后重编，构建一路绿、功能静默消失。）
7. **NMake 构建目录不能混生成器**：检测 `CMakeCache.txt` 里的 generator，不一致就清目录重配置；
   复用缓存目录前确认 `build.make` 仍在（半清理目录会 U1052 死得不明不白）。
8. **`/utf-8` 全程开**：否则 MSVC 按 GBK 解析中文注释，行尾字节吞掉下一行 `#include`，
   在头文件里炸出上百条假错误。本仓库 CMake 全局 `add_compile_options(/utf-8 ...)`。
9. **`/W4 /WX` 只加自己目标**：全局加会让第三方目标（自带 /W1）每个文件打一条 D9025 覆盖提示，
   一次构建多几百行噪声。
10. **AHK_H 内核编译（engine 里程碑）**：MSBuild 编 `AutoHotkeyx.sln` 时必须
    `/p:OutDir` `/p:IntDir` 重定向到 `.temp\` 且按项目分目录（MSB8028）；
    `AutoHotkeyx` 工程的 DLL 形态走 `MT_ReleaseDll` 配置。
11. **从 PowerShell 跑这些 .bat**：用 `cmd /c 'full.bat'` 并先 `$env:AHKDECK_CHAINED='1'`，
    否则结尾 pause 会挂住自动化会话。
12. **echo 文本里禁止 `->` 和未引号的 `<`/`>`**：cmd 把 echo 行里的 `>` 当**重定向**解析——
    `echo   AHKDeck.exe -> <路径>` 会把刚链出来的真文件**截断成 23 字节的提示文字**
    （m006 首日实录：一次“成功”的构建在最后的 echo 块里把 AHKDeck.exe 和 fltk.lib 各覆盖成 23 字节，
    钩子校验在前面通过、事后才发现）。echo 展示路径一律用 `:` 分隔；必须写尖括号时加引号。
    另外：4_build 调 3_patch 时用 `AHKDECK_CHAINED_SAVE` 保存/恢复链式标志，不能直接清空——
    清空会让成功结尾的 pause 在自动化里误触发。

13. **先 lint 后执行**：写完的 .bat 必先过 `tools\lint_bats.ps1`（抓块内未转义括号、echo 里的重定向、
    尾反斜杠 cd、非 ASCII、LF 行尾——五类都是真实炸过的坑）；C++/实验命令输出一律重定向日志，
    只读尾部，不在会话里反复试错刷屏。

## 与 pecia 的差异（为什么不是整目录照抄）

- 第三方库目录叫 `.3rd\`（我们的 AGENTS 约定），不是 `.thirdparty\`；且库内不放 build 树。
- 中间产物一律 `.temp\`（项目目录约定），不像 pecia 放项目根 `temp\` 与 `build_cmake\`。
- 无 Rust FFI 环节（AHKDeck 宿主单语言 C++），4_build 直接是"FLTK + 主程序"。
- 成品只进 `.build\`，测试 exe 进 `.temp\bin\`（测试属中间产物）。
