# 界面量测：inspect_ui、capture_ui 与 ui_pixel

这三个工具回答同一个问题：**我刚写的界面，实际落在屏幕上的哪里、长什么样。**
一个给几何与控件树（含列表条目与控件当前值），一个给整窗像素，一个给单点颜色。
都是只读——不激活目标窗口、不改样式、不发输入。

只有在要核对布局时才需要读这一份；平时写脚本、跑脚本看 [MCP接入](MCP接入.md) 就够了。

## 什么时候用

- 写完一段 §10 布局，想知道那十几行 `x,y,w,h` 有没有落偏。
- 界面看着不对，但不知道是哪个控件、偏了几个像素。
- 一个控件该出现却没出现，要确认它到底建没建。
- 想核实某个像素是不是设计里的那个颜色。
- 想知道列表里是哪几行、勾选框勾没勾、滑块停在哪、当前选了第几个页签。

不需要用它的时候：纯粹的逻辑错误、语法错误、热键不响应——那些看
`get_session_output` 的报错更快。

## inspect_ui

```
session   三选一   会话编号（run_script / run_editor / list_sessions 的返回值）
hwnd      三选一   窗口句柄（十六进制 0x001307A8 或十进制）；只看这一个窗口，含隐藏窗口
pid       三选一   进程编号；枚举该进程的顶层窗口，任何第三方程序都可以
scope     可选     tree（默认，含全部子控件）| windows（只要顶层窗口）
max       可选     最多返回多少个控件，默认 200，上限 500
wait_ms   可选     等窗口出现的最长毫秒，默认 1500，上限 5000
items     可选     true 时读控件里的内容：七类控件的条目 + 所有控件的当前值，默认 false
rows      可选     items=true 时最多返回多少行，默认 50，上限 200
```

`session` / `hwnd` / `pid` 三选一：`session` 看本宿主的脚本；`hwnd` 或 `pid` 看任何别的程序
（写「操作某个软件」的脚本时，先用它看清对方窗口里有什么）。

返回：

```json
{
  "ok": true,
  "session": 7,
  "count": 1,
  "controls": 5,
  "truncated": false,
  "windows": [
    {
      "hwnd": "0x00120A3C", "depth": 0, "class": "AutoHotkeyGUI", "text": "定时器",
      "client": {"x":0,"y":0,"w":420,"h":388},
      "screen": {"x":742,"y":302,"w":420,"h":388},
      "style": "0x16CF0000", "exstyle": "0x00000100",
      "visible": true, "enabled": true, "dpi": 96,
      "controls": [
        {"hwnd":"0x00120A4A","depth":1,"class":"Static","text":"",
         "client":{"x":16,"y":76,"w":388,"h":242},
         "screen":{"x":758,"y":378,"w":388,"h":242},
         "style":"0x50000000","exstyle":"0x00000000","visible":true,"enabled":true},
        {"hwnd":"0x00120A5C","depth":1,"class":"Edit","text":"08:30:00",
         "client":{"x":17,"y":325,"w":226,"h":26},
         "screen":{"x":759,"y":627,"w":226,"h":26},
         "style":"0x500100C4","exstyle":"0x00000000","visible":true,"enabled":true}
      ]
    }
  ]
}
```

| 字段 | 含义 |
|---|---|
| `client` | **相对顶层窗口客户区**的矩形，与开发指南 §10 坐标表同口径 |
| `screen` | 屏幕物理像素矩形（多显示器时用） |
| `depth` | 0 是顶层窗口，1 是它的直接子控件，更深是子控件的子控件 |
| `class` | 窗口类名：`AutoHotkeyGUI` / `Static` / `Edit` / `Button` / `SysListView32` / `msctls_trackbar32` |
| `text` | 控件文字；`Static` 的标题、`Edit` 的正文都在这儿，读不到给 `""` |
| `style` / `exstyle` | `GWL_STYLE` / `GWL_EXSTYLE` 的十六进制 |
| `dpi` | `GetDpiForWindow`，只有顶层窗口有 |
| `count` / `controls` / `truncated` | 可见顶层窗口数、控件总数、是否被 `max` 截断 |

**顶层窗口的 `client` 恒为 `0,0,客户区宽高`**（不含边框），`screen` 才是含边框的外框。
所以顶层窗口两个框宽度会差 16 像素左右，这是正常的。

## items：控件里的条目与当前值

**所有 Win32 控件本来就在 `controls` 里**——它们是窗口，`inspect_ui` 默认就给出类名、文字、
坐标、样式、状态位。`items=true` 补的是那些**不在窗口文字里**的东西，分两层：

- `items`：七类控件的条目（一行行的文字，不是窗口）
- `state`：任意控件的当前值（一个数或一个状态，不是一行文字）

### 条目：`items`

| 控件类名 | `kind` | 读法 |
|---|---|---|
| `SysListView32` | `listview` | 多列，`columns` 取自表头控件（`-Hdr` 隐藏表头时仍然有效） |
| `ListBox` | `listbox` | 单列 |
| `ComboBox` | `combobox` | 单列，读的是下拉列表里的项，不是当前选中值 |
| `SysTreeView32` | `treeview` | 单列 + `levels` 层级（0 是根，行序是深度优先） |
| `SysTabControl32` | `tab` | 单列，每行一个页签标题 |
| `msctls_statusbar32` | `statusbar` | 单列，每个分栏一行 |
| `SysHeader32` | `header` | 单列，每列一个标题（ListView 表头就是它） |

```json
{"class":"SysListView32","text":"","state":{},
 "items":{"kind":"listview","count":2,"columns":2,
          "rows":[["文件A","存在"],["文件B","缺失"]],
          "selected_count":0,"checked_rows":[0],"truncated":false}}
{"class":"SysTreeView32","text":"",
 "items":{"kind":"treeview","count":4,"rows":[["根一"],["子一"],["子二"],["根二"]],
          "truncated":false,"levels":[0,1,1,0],"selected":"根一"}}
```

| 字段 | 含义 |
|---|---|
| `kind` | 见上表 |
| `count` | 条目总数（被 `rows` 截断之前） |
| `columns` | 只有 listview 有：列数 |
| `rows` | 每行每列的文字；除 listview 外每行只有一个元素 |
| `levels` | 只有 treeview 有：每行的层级，与 `rows` 等长 |
| `selected` | treeview 有：选中节点的文字 |
| `selected_count` / `checked_rows` | listview 有：选中行数、勾选的行号（从 0 起） |
| `truncated` | 是否因为 `rows` 上限少给了行 |
| `items_error` | 读不到时的原因（控件无响应 / 目标进程打不开）；此时没有 `items` |

### 当前值：`state`

所有控件都可能带 `state`，读不到的字段直接不给，不会填 0 冒充。

| 控件类名 | `state` 字段 | 含义 |
|---|---|---|
| `Button`（勾选/单选/三态） | `checked` | 0 未勾 / 1 勾 / 2 不确定 |
| `ComboBox` / `ListBox` / `SysTabControl32` | `selected` | 当前选中项序号（从 0 起） |
| `ListBox`（多选） | `selected_count` | 选中行数 |
| `msctls_trackbar32` / `msctls_progress32` | `pos` / `min` / `max` | 当前位置与范围 |
| `msctls_updown32` | `pos` | 当前值 |
| `SysDateTimePick32` / `SysMonthCal32` | `date` | `YYYY-MM-DD HH:MM:SS` |
| `msctls_hotkey32` | `hotkey` | AHK 写法，如 `^+!D` |

```json
{"class":"Button","text":"记住","state":{"checked":1}}
{"class":"msctls_trackbar32","text":"","state":{"pos":35,"min":0,"max":100}}
```

大写字母在 Hotkey 控件里存成 Shift+键，所以脚本写 `^!D`，读回来是 `^+!D`——这是控件语义，
不是解码错误。

### 实现事实（改这块代码前必须知道）

- **编组规则按消息族分，不按「是不是控件」分。** `LB_*` / `CB_*` 由系统代为编组
  （与 `WM_GETTEXT` 同待遇）→ 必须用**本地**缓冲区；`LVM_*` / `TVM_*` / `TCM_*` / `SB_*` / `HDM_*`
  不编组 → 结构体与文字缓冲都要 `VirtualAllocEx` 到目标进程，结构体自带指针，每次调用前重写。
- **TreeView 的层级要自己走**：`TVM_GETCOUNT` 只给总数，层级靠 `TVM_GETNEXTITEM` 的
  `TVGN_CHILD` / `TVGN_NEXT` 递归；只按 `TVGN_NEXT` 走会得到「所有根节点平铺」。
- **`DTM_GETSYSTEMTIME` 成功返回 `GDT_VALID`（0），失败是 `GDT_ERROR`（−1）**，
  而 `MCM_GETCURSEL` 成功返回非零。两者「成功」的约定相反，按 BOOL 统一判断会把日期全丢掉。
- **`UDM_GETPOS` 的错误标志在高字**，非零时低字不是有效位置，不能报出来。
- 每次发送都带 100 ms 超时：对方卡死时工具不跟着卡住，代价是那一格读不到。

## capture_ui

```
session   必需   会话编号
window    可选   第几个可见顶层窗口，默认 0（序号与 inspect_ui 的 windows 一致）
rect      可选   只截一块，客户区坐标 "x,y,w,h"，与 inspect_ui 的 client 同口径
wait_ms   可选   等窗口出现的最长毫秒，默认 1500，上限 5000
```

返回 BMP 的相对路径（相对 `AHKDeck.exe` 所在目录）、宽高与字节数。用 `PrintWindow` 让窗口
自己绘制，所以窗口被别的程序挡住、甚至不在前台也截得到；屏幕拷贝在这种时候只会拍到遮挡物。
看间距、字号渲染、圆角、颜色层次用它；字段级核对用 `inspect_ui`，单点取色用 `ui_pixel`。
`rect` 可直接填 `inspect_ui` 里某个控件的 `client`，只看那一块，省 token。

## ui_pixel

```
x  必需   屏幕坐标 x
y  必需   屏幕坐标 y
```

返回 `{"ok":true,"x":758,"y":378,"rgb":"B7C8E0"}`；取不到时 `rgb:""` 并带 `reason`。

单点约 16ms，**没有批量接口**——扫一块要几十秒，那个代价得调用方自己决定要不要付。

## 判读口径（这几条不看会误判）

1. **差 1 像素不是错位。** §10 的框线画法是「父块铺 `FRAME` 色 + 原生控件内缩 1px」，
   所以同一个框会量到两项：父块 `client 16,76 388×242`（`text` 为空）和内块
   `client 17,77 386×240`。要和源码比，比**父块**那一项；内块本来就小 1px。
2. **空 ListView 不画背景。** 列表没有行时露出的是父块框线色，`ui_pixel` 读到 `B7C8E0`
   而不是 `FFFFFF`。这是真缺陷，不是取色错误；正解是在列表下面先铺一块同尺寸白底垫块。
3. **同位置多个 `Static` 是三层色块，不是重复控件。** `FlatButton` / `FlatCheck` 就是
   外块 + 内块 + 文字三块，坐标各差 1px，`depth` 都是 1。删掉任何一层都会缺一条边。
4. **坐标是物理像素。** 缩放 150% 时 `dpi` 读 144，坐标不折算，和源码里的数字同号。
5. **窗口是异步建的。** `run_script` 返回 `running` 不代表窗口已建好。`wait_ms` 等不到
   就返回 `windows:[]` 和一条 `note`，这时要么重试，要么先 `get_session_output` 看是不是
   脚本在 `Gui.Show` 之前就报错了。

## 典型闭环

```
edit_editor    → 写界面
run_editor     → 跑起来（verdict 可能是 resident，常驻窗口正常）
inspect_ui     → 拿控件树，把每项 client 和源码里的 x,y,w,h 逐行比
inspect_ui     → items=true 核对列表行、勾选状态、滑块值这些「框里的东西」
patch_editor   → 改偏掉的那一行
run_editor     → 再跑
inspect_ui     → 复查
capture_ui     → 看整窗外观（间距、字号渲染、圆角）
ui_pixel       → 需要确认颜色时补一个点
```

给模型的提示可以很简单：

```
跑起来之后用 inspect_ui 量一下这个窗口，把每个控件的 client 和我源码里写的
x,y,w,h 逐行对一遍，只报对不上的那几行。
```

## 边界

- 只列**可见**窗口。脚本藏起来的主窗口、`+Hidden` 的 Gui 不会出现在结果里。
- 只列**该会话**的窗口：按脚本进程归属（m192 起内核是独立子进程），不会把宿主的界面或别的会话混进来。
- 深度上限 8 层，够 §10 用；更深的结构会被截断，`truncated` 不一定为真（这是笔数上限，不是深度标记）。
- 读不到 `Edit` 正文时不报错，给 `""`——读正文走 `WM_GETTEXT` 且带 40ms 超时，
  脚本忙到 40ms 不响应就会空一次，重调即可。
