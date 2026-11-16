# 界面量测：inspect_ui、capture_ui、ui_pixel、ui_diff、send_input、assert_ui 与 wait_ui

这七个工具回答同一个问题：**我刚写的界面，实际落在屏幕上的哪里、长什么样，交互对不对。**
它们分三组：**测量组**（inspect_ui 给几何与控件树／capture_ui 给整窗像素／ui_pixel 给单点颜色／ui_diff 比对两帧像素）与**断言组**（assert_ui 判定条件、wait_ui 有界等待）是只读的——不激活目标窗口、不改样式、不发输入；
**send_input 是唯一的写入工具**，会向目标窗口发送键鼠输入。用之前先看清这一条，别把交互工具当只读量具。

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
| `client` | **相对顶层窗口客户区**的矩形，与 docs/UI规范.md §10.3 坐标表同口径 |
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
stable    可选   连续相同帧数（≥2 启用）：反复重截直到画面连续 N 帧不变，防「截到重绘中间态」；
                 stable_timeout 默认 3000ms。滚轮/点击后马上截图时建议 stable=2
wait_ms   可选   等窗口出现的最长毫秒，默认 1500，上限 5000
```

返回 BMP 的相对路径（相对 `AHKDeck.exe` 所在目录）、宽高与字节数。用 `PrintWindow` 让窗口
自己绘制，所以窗口被别的程序挡住、甚至不在前台也截得到；屏幕拷贝在这种时候只会拍到遮挡物。
看间距、字号渲染、圆角、颜色层次用它；字段级核对用 `inspect_ui`，单点取色用 `ui_pixel`。
`rect` 可直接填 `inspect_ui` 里某个控件的 `client`，只看那一块，省 token。

## ui_diff

逐像素比对两张 capture_ui 截图，把「画面哪里变了」变成一条可断言的返回值。

    a / b     必填   capture_ui 返回的 path（exe 相对路径）
    threshold 可选   逐像素三通道差值和的门槛，0-255，默认 0（任何变化都算）；
                     抗锯齿噪声明显时调大（如 8）
    rows      可选   最多返回多少条 band，默认 10，上限 50

返回：`same`（是否逐像素一致）、`diff_pixels`、`diff_ratio`、`bbox`（变化区域包围盒）、
`bands`（按变化量降序的连续行段，每条 y0/y1/changed）、`samples`（最多 5 个样本点）。

判读口径：

- **等宽、等距的 band** = 行级缺陷：裁字（每 4 行一条）、滚动残影、斑马纹错位。
- **一整片大 band（占图一半以上）** = 整块重绘、位移或遮挡。
- **bbox 的 x/y 变了** = 元素位移（实案：排序后表头左移 16px，就是 bbox 抓出来的）。
- 两张图尺寸不同直接报错——先确认两次截图之间窗口没被缩放。

标准验收闭环：**改 UI 前后各 capture_ui 一次 → ui_diff → 把 diff_pixels 与 bbox 写进
HISTORY 或测试断言**。

## send_input

把鼠标/滚轮/键盘消息用 PostMessage 注入会话窗口，序列与真实输入一致，控件端区分不出
来源。不碰真实光标键盘、不抢前台、不受 UIPI 限制（目标是本宿主会话自己的窗口）。

    session   必填   会话编号
    action    必填   click / rclick / dblclick / wheel / key / text / move
    x / y     可选   客户区坐标（inspect_ui 的 client 口径），省略取客户区中心
    window    可选   第几个可见顶层窗口，默认 0
    delta     可选   wheel 专用，一格 ±120，正=向上，默认 -120
    key       可选   key 专用：Enter / Esc / Tab / Space / PageUp / PageDown / Home / End /
                     Up / Down / Left / Right / Delete / Backspace / Insert / F1..F12
    text      可选   text 专用，逐字符 WM_CHAR，喂输入框

要点：

- **坐标按「最深的非透明子控件」命中**，与真实点击同规则：Text 色块没加 `+0x100`（K19）
  时会像真实点击一样落在父窗口上。
- **key / text 送焦点控件**。脚本刚启动时焦点常为空——先用 `click` 点一下要输入的控件
  （真实点击会设焦点），工具查不到焦点时会临时挂入输入队列把焦点给坐标命中的控件
  （返回 `focus.set_by_tool=true`），两样都不行才报错。
- **为什么需要它**：回调用错（K32 对象当函数调用）、右键菜单挂错消息（K45/K46）、
  滚轮焦点错位（K29）——这些 bug 截图全是常态，只有把消息真的送进控件才暴露。
- 验证 Click 回调：点击后 `inspect_ui(items=true)` 看行数/状态，或 `get_session_output`
  看回调里的输出；改画面再用 capture_ui + ui_diff。

## assert_ui / wait_ui（判定，不靠肉眼）

前四个工具负责「看」和「动」，这两个负责「判」：内部调同一个 `inspect_ui` 拿快照，
再按 `checks` 逐项求值，工具直接返回 `verdict=pass/fail`。`ok=true` 只表示工具调用成功，
**`verdict` 才是断言结论**——脚本测试可以直接拿它当硬断言，不必自己解析界面树再判断。

    session/hwnd/pid  三选一，与 inspect_ui 完全相同
    items/rows/max/wait_ms  透传给内部 inspect_ui
    checks            必填，非空数组，最多 64 项
    timeout_ms        wait_ui 专用，默认 3000，上限 15000
    interval_ms       wait_ui 专用，默认 100，范围 30-1000

每项 check：

    select   {class, text, hwnd, index}：class 精确（忽略大小写）、text 包含匹配、
             index 取第几个命中（默认 0）；不传 select 则选顶层窗口
    field    点分路径：text / class / visible / enabled / state.checked /
             items.count / controls.count；数组上的 count 是元素数
    op       eq（默认）/ ne / contains / gte / lte / gt / lt / truthy / exists
    value    期望值，字符串 / 数字 / 布尔

判读要点：

- 顶层窗口 + 各窗口的 controls 是一个有序候选序列，`index` 按这个顺序数，
  和 `inspect_ui` 返回里看到的顺序一致。
- `select` 没匹配到控件时该 check 记 `pass=false`、`note="没有匹配的控件"`；
  带 `items.count` 的断言要先传 `items=true`。
- `wait_ui` 每轮内部 `wait_ms=0`，等待完全由 `timeout_ms`/`interval_ms` 控制，
  不会出现每轮各等 1.5s 叠成十几秒。耗时看 `elapsed_ms` 与 `attempts`。
- 超时回 `verdict=fail` + `timed_out=true`，并保留最后一次的逐项 `checks` 供诊断；
  条件本来就已满足时一轮就返回。

一组典型断言：

```json
{
  "session": 1,
  "items": true,
  "checks": [
    {"select": {}, "field": "visible", "op": "eq", "value": true},
    {"select": {"class": "SysListView32"}, "field": "items.count", "op": "eq", "value": 2},
    {"select": {"class": "Button", "text": "确定"}, "field": "text", "op": "contains", "value": "确定"}
  ]
}
```

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
assert_ui      → 直接判定通过与否（verdict=pass/fail），不必肉眼比对
wait_ui        → 窗口/列表异步就绪时先等再判
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
