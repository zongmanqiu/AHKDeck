# engine/ — AHK_H 宿主封装层

职责：把钉定的内核（`.3rd/AutoHotkey_H-v2.0.25`，commit `fb9e48c3`，ReleaseDll 编译）包成宿主可用的最小 API。
**一个脚本 = 本进程内一条内核线程**：不产生第二个进程，宿主进程退出时全部结束。实现见 [DeckEngine.cpp](DeckEngine.cpp)，接口见 [DeckEngine.h](DeckEngine.h)。

## 现在的 API（`DeckEngine.h`）

| 面 | 说明 |
|---|---|
| `Load/Unload/IsLoaded` | 默认载入宿主 exe 旁的 `AutoHotkey64.dll`，可由 `AHKDECK_KERNEL` 指定；必须具备 `DeckNewThread`、`DeckSetErrorSink`、`DeckSetScriptIsolation` 三个导出，否则报错；`DeckTrayMenu` 是可选导出（m132），缺失时托盘「运行中」退回两段弹出；正常 `Unload` 停止会话但保留内核映射 |
| `StartSession(path,args,&err)` | 起一个会话，返回自增 `id`；读不到文件／解析失败也照样给会话（`finished` + rc=2，同官方 `CRITICAL_ERROR`） |
| `GetSession/Sessions/StateName` | 状态快照（`running/stopping/finished`）；读取时**惰性回收**已死的线程，所以轮询 `Sessions()` 的 UI 不会读到假"运行中" |
| `WaitSession/StopSession/StopAll` | 通过具有 `THREAD_QUERY_INFORMATION` 权限的句柄读取退出码；显式停止发送 `WM_QUIT`，并对脚本主窗口发送有超时的 `WM_CLOSE`，处理模态循环情形 |
| `FindScriptWindow` | 按会话线程、宿主进程及 `AutoHotkey` 窗口类查找脚本主窗口；不是脚本的 `AutoHotkeyGUI` 窗口 |
| `TrayMenu(sessionId)` | 取会话线程的托盘菜单 HMENU（内核惰性建菜单）；会话未启动/已结束返回空。只读，宿主复制后不改写脚本的菜单 |
| `LogRoot` | 返回 `Load()` 固定的日志根供内部文件访问使用 |
| `PopRecord(rec, timeoutMs)` | 消费记录、归属会话、补 `"session":<id>` 并写入 JSONL，逐行 flush；参数是记录引用与超时值 |

## 三条不可动的边界

1. **错误汇与启动回调兜住异常**：`OnError`、`OnThreadStarted` 使用 `try/catch`，避免异常越过 DLL 回调边界。
2. **错误汇保持轻量**：`OnError` 解析 JSON、加锁入队并通知消费端；不在脚本线程中执行日志文件 I/O。
3. **致命错误按会话处理**：`PopRecord` 对已归属的 `fail`/`critical` 标记 fatal，并向仍在运行的会话发送
   `WM_QUIT`，避免定时器反复报同一错误。`warn`/`fail_or_ok` 不触发此停止路径；消费端必须持续取记录。

## 日志落点

- `Engine::Load()` 根据宿主可执行文件位置固定日志根为程序旁的 `Logs/`，不使用进程 cwd；脚本可能改变进程工作目录。
- 会话日志位置对外显示/落 journal 时使用相对 `Logs\<脚本名>-<会话id>-<时间戳>.jsonl`；这不等于日志内容已经脱敏。
- 归属不到的记录进 `logs\_unattributed-<时间戳>.jsonl`，journal 里带 `"session":0`——不丢信息，也不猜归属。

## 不做 / 已知边界

- **不卸载内核**：`FreeLibrary` 不是安全动作（AHK_H 的 `DLL_THREAD_DETACH` 会走 `TerminateApp`），
  也不是需要的能力；进程退出时由系统回收。
- 不依赖 `ahkTerminate`；显式停止采用上述 `WM_QUIT` 与 `WM_CLOSE` 路径，不是强制终止线程。
- `Route()` 优先按线程 ID 归属；只有一个会话时直接归属该会话，否则尝试匹配未启动会话的主脚本路径。
  多会话下，若 `#Include` 加载错误仅带被包含文件路径且无可匹配线程 ID，会落入 `_unattributed`；
  当前错误协议没有会话令牌，不能保证这类错误精确归属。
- 文件身份隔离不等于进程资源隔离：脚本共享进程工作目录等状态。
- 运行日志可能包含脚本原始路径、错误内容和敏感数据；向外分享前须检查并脱敏，不应直接发布原始日志。

依赖方向：`engine` 依赖 `core`；`ui` 经 `engine` 使用内核，不直接依赖 AHK_H 头。
构建接线见 [CMakeLists.txt](../CMakeLists.txt)。

## 实现与测试入口

- [内核补丁说明](../patches/AutoHotkey_H-v2.0.25/README.md)：错误汇、启动回调及脚本身份隔离。
- [session_gate.cpp](../test/session_gate/session_gate.cpp)：并发会话、加载失败、错误归属、退出码、定时器及脚本窗口测试。
- [gate_journal.ps1](../test/session_gate/gate_journal.ps1)：journal JSONL 验证。
- [smoke_workbench.ps1](../test/ui_gate/smoke_workbench.ps1)：手动 UI 冒烟，含随包 include 加载检查。

`session`、`journal` 由 CMake 注册到 CTest；手动 UI 冒烟不属于默认构建测试。
