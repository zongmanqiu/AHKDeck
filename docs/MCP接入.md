# AHKDeck 的 MCP 接入

AHKDeck 自带一个 **MCP stdio 端点**。挂到支持 MCP 的 AI 客户端（Trae、Qoder、WorkBuddy、
Claude Desktop、Cursor…）之后，AI 可以：

- 列出脚本库、读取任意脚本；
- 往编辑区写脚本（不落盘，等人检查）；
- 运行编辑区 / 运行已有脚本，**拿到内核原话的报错（带行号）**；
- 读会话输出、停止常驻脚本。

于是「AI 写 → 跑 → 读错 → 改 → 再跑」这条闭环不需要人复制粘贴。

## 一、两个字段

```json
{
  "mcpServers": {
    "ahkdeck": {
      "command": "<AHKDeck 安装目录>\\AHKDeck.exe",
      "args": ["--mcp-stdio"]
    }
  }
}
```

只把 `<AHKDeck 安装目录>` 换成解压出来的那个文件夹，其余原样。JSON 里路径用 `\\`（或 `/`）。

放置位置（各客户端自己的规则，通常只需其中一处）：

| 客户端 | 位置 |
|---|---|
| Trae | 设置里的 MCP 服务，或项目的 MCP 配置 |
| Qoder | 个人设置 → MCP 服务，或项目 `.qoder/config/mcp.json` |
| WorkBuddy | `~/.workbuddy/mcp.json` 或项目 `.workbuddy/mcp.json` |
| Claude Desktop | `claude_desktop_config.json` 的 `mcpServers` |

如果客户端读的是单独的 json 文件，上面整段就是文件内容，一个字都不用改。
配好后重启客户端，工具列表里应出现 12 个：`list_scripts`、`read_script`、`edit_editor`、
`check_editor`、`validate_editor`、`patch_editor`、`run_editor`、`run_script`、
`get_session_output`、`stop_script`、`list_sessions`、`stop_all`。

## 二、怎么跟 AI 说

```
用 ahkdeck 的 MCP 工具帮我写一个 AHK v2 脚本：每 5 秒在 A_ScriptDir 下追加一行时间戳。
先写进编辑区，运行，看到报错就改，直到 rc=0。
```

AI 的典型动作是 `edit_editor` → `run_editor` → 读 `output` 里的 `line` / `message` → 改 →
再 `run_editor`。失败时返回 `rc`、`fatal`、`phase`（load/run）、`file`、`line`、`message`。

`edit_editor` 还会回 `changed` 与 `lines_before` / `lines_after` / `first_changed_line` /
`last_changed_line`，改完只重读变动的那几行；`get_session_output` 可用 `max_bytes`（默认 32768）
限制读取长度，`truncated:true` 表示前面还有更早的输出没读到。要连续跟读同一个会话时，
把上次返回的 `next_offset` 作为 `after_offset` 再读，只拿新增部分；返回 `rewound:true`
表示日志被轮转或截断过（游标已从 0 重来），`log_rotated:true` 表示旧文件已存为 `.1`。

会话不是只能靠记住编号：`list_sessions` 列出本次宿主里所有会话（编号、脚本名、状态、rc），
`stop_all` 一次停掉所有还在跑的会话并回报 1 秒后仍在的数量。

运行前可以先让 AI 调 `check_editor`：它只读编辑区做静态检查，能提前发现 v1 残留写法
（如 `MsgBox, 123`、`%var%`）和未闭合引号，省掉一次必然失败的运行。
想确认「语法到底能不能过」时调 `validate_editor`：它用内核自己的解析器校验编辑区，
**只解析、不执行**；有语法错时 `rc=2`，`findings` 带行号与内核原话，`ok=true` 就说明可以运行。
只改一两行时用 `patch_editor`：`old_text` 必须唯一命中，否则整批拒绝、编辑区不变；
`preview:true` 只回变更摘要、不落地。

服务端在 `initialize` 里回带 `instructions`：上面这条「写 → 静态排查 → 内核验证 →
运行 → 读错 → 修」的闭环由客户端直接注入模型上下文，不必写进提示词。
`check_editor` 的 `error` 只是文本启发式、可能误报，**语法能否通过一律以
`validate_editor` 为准**；`run_editor` / `run_script` 在 `verdict` 非 `pass` 时还会回带
`next`，直接说明下一步该调哪个工具（`get_session_output`、`patch_editor`、`stop_script`）。

## 三、边界

- **不泄露本机路径**：工具输出只给脚本 `id`（`S1`…）和文件名，不给盘符全路径；越界路径
  （`..`、非 `.ahk`、设备名等）直接拒绝。
- **`--mcp-stdio` 拉起的是 deck 窗口本身**：脚本在这个进程里跑，关掉宿主脚本同步结束。
  它**不走单实例唤醒**——所以别在已经手开一个 AHKDeck 的情况下再让客户端拉一个，
  两个窗口会各自读写同一份 `Scripts\` 与 `settings.json`。
- **客户端断开**（stdin 关闭）只结束端点线程，窗口照常可用；窗口关闭 = 服务结束，
  客户端下次调用会失败重连。
- 协议是换行分隔的 JSON-RPC 2.0；端点日志走 **stderr**，不污染协议帧。

## 四、排错

| 现象 | 处理 |
|---|---|
| 客户端里看不到 ahkdeck | `command` 是否指向 `AHKDeck.exe`、`args` 是否为 `["--mcp-stdio"]` |
| 报 "not an AHKDeck-capable kernel" | `AutoHotkey64.dll` 必须与本 exe 同目录，且是发行包里的那份 |
| 工具调用超时 | 常驻脚本会返回 `state:"running"`，先 `get_session_output`，再 `stop_script` |
| 想手工验证 | `AHKDeck.exe --version` 正常即说明 exe 可用；MCP 只多了一条 stdio 通道 |
