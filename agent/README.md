# agent/ — 脚本工具层与 MCP 端（m063／m064）

一句话：**工具的唯一定义处**。谁驱动这 10 个脚本工具都行，实现与清单只有一份。

```
Toolbox     10 个脚本工具 + 中性工具定义（ToolDef）；线程安全；只经 core/AiPolicy 落盘
McpServer   MCP stdio 端：换行分隔 JSON-RPC，stdout 只放协议帧，日志走 stderr
```

## 为什么要有这一层

`ai/DeckAgent`（内建 OpenAI 兼容对话循环，m018）以前既是循环又是工具。m064 要加 MCP 端，
如果 MCP 那侧再写一份工具清单，就是"两处描述同一事实"——迟早互相指错。
所以工具先搬来这里（m063），**清单与实现只有 `Toolbox.cpp` 一份**。

m065 把那个循环删了：模型、重试、上下文管理交给外部 MCP 客户端，它们比我们的 8 步循环
做得好（实测：`glm-4-flash` 会把 v1 语法写进工具参数，且两工具指令只走一步就停）。
现在这一层只有一个消费者（MCP 端）加一个不依赖模型的门禁，但**工具清单仍然只此一处**。

## 工具面

`list_scripts` / `read_script` / `edit_editor` / `check_editor` / `validate_editor`
/ `patch_editor` / `run_editor` / `run_script` / `get_session_output` / `stop_script`

- 模型只看脚本 **id**（`S1`…），看不到本机绝对路径（规则 9）。`list_scripts` 每次都重新枚举，
  所以外部 agent 新建的文件下一句就能看见。
- `edit_editor` 只写编辑区、不落盘；它把内容投回 UI 线程应用，并等一拍，
  保证紧随其后的 `run_editor` 跑的是刚写进去的文本，不是上一拍的旧缓冲。
- `write_script`（落盘）**不在对外清单里**，只作为内部分支保留：`test/mcp_gate` 拿它验越界拒绝，
  越界路径（`..`、盘符外、非 `.ahk`、设备名、备用数据流）一律拒绝。
- 常驻脚本：`run_editor`/`run_script` 超时后回 `state:"running"` + 会话号，
  由调用方 `stop_script`。"跑通"的结构化 verdict 归 m062。
- 写入回执（m101）：`edit_editor` / `write_script` 返回 `changed`、`lines_before`、
  `lines_after`、`first_changed_line`、`last_changed_line`（公共前缀/后缀逐行比较），
  调用方只重读改动的那几行即可。
- 有界输出（m101）：`get_session_output` 可选 `max_bytes`（默认 32768，范围 64–65536），
  并返回 `truncated` / `bytes`，调用方能分清"没有更多"与"上面还有被截掉的内容"。
- 静态检查（m102）：`check_editor` 只读编辑区做纯文本扫描（v1 专有命令、v1 命令式写法、
  `%var%` 引用、未闭合引号），`ok=false` 表示有 error 级问题；不执行、不落盘。
- 小补丁（m102）：`patch_editor` 支持 `replace` / `insert_after` / `delete`，`old_text`
  必须唯一命中，否则整批拒绝；`preview=true` 只回变更摘要不改编辑区。
- 脚本传参（m102）：`run_editor` / `run_script` 可选 `args`（最多 16 个），脚本里用
  `A_Args` 读取。
- 语法预检（m103）：`validate_editor` 走内核自己的 `/validate` 模式——只解析、不执行。
  内核的验证入口只在命令行上有，且同一进程加载过脚本后会拒绝再验（`Line::sSourceFileCount`
  保护），所以这条路径由宿主把编辑区写成临时脚本、再以 `--validate` 拉起一个短命进程，
  读回 JSONL 报告。`rc=2` 表示有语法错，`findings` 带行号与内核原话；`ok=true` 即可运行。

## 接入 MCP 客户端

**面向用户/AI 的接入说明就在发行包里**：`docs\MCP接入.md`（配置示例就在正文里、10 个工具、
典型提示词、边界、排错）；程序内「帮助 > MCP 接入」用系统默认程序打开它。
本节是开发者视角的同一份事实，不重复那份文档的内容。

产品本身不需要密钥、不联网；客户端把我们的 exe 当一个 stdio 服务进程拉起。
配置里的路径请换成你自己机器上的安装目录（文档不写任何人的绝对路径）。

WorkBuddy —— `~/.workbuddy/mcp.json` 或项目 `<dir>/.workbuddy/mcp.json`：

```json
{
  "mcpServers": {
    "ahkdeck": {
      "command": "<安装目录>\\AHKDeck.exe",
      "args": ["--mcp-stdio"]
    }
  }
}
```

Qoder —— MCP 服务（个人设置）或项目 `.qoder/config/mcp.json`，同样的两个字段。

两家的配置里都只有 `command`/`args`/`env`，没有 url ⇒ **只有 stdio 这一条路**，
所以 `--mcp-stdio` 拉起的就是 deck 窗口本身：脚本、编辑区、控制台都在这一个进程里，
只有宿主进程真正退出时，脚本才随之结束；stdin 断开本身不会关闭窗口。

### 行为边界

- `--mcp-stdio` 不走单实例唤醒：客户端需要**这个**进程活着当它的服务。
  因此如果你已经手开了一个 AHKDeck，再让客户端拉起，会同时存在两个 deck
  （各自读写同一个 `Scripts\` 与 `settings.json`）。已知边界，MCP 用哪个就只开哪个。
- 客户端断开 stdin ⇒ 端点线程结束并在控制台留一行，窗口继续正常用；
  进程退出时端点线程是 detach 的，绝不 join（阻塞在 ReadFile 的读线程不能拖住退出）。

## 门禁

```
test/mcp_gate/mcp_client.py            握手、清单等于工具真源、-32601/-32602、
                                       坏脚本→读到内核原话→改对→通过、越界拒绝、常驻判活
test/mcp_gate/fake_mcp_server.py       故意答错的假服务端（自证钉子）
ctest: mcp_selftest（默认跑，无窗口） / mcp_stdio（LABEL=gui，AHKDECK_GATE_UI=1 才跑）
```

`mcp_selftest` 的判据是**必须翻红**：假服务端改协议版本、少报工具、把 `write_script`
放回清单、参数不用 JSON Schema、未知方法不给 -32601，客户端要逐条抓到才算量具是活的。
