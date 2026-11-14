# engine/ — 进程外内核宿主层（m192）

职责：把**唯一一个**官方 AutoHotkey v2.0.31 内核 EXE
（`.3rd/AutoHotkey-v2.0.31-src` ＋ `.main/patches/AutoHotkey-v2.0.31`，`Release|x64` 编到
`.temp/kernel_official/AutoHotkey64.exe`）包成宿主可用的最小 API。
**一个脚本 = 一个独立子进程**：宿主不再加载任何 DLL，也不在自身进程里跑 AHK 线程。
实现见 [DeckEngine.cpp](DeckEngine.cpp)，接口见 [DeckEngine.h](DeckEngine.h)。

> m192 之前这里是 AHK_H v2.0.25 的 `AutoHotkey64.dll` 进程内封装（每会话一条内核线程）。
> 用户拍板「一个内核、跟踪官方最新版」后整层重写，公共 API 的 16 个方法签名保持零变化。
> **合并托盘菜单是硬需求**，不许因为进程拆分而退回「每个脚本一个托盘图标」——见下方托盘桥。

## 现在的 API（`DeckEngine.h`）

| 面 | 说明 |
|---|---|
| `Load/Unload/IsLoaded` | `Load` 只解析内核 EXE 路径（`AHKDECK_KERNEL` 优先，否则宿主 exe 旁的 `AutoHotkey64.exe`）并固定日志根；不再 `LoadLibrary`，没有导出符号要求。`Unload` 先 WM_CLOSE，再按 Job Object 兜底终止残留子进程 |
| `StartSession(path,args,&err)` | 起一个会话（＝起一个子进程），返回自增 `id`；读不到文件／解析失败也照样给会话（`finished` + rc=2） |
| `GetSession/Sessions/StateName` | 状态快照（`running/stopping/finished`）；读取时惰性回收已退出的子进程，UI 不会读到假「运行中」 |
| `WaitSession/StopSession/StopAll` | 停止＝向子进程主窗口发有超时的 `WM_CLOSE`（走内核自己的 OnExit），无窗口/挂死时 `TerminateJobObject` 兜底；退出码由进程句柄读取 |
| `FindScriptWindow` | 按**子进程 PID** ＋ `AutoHotkey` 窗口类精确定位主窗口；PID 复用与本机另一个 AHK 都不会误投 |
| `TrayMenu(sessionId)` | 请求子进程把托盘菜单序列化成 JSON 快照，宿主据此重建一个**宿主自有**的 HMENU；每次请求销毁旧菜单，会话结束时释放 |
| `LogRoot` | 返回 `Load()` 固定的日志根 |
| `PopRecord(rec, timeoutMs)` | 消费记录、归属会话、补 `"session":<id>` 并写入 JSONL，逐行 flush |

## 进程间传输（旧 H 导出 → 新通道）

| 传输 | 旧（进程内 H） | 新（官方 2.0.31 子进程） |
|---|---|---|
| 错误/警告 | `DeckSetErrorSink` 回调 JSON | 内核补丁写 per-session raw JSONL（`temp\work\*-errors.jsonl`），引擎 `PumpRawFiles` 增量抽取；字段含 `ts/thread/seq/severity/func/source/stack`，`thread` 现为**子进程 pid** |
| `FileAppend "*"` | `DeckSetOutputSink` 回调 | 子进程 stdout 管道 ＋ 每会话阻塞读线程，包成 `kind=stdout` 记录 |
| 托盘菜单 | `DeckTrayMenu` 导出 HMENU | `WM_APP+0x1DE0` 请求 → 子进程写 `{"items":[...]}` 快照 → 宿主重建 HMENU；菜单项点击由宿主 `PostMessage(WM_COMMAND, id)` 回传，走内核原生分发路径 |
| 停止 | `PostThreadMessage(WM_QUIT)` + WM_CLOSE | WM_CLOSE ＋ Job Object（`KILL_ON_JOB_CLOSE`）——宿主退出时所有子进程一并结束，m012 挂死类问题从结构上消失 |
| 校验 | 进程内 `Main("/validate")` | 子进程 `kernel /validate` ＋ `AHKDECK_ERROR_FILE` 取 JSON |

## 三条不可动的边界

1. **子进程隔离是结构性的**：每个会话一个进程 ＋ Job Object，脚本崩溃/`ExitProcess` 不再拖死宿主；
   宿主异常退出时系统回收全部子进程。
2. **错误汇保持轻量**：`PumpRawFiles` 只做增量抽取、加锁入队，不在 UI 线程做文件 I/O；
   E4 防风暴规则保留（`severity` 为 `fail`/`critical` → `StopSession`），`warn`/`fail_or_ok` 不触发停止。
3. **托盘桥不许省**：`AHKDECK_NO_TRAY=1` 只收起子进程的托盘图标；菜单快照与 `WM_COMMAND` 回传让
   宿主的「运行中」合并菜单保持 m132 行为。**m194**：还必须传 `AHKDECK_HOST_EXE`，
   否则官方脚本按 `A_AhkPath` 误判为独立运行，一句 `A_IconHidden := false` 就会把
   `g_NoTrayIcon` 改回 false 并重新建图标（用户实测的回归）。子进程忙
   （`SendMessageTimeout` 500ms）时本轮快照为空，下次展开菜单重试，UI 退回两段弹出而不是报错。

## 日志落点

- `Engine::Load()` 根据宿主可执行文件位置固定日志根为程序旁的 `temp/logs/`（m177，经 `core/AppPaths`），
  不使用进程 cwd；子进程继承 cwd，但宿主侧路径全是绝对路径。
- 会话日志对外显示/落 journal 时用相对 `temp/logs/<脚本名>-<会话id>-<时间戳>.jsonl`；这不等于日志已脱敏。
- 归属不到的记录进 `logs\_unattributed-<时间戳>.jsonl`，journal 里带 `"session":0`——不丢信息，也不猜归属。

## 不做 / 已知边界

- **不再有内核卸载问题**：没有 `FreeLibrary`、没有 `DLL_THREAD_DETACH`，进程退出由系统与 Job Object 收尾。
- 子进程共享宿主传下去的环境变量，但**不共享** AHK 全局状态；文件身份隔离由进程边界天然满足。
- 菜单快照要求子进程先 `UserMenu::CreateHandle()`（官方菜单是惰性创建，NO_TRAY 下永远不会自己弹出）——
  补丁 4 已在 dump 前强制创建，否则序列化出空数组。
- 子进程窗口消息用 `SendMessageTimeout`/`PostMessage`，绝不在菜单 dispatch 里同步等待子进程。
- `CreateProcessW` 传 Unicode 环境块必须带 `CREATE_UNICODE_ENVIRONMENT`，且 env 必须按名字排序；
  否则报 `ERROR_INVALID_PARAMETER(87)`。
- 运行日志可能包含脚本原始路径、错误内容和敏感数据；向外分享前须检查并脱敏。

依赖方向：`engine` 依赖 `core`；`ui` 经 `engine` 使用内核，不直接依赖 AHK 头。
构建接线见 [CMakeLists.txt](../CMakeLists.txt)。

## 实现与测试入口

- [官方内核补丁说明](../patches/AutoHotkey-v2.0.31/README.md)：五处 env 门控补丁与验证入口。
- [session_gate.cpp](../test/session_gate/session_gate.cpp)：并发会话、加载失败、错误归属、退出码、
  定时器、脚本窗口与托盘菜单（F/G 组）测试。
- [gate_journal.ps1](../test/session_gate/gate_journal.ps1)：journal JSONL 验证。
- [gate_single_kernel.ps1](../test/shipping_gate/gate_single_kernel.ps1)：出货布局「只有一个 AHK PE」门禁。
- [smoke_workbench.ps1](../test/ui_gate/smoke_workbench.ps1)：手动 UI 冒烟（含合并托盘菜单，需真机手测）。

`session`、`journal`、`single_kernel` 由 CMake 注册到 CTest；手动 UI 冒烟不属于默认构建测试。
