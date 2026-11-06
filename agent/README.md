# agent/ — 脚本工具层与 MCP 端（m063／m064）

一句话：**工具的唯一定义处**。谁驱动这 7 个脚本工具都行，实现与清单只有一份。

```
Toolbox     7 个脚本工具 + 中性工具定义（ToolDef）；线程安全；只经 core/AiPolicy 落盘
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

`list_scripts` / `read_script` / `edit_editor` / `run_editor` / `run_script`
/ `get_session_output` / `stop_script`

- 模型只看脚本 **id**（`S1`…），看不到本机绝对路径（规则 9）。`list_scripts` 每次都重新枚举，
  所以外部 agent 新建的文件下一句就能看见。
- `edit_editor` 只写编辑区、不落盘；它把内容投回 UI 线程应用，并等一拍，
  保证紧随其后的 `run_editor` 跑的是刚写进去的文本，不是上一拍的旧缓冲。
- `write_script`（落盘）**不在对外清单里**，只作为内部分支保留：`test/mcp_gate` 拿它验越界拒绝，
  越界路径（`..`、盘符外、非 `.ahk`、设备名、备用数据流）一律拒绝。
- 常驻脚本：`run_editor`/`run_script` 超时后回 `state:"running"` + 会话号，
  由调用方 `stop_script`。"跑通"的结构化 verdict 归 m062。

## 接入 MCP 客户端

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
