# patches/AutoHotkey_H-v2.0.25 — 内核补丁归档

对 `.3rd\AutoHotkey_H-v2.0.25\`（上游 commit `fb9e48c3`）源码的修改唯一权威归档。

**当前落盘的上游改动**（`baseline.sha256` 逐文件钉住 pristine 哈希，`build\guard_patch.ps1`
在同步前断言目标树"要么是原始文本、要么已带我们的补丁"）：

| 文件 | 内容 | 记录在哪 |
|---|---|---|
| `source\ahkversion.h` | 版本串落为 v2.0.25（下面 §补丁 1） | 本文 §补丁 1 |
| `source\deck_sink.h`（新增文件） | 错误汇的 C ABI 声明 + 隔离开关声明 | `.plan\m010-错误可见性.md` |
| `source\ahklib.cpp` | `DeckNotifyError` 实现 + 三个导出 `DeckSetErrorSink` / `DeckSetScriptIsolation` / `DeckNewThread` | `.plan\m010`、`.plan\m011` |
| `source\error.cpp` | `LibNotifyProblem` / `UnhandledException` 接汇；`ScriptWarning` 顺序修正 | `.plan\m010` |
| `source\AutoHotkey.cpp` | 线程主入口的文件身份隔离（真实 filespec 取代合成 `*THREAD…` 资源名） | `.plan\m010` |
| `source\script.cpp` | ① `Script::Init` 的"次线程继承首脚本目录"分支隔离；② 正文加载的主脚本也要把加载期 cwd 设到脚本目录（下面 §补丁 2） | `.plan\m010`、本文 §补丁 2 |

## 补丁 1：`source\ahkversion.h` —— 版本串落为 v2.0.25

- **动机**：钉定的 tar.gz 源码树没有 `.git`，`AutoHotkeyx.vcxproj` 的 `SetAhkVersion`
  目标（`Condition` 要求 `.git\logs\HEAD` 存在）整体不执行，`git describe` 永远不会跑，
  `RAW_AHK_VERSION` 回退成上游 `ahkversion.h` 的 `"2.0"`。后果：
  `A_AhkVersion` 报 `2.0`、错误对话框与 `#Requires` 诊断里的解释器版本也是 `2.0`，
  与钉定的 v2.0.25 不符（m004 对拍 17 例的唯一 DIFF）。
  注意：命令行 `/p:RawAhkVersion=...` **无效**——目标被 `Condition` 跳过，属性没机会落到
  `PreprocessorDefinitions`。
- **改动点**：`RAW_AHK_VERSION` 默认值 `"2.0"` → `"2.0.25"`；`RC_INVOKED` 分支
  `AHK_VERSION_N` 默认 `2,0,0,0` → `2,0,25,0`（文件属性同步）。
- **验证**：重建后 `A_AhkVersion` == `2.0.25`；`#Requires AutoHotkey v2.0.25` 通过、
  `v2.0.30` 仍按预期加载失败（错误文本里显示 `Current interpreter: ... v2.0.25`）。
- **升级注意**：bump `deps.txt` 的内核版本时，必须同步改这两个默认值，否则版本串会撒谎。

## 补丁 2：`source\script.cpp` —— 正文加载的主脚本补上加载期工作目录

- **动机**：AHK_H 的进程内线程是把脚本**正文**交给加载器的（`*THREAD…?<ptr>#<len>.AHK`
  那条路），于是 `Script::OpenIncludedFile()` 在 `if (aScriptText)` 分支里直接
  `return CONDITION_TRUE`，**永远走不到**函数末尾那句 `else SetWorkingDir(mFileDir)`。
  而 `#Include name.inc` 这种相对写法，内核是用 `GetFullPathName()` 按**当前目录**解析的
  （`script.cpp` OpenIncludedFile 内），所以它解析到的是宿主进程的 cwd，不是脚本自己的目录。
  后果：用户的 `鼠标左键双击最小间隔.ahk` 里那句 `#Include AHKDeckBridge.inc`
  在 deck 里加载失败（`fail load … #Include file "AHKDeckBridge.inc" cannot be opened.`），
  在官方 `AutoHotkey64.exe` 里却正常——"跑起来和官方没区别"这条要求正是在这里被打破。
  之前没暴露，是因为 m010/m011 的语料没有相对 `#Include`；而 m011 之后"脚本一启动就把进程
  cwd 改成自己的目录"（`A_WorkingDir` 是进程级）让这件事变成了一种**顺序依赖**：
  只要之前跑过别的脚本，它就"看起来能用"。
- **改动点**：`if (aScriptText)` 分支在 `++Line::sSourceFileCount` 之后、返回之前，对
  `source_file_index == 0`（即这一线程的主脚本）补一句 `SetWorkingDir(mFileDir)`，
  与文件加载路径末尾那句 `else SetWorkingDir(mFileDir)` 同形同语义。
  非首文件、`mFileDir` 为空都不触发；上游默认路径（文件加载）一行未动。
- **验证**：
  - `build\full.bat` 10/10 绿（含 `session` 门禁 F1–F12：两个并发会话各自的主窗口、
    坐标原样到达、邻居收不到、结束后取不到窗口；把类名改成 `AutoHotkeyGUI` 后
    F2/F3/F4/F5/F6/F7/F9/F10 全红，改回后复绿——这 8 格是活的）。
  - `test\ui_gate\smoke_workbench.ps1` 第 0/1 格：`.build\Scripts` 里随包的
    `AHKDeckBridge.inc` 存在，且 `AHKDeck.exe --run` 一个 `#Include` 它的脚本 rc=0。
  - 端点重读：`test\kernel_gate\gate_deck_pair.ps1`（本轮新增）把 m004 的 26 例语料同时喂给
    官方 2.0.25 与 `.build\AHKDeck.exe --run`，逐字节比 rc 与 `out.txt`；只把
    "引擎自己的路径"（`A_AhkExe`、各自工作目录）归一化成 `<EXE>`/`<WD>`，
    `A_AhkVersion` 等一律严格比 → **26 PASS / 0 DIFF / 0 弹窗**。
    这条门禁第一次运行就是红的（2 例 DIFF_OUT），红的原因正是它该抓的东西：路径身份。
- **升级注意**：这一支的两个锚点是 `if (aScriptText)` 分支里的 `++Line::sSourceFileCount;`
  与函数末尾的 `else SetWorkingDir(mFileDir);`；任一被上游改写，`guard_patch.ps1` 会先拦住
  （目标既非 pristine 也非补丁文本 → 3_patch 直接失败，内核保持未打补丁）。

## 纪律（与 fltk-1.4.5 同一套）

1. **只增不减**：补丁文件一旦进本目录即为权威版本，历史不删。
2. **单向同步**：`build\3_patch.bat` 只做 patches → .3rd 覆盖，**禁止反向复制**
   （.3rd 是 pristine 上游 + 补丁的落点，不是权威源）。
3. **逐条记录**：每改一个上游文件，在本文件追加一节——动机/改动点/验证方式
   （格式照 `..\fltk-1.4.5\FLTK_PATCHES.md`）。
4. **显式清单**：`3_patch.bat` 同步补丁用显式文件列表，不做整树盲拷。
5. **升级流程**（m005 §2）：官方稳定线出新版 → 看 AHK_H v2.0 线是否 merge →
   有则 bump `build\deps.txt` 的 commit 并重放补丁 → 跑 m004 对拍门禁。

## 编译注意（engine 里程碑执行时）

- `AutoHotkeyx.sln` 只有 `AutoHotkeyx`（exe；DLL 能力走 `MT_ReleaseDll` 配置）＋
  `lib_pcre` ＋ `lib_double` 三个工程，**没有独立 AutoHotkeyLib 工程**（sln 逐行核对过）。
- 用 MSBuild 编内核时：`/p:OutDir` 与 `/p:IntDir` 重定向到 `.temp\`，且**按项目分目录**，
  否则 MSB8028（多工程共享中间目录）。
- 仍有 3 处绕不过去的落地位置：asm 的 CustomBuild 往 `$(ProjectDir)temp\<platform>\` 写
  `x64call.obj` / `x64stub.obj`，链接/清单阶段留 `$(ProjectDir)temp\AutoHotkey.exe.manifest`，
  两个静态库子工程还会写 `source\lib_pcre\temp\`、`source\lib_double\temp\`。
  **每次内核构建后都要清掉这 4 个位置**（`.3rd` 只放源码）。
- DLL 配置走 `AutoHotkeyx.sln`：`lib_pcre` / `lib_double` 没有 `*_ReleaseDll` 配置，
  SLN 里把它们的 `ReleaseDll` 映射到 `Release`；直接编 `.vcxproj` 会报 MSB8013。
- `.3rd\AutoHotkey_H-v2.0.25\` 内不得出现任何编译产物。
