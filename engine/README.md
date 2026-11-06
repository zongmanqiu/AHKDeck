# engine/ — AHK_H 宿主封装层

职责：把钉定的内核（`.3rd/AutoHotkey_H-v2.0.25`，commit `fb9e48c3`，ReleaseDll 编译）包成宿主可用的最小 API。
**一个脚本 = 本进程内一条内核线程**：不产生第二个进程，本体关闭即全部关闭（用户要求 1a／2）。

## 现在的 API（`DeckEngine.h`）

| 面 | 说明 |
|---|---|
| `Load/Unload` | 载入 `.build\AutoHotkey64.dll`，取 `DeckNewThread`/`DeckSetErrorSink`/`DeckSetScriptIsolation`（三者缺一即判定"不是 deck 版内核"，直接报错，不降级运行）；`Unload` **不卸载内核**（见下） |
| `StartSession(path,args,&err)` | 起一个会话，返回自增 `id`；读不到文件／解析失败也照样给会话（`finished` + rc=2，同官方 `CRITICAL_ERROR`） |
| `GetSession/Sessions/StateName` | 状态快照（`running/stopping/finished`）；读取时**惰性回收**已死的线程，所以轮询 `Sessions()` 的 UI 不会读到假"运行中" |
| `WaitSession/StopSession/StopAll` | 退出码经 `GetExitCodeThread`（句柄必须带 `THREAD_QUERY_INFORMATION`，否则静默读 0）；停止＝`PostThreadMessage(WM_QUIT)`，与内核自己的 `TerminateSubThread` 同法 |
| `PopRecord(&rec,&ms)` | 取一条记录：按内核线程 id 归属到会话、给 JSON 补 `"session":<id>`、追加进该会话的 JSONL 并逐行 flush |

## 三条不可动的边界

1. **回调边界一律 `try { } catch (...)`**（线程入口/sink）：内核里是 C++ 异常，漏兜就是 fastfail `0xC0000409`，连 dump 都没有。
2. **sink 里不做事**：只加锁入队。格式化与文件 I/O 属于消费端，绝不能拖住脚本线程。
3. **致命错误（`fail`/`critical`）结束它自己的会话**，等价于官方弹框上的"Exit"。实测原因：只展开抛错那条线程时，
   定时器里的同一错误 10 秒能出 **63 条**记录且不收敛（m011 §6③）。`warn`/`fail_or_ok` 仍取官方"OK"继续跑。

## 日志落点

- `Engine::Load()` 根据宿主可执行文件位置固定日志根为程序旁的 `Logs/`，不使用进程 cwd；脚本可能改变进程工作目录。
- 对外显示/落 journal 的仍是相对 `logs\<脚本名>-<会话id>-<时间戳>.jsonl`（规则 9：不出现本机绝对路径）。
- 归属不到的记录进 `logs\_unattributed-<时间戳>.jsonl`，journal 里带 `"session":0`——不丢信息，也不猜归属。

## 不做 / 已知边界

- **不卸载内核**：`FreeLibrary` 不是安全动作（AHK_H 的 `DLL_THREAD_DETACH` 会走 `TerminateApp`），
  也不是需要的能力；进程退出时由系统回收。
- **没有 `ahkTerminate` 这个导出**（内核里不存在）；停止一律 `WM_QUIT`。
- `#Include` 里的加载错误目前归属不到会话（记录只带它自己的文件路径，未启动成功的会话没有线程 id 可对）⇒ 落 `_unattributed`。
  要精确需内核带会话令牌，重开条件见 `.plan/m011-多脚本会话.md` §7。
- 运行日志可能包含脚本原始路径与错误内容；向外部分享前应检查敏感信息。

依赖方向：`engine` 依赖 `core`；`core` 禁止 include `engine`；`ui` 经 `engine` 使用内核，不直碰 AHK_H 头。
错误可见性的内核侧补丁与验收见 `.plan/m010-错误可见性.md`；多会话见 `.plan/m011-多脚本会话.md`。
