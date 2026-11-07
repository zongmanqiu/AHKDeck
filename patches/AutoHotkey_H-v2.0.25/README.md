# patches/AutoHotkey_H-v2.0.25 — 内核补丁归档

本目录是 AHKDeck 对工作区 `.3rd/AutoHotkey_H-v2.0.25/` 源码修改的权威归档。
上游为 `thqby/AutoHotkey_H`，钉定 commit `fb9e48c3d326adebcfe080d1a4f9226045b51c4f`，
下载来源见 [deps.txt](../../build/deps.txt)。

[baseline.sha256](baseline.sha256) 记录原始文件哈希；[guard_patch.ps1](../../build/guard_patch.ps1)
在同步前校验目标文件为原始版本或当前补丁版本，拒绝覆盖未知修改。

| 归档实现 | 内容 |
|---|---|
| [source/ahkversion.h](source/ahkversion.h) | 解释器与资源版本默认值固定为 2.0.25 |
| [source/deck_sink.h](source/deck_sink.h) | 错误汇回调类型、通知函数及隔离开关声明 |
| [source/ahklib.cpp](source/ahklib.cpp) | `DeckNotifyError` 生成 UTF-8 JSON；`LibNotifyProblem` 优先接汇；导出 `DeckSetErrorSink`、`DeckSetScriptIsolation`、`DeckNewThread` |
| [source/error.cpp](source/error.cpp) | 未处理异常提取字段后接汇；`ScriptWarning` 先检查关闭警告，再通知错误汇 |
| [source/AutoHotkey.cpp](source/AutoHotkey.cpp) | 隔离模式下以真实 filespec 作为线程脚本身份，不用合成资源名覆盖 |
| [source/script.cpp](source/script.cpp) | 隔离次线程的脚本目录继承；正文加载的主脚本设置加载期工作目录 |

## 错误汇与会话

`DeckSetErrorSink` 注册进程级回调；不安装错误汇时保留上游处理路径。
未处理异常在接汇后，对 `FAIL_OR_OK` 返回继续，其余失败返回 `FAIL`；宿主消费致命记录时停止对应会话。
`DeckNewThread` 暴露带启动回调和上下文的五参数入口，让宿主在线程存活时取得线程 ID 和句柄。
`DeckSetScriptIsolation` 控制脚本文件身份与目录继承，不提供进程资源隔离。
宿主接入见 [DeckEngine.cpp](../../engine/DeckEngine.cpp)，接口与边界见 [engine/README.md](../../engine/README.md)。

## 补丁 1：`source\ahkversion.h` —— 版本串落为 v2.0.25

- **动机**：钉定的 tar.gz 源码树没有 `.git`，`AutoHotkeyx.vcxproj` 的 `SetAhkVersion`
  目标（`Condition` 要求 `.git\logs\HEAD` 存在）整体不执行，`git describe` 永远不会跑，
  `RAW_AHK_VERSION` 回退成上游 `ahkversion.h` 的 `"2.0"`。后果：
  `A_AhkVersion` 报 `2.0`、错误对话框与 `#Requires` 诊断里的解释器版本也是 `2.0`，
  与钉定的 v2.0.25 不符。
  注意：命令行 `/p:RawAhkVersion=...` **无效**——目标被 `Condition` 跳过，属性没机会落到
  `PreprocessorDefinitions`。
- **改动点**：`RAW_AHK_VERSION` 默认值 `"2.0"` → `"2.0.25"`；`RC_INVOKED` 分支
  `AHK_VERSION_N` 默认 `2,0,0,0` → `2,0,25,0`（文件属性同步）。
- **验收要求**：重建后应检查 `A_AhkVersion` 为 `2.0.25`，`#Requires AutoHotkey v2.0.25` 通过、
  `v2.0.30` 加载失败，且诊断中的解释器版本为 `2.0.25`；这里不是本次运行结果。
- **升级注意**：bump `deps.txt` 的内核版本时，必须同步改这两个默认值，否则版本串会撒谎。

## 补丁 2：`source\script.cpp` —— 正文加载的主脚本补上加载期工作目录

- **动机**：AHK_H 的进程内线程是把脚本**正文**交给加载器的（`*THREAD…?<ptr>#<len>.AHK`
  那条路），于是 `Script::OpenIncludedFile()` 在 `if (aScriptText)` 分支里直接
  `return CONDITION_TRUE`，**永远走不到**函数末尾那句 `else SetWorkingDir(mFileDir)`。
  而 `#Include name.inc` 这种相对写法，内核是用 `GetFullPathName()` 按**当前目录**解析的
  （`script.cpp` OpenIncludedFile 内），所以它解析到的是宿主进程的 cwd，不是脚本自己的目录。
  这会使相对 `#Include` 依赖宿主的当前工作目录，产生加载顺序相关的错误。
  此补丁对齐主脚本加载时的目录语义；`A_WorkingDir` 仍是进程级状态，不是每会话独立目录。
- **改动点**：`if (aScriptText)` 分支在 `++Line::sSourceFileCount` 之后、返回之前，对
  `source_file_index == 0`（即这一线程的主脚本）补一句 `SetWorkingDir(mFileDir)`，
  与文件加载路径末尾那句 `else SetWorkingDir(mFileDir)` 同形同语义。
  非首文件、`mFileDir` 为空都不触发；上游默认路径（文件加载）一行未动。
- **验证入口**：
  - [session_gate.cpp](../../test/session_gate/session_gate.cpp)：多会话、错误归属、退出码、定时器错误及脚本窗口测试；
    [gate_journal.ps1](../../test/session_gate/gate_journal.ps1)：JSONL journal 检查。
    两者由 [CMakeLists.txt](../../CMakeLists.txt) 注册到 CTest。
  - [smoke_workbench.ps1](../../test/ui_gate/smoke_workbench.ps1) 第 0/1 格检查随包
    `AHKDeckBridge.inc` 存在及 `--run` 加载相对 `#Include` 成功；这是手动 UI 冒烟入口，不由默认流水线运行。
  - [gate_deck_pair.ps1](../../test/kernel_gate/gate_deck_pair.ps1) 比较官方解释器与成品的退出码和输出。
    此入口不在 CTest 中，且依赖未随源码归档的临时语料；缺少语料时不能直接复现，不能当作默认构建门禁。
  上述是测试入口与覆盖范围，不是本次文档修改的运行结果。
- **升级注意**：这一支的两个锚点是 `if (aScriptText)` 分支里的 `++Line::sSourceFileCount;`
  与函数末尾的 `else SetWorkingDir(mFileDir);`；任一被上游改写，`guard_patch.ps1` 会先拦住
  （目标既非 pristine 也非补丁文本 → 3_patch 直接失败，内核保持未打补丁）。

## 补丁维护与构建

1. 本目录是权威归档，第三方落点不是反向复制的来源；修改时同步更新本文的动机、实现和验证入口。
2. [3_patch.bat](../../build/3_patch.bat) 通过基线检查后枚举 `source/*` 文件，按内容差异单向同步；
   当前实现不是逐文件硬编码清单，也不是递归复制整个目录。
3. 升级时更新 [deps.txt](../../build/deps.txt)、基线哈希与版本默认值，重放补丁并运行对应测试；
   成品对拍还需事先准备测试脚本要求的语料，不能仅凭版本号宣称兼容。
4. [build_kernel.bat](../../build/build_kernel.bat) 使用 MSBuild 构建 `AutoHotkeyx.sln`，
   配置为 `MT_ReleaseDll` / `x64`；通过 solution 处理静态库子工程配置映射，不直接构建单个工程。
5. 当前脚本把 `OutDir`、`IntDir` 分别重定向到 `.temp/kernel_build/bin/`、`.temp/kernel_build/obj/`，
   未按项目追加子目录，不能宣称已实现按项目隔离。DLL 复制到 `.temp/kernel/AutoHotkey64.dll`，
   再由 [4_build.bat](../../build/4_build.bat) 同步到 `.build/AutoHotkey64.dll`。
6. 上游构建仍会在源码树产生 `temp` 暂存目录；内核成功构建后脚本递归清理这些目录。
   失败分支会提前退出，可能留下暂存产物；第三方目录应保持仅含源码，不应将残留产物归档。
