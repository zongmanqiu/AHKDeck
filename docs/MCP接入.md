# AHKDeck 的 MCP 接入

AHKDeck 自带一个 **MCP stdio 端点**。挂到支持 MCP 的 AI 客户端（Trae、Qoder、WorkBuddy、
Claude Desktop、Cursor…）之后，AI 可以：

- 列出脚本库、读取任意脚本；
- 往编辑区写脚本（不落盘，等人检查）；
- 运行编辑区 / 运行已有脚本，**拿到内核原话的报错：行号 + 那一行原文 + 调用链**；
- 读会话输出、停止常驻脚本；
- **量出脚本窗口的控件树与像素色**，核对布局到底落在哪。

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
配好后重启客户端，工具列表里应出现 15 个，按用途分五组：

| 用途 | 工具 |
|---|---|
| 看与改源码 | `list_scripts`、`read_script`、`edit_editor`、`patch_editor` |
| 检查与运行 | `check_editor`、`validate_editor`、`run_editor`、`run_script` |
| 会话与输出 | `get_session_output`、`list_sessions`、`stop_script`、`stop_all` |
| 界面量测 | `inspect_ui`、`ui_pixel` —— 字段语义与判读口径见 [MCP界面量测](MCP界面量测.md) |
| 语法查询 | `lookup_help` —— 离线查 AHK 函数签名与参数，见下文第五节 |

这份文档只讲「怎么接、怎么跑、怎么排错」。界面量测是另一份，需要核对布局时再读，
免得每次把用不上的口径一起带进上下文。

## 二、怎么跟 AI 说

```
用 ahkdeck 的 MCP 工具帮我写一个 AHK v2 脚本：每 5 秒在 A_ScriptDir 下追加一行时间戳。
先写进编辑区，运行，看到报错就改，直到 rc=0。
```

AI 的典型动作是 `edit_editor` → `run_editor` → 读 `errors[]` → 改 → 再 `run_editor`。

`edit_editor` 还会回 `changed` 与 `lines_before` / `lines_after` / `first_changed_line` /
`last_changed_line`，改完只重读变动的那几行；`get_session_output` 可用 `max_bytes`（默认 32768）
限制读取长度，`truncated:true` 表示前面还有更早的输出没读到。要连续跟读同一个会话时，
把上次返回的 `next_offset` 作为 `after_offset` 再读，只拿新增部分；返回 `rewound:true`
表示日志被轮转或截断过（游标已从 0 重来），`log_rotated:true` 表示旧文件已存为 `.1`。

会话不是只能靠记住编号：`list_sessions` 列出本次宿主里所有会话（编号、脚本名、状态、rc），
`stop_all` 一次停掉所有还在跑的会话并回报 1 秒后仍在的数量。

## 三、出错之后：定位到具体哪一行

`errors[]` / `warnings[]` 里的每一条都是内核在**出错那一刻**抓到的事实，不是事后猜的：

| 字段 | 内容 | 用来回答 |
|---|---|---|
| `line` | 出错行号 | 是哪一行 |
| `source` | **那一行的原文** | 那行到底写了什么，不用去数行 |
| `message` | 内核原话 | 为什么错 |
| `extra` | 内核的 `Specifically:` 片段 | 错在表达式的哪一截 |
| `file` | 文件名（叶子，不是全路径） | 哪个文件 |
| `func` | 出错时正在执行的函数 | 在谁的函数体里 |
| `stack` | 调用链，顶层在前，每帧 `文件 (行) : [函数] 该帧原文` | 怎么走到这儿的 |

```json
{
  "kind": "error", "phase": "runtime", "severity": "fail_or_ok",
  "file": "ai-editor.ahk", "line": 11,
  "source": "    x := n / 0",
  "func": "Inner",
  "message": "Divide by zero.",
  "stack": "ai-editor.ahk (11) : [Inner] x := n / 0\nai-editor.ahk (7) : [Outer] Inner(3)\n> timer"
}
```

`warnings[]`（最多 5 条）是 `#Warn` 那一类，形状相同。它常常比错误更有用：`#Warn` 报的
「这个变量可能从未赋值」通常就是拼错的变量名，而它不会让脚本失败。

判读时的三条口径：

1. **`file` 是叶子名**，因为工具输出不许带盘符路径。同一次运行里出现两个同名文件才是问题
   （`#Include` 了别处的同名文件），否则按名字对得上就行。
2. **`line` 数的是它自己那个文件**，不是编辑区的行。`script_dir=temp` 时运行的是临时副本，
   内容和编辑区一致、行号也一致；一旦脚本 `#Include` 了别的文件，就得看 `file` 是谁。
3. **`stack` 是「怎么到这儿」的唯一线索**。同样是第 11 行，从热键里进来和从定时器里进来
   是两种完全不同的修法。`> timer`、`> OnMessage`、`> Gui` 这类行是线程入口。

`validate_editor` 的 `findings[]` 同样带 `line` / `source` / `message` / `extra`，所以
「语法错在哪一行」在没跑起来之前就能看清。

## 四、检查与运行

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

**运行期产物只有一个根**：exe 旁的 `temp/`（m177）。`temp/logs/` 是会话 JSONL，
`temp/work/` 是校验与 `script_dir=temp` 的试跑副本，`temp/backups/` 是 `write_script`
覆盖前的旧版。工具结果里的路径都写相对形式（`temp/logs/...`），要给人看就把绝对路径
拼上 exe 目录。这四类文件共用同一个保留策略：「设置 > 选项…」选永不 / 7 / 30 / 180 天，
每次启动清一次。

## 五、语法查询：不确定就查

`lookup_help` 查的是随包的离线索引（`docs\help-index.json`，356 页，由官方
`AutoHotkey_2.0.30/AutoHotkey.chm` 生成）。索引取自官方 2.0.30 手册，比本机内核
`AutoHotkey64.exe`（2.0.31）略旧：绝大多数条目一致，但 2.0.31 新增的内容索引里可能查不到，
拿不准就先 `run_editor` 试一次。网上那份 Markdown 镜像只到 2.0.18，比这两者都旧。

**为什么要有它**：写错的 API 大多不是语法错。参数顺序颠倒、选项名拼错、把格式串当初始值，
这些代码能解析、能运行，只是做得不对。`validate_editor` 和 `run_editor` 都拦不住，
因为它们只看「能不能跑」。唯一的解法是写之前查一下。

```
lookup_help(query="StrSplit")
→ {"ok":true,"query":"StrSplit","exact":true,"total":1,
   "hits":[{"name":"StrSplit",
            "syntax":"Array := StrSplit(String , Delimiters, OmitChars, MaxParts)",
            "summary":"Separates a string into an array of substrings using the specified delimiters.",
            "params":[{"name":"String","type":"String","desc":"A string to split."},
                      {"name":"Delimiters","type":"String or Array","desc":"If blank or omitted, ..."}]}]}
```

判读三条：

1. **`exact` 分真假**。`true` 表示按名字或别名精确命中，可以照抄签名。`false` 只是模糊命中，
   先看 `name` 是不是你要的那个再决定——查 `Str` 会给你 `StrSplit`、`StrLen`、`StrReplace` 一串。
2. **`name` 可能是合页名**。AHK 的帮助会把同族函数写在一页，所以会看到
   `Send / SendText / SendEvent / SendInput / SendPlay`、`Run / RunWait`、`Gui Object`。
   这时看 `aliases`：你用哪个名字，看哪个参数表。
3. **`hits` 为空不是出错**。它会给 `hint`。换成英文名再查——中文描述匹配不到任何东西，
   因为索引里的正文是英文。

`full=true` 才回示例代码。示例平均 360 字节，是单条回答里最大的一块，默认不给。

索引缺了或坏了会直接报 `读不到 docs\help-index.json`。它随程序分发：装完整就有，
不需要联网，也不需要再装一份 AutoHotkey。索引本身由 `tools/make_help_index.py`
从 chm 生成，换内核版本时重跑一次即可。

## 六、边界

- **不泄露本机路径**：工具输出只给脚本 `id`（`S1`…）和文件名，不给盘符全路径；越界路径
  （`..`、非 `.ahk`、设备名等）直接拒绝。
- **`--mcp-stdio` 拉起的是 deck 窗口本身**：脚本在这个进程里跑，关掉宿主脚本同步结束。
  它**不走单实例唤醒**——所以别在已经手开一个 AHKDeck 的情况下再让客户端拉一个，
  两个窗口会各自读写同一份 `Scripts\` 与 `settings.json`。
- **客户端断开**（stdin 关闭）只结束端点线程，窗口照常可用；窗口关闭 = 服务结束，
  客户端下次调用会失败重连。
- 协议是换行分隔的 JSON-RPC 2.0；端点日志走 **stderr**，不污染协议帧。

## 七、排错

| 现象 | 处理 |
|---|---|
| 客户端里看不到 ahkdeck | `command` 是否指向 `AHKDeck.exe`、`args` 是否为 `["--mcp-stdio"]` |
| 启动报「找不到内核」或 `--run` 不工作 | `AutoHotkey64.exe`（官方 2.0.31 ＋ m192 补丁）必须与本 exe 同目录，且是发行包里的那份；包内不应再有 `AutoHotkey64.dll` 或 `tools\` |
| 工具调用超时 | 常驻脚本会返回 `state:"running"`，先 `get_session_output`，再 `stop_script` |
| 想手工验证 | `AHKDeck.exe --version` 正常即说明 exe 可用；MCP 只多了一条 stdio 通道 |
