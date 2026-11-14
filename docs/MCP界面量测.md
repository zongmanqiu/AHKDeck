# 界面量测：inspect_ui 与 ui_pixel

这两个工具回答同一个问题：**我刚写的界面，实际落在屏幕上的哪里、长什么样。**
一个是几何与控件树，一个是单点颜色。都是只读——不激活目标窗口、不改样式、不发输入。

只有在要核对布局时才需要读这一份；平时写脚本、跑脚本看 [MCP接入](MCP接入.md) 就够了。

## 什么时候用

- 写完一段 §10 布局，想知道那十几行 `x,y,w,h` 有没有落偏。
- 界面看着不对，但不知道是哪个控件、偏了几个像素。
- 一个控件该出现却没出现，要确认它到底建没建。
- 想核实某个像素是不是设计里的那个颜色。

不需要用它的时候：纯粹的逻辑错误、语法错误、热键不响应——那些看
`get_session_output` 的报错更快。

## inspect_ui

```
session   必需   会话编号（run_script / run_editor / list_sessions 的返回值）
scope     可选   tree（默认，含全部子控件）| windows（只要顶层窗口）
max       可选   最多返回多少个控件，默认 200，上限 500
wait_ms   可选   等窗口出现的最长毫秒，默认 1500，上限 5000
```

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
patch_editor   → 改偏掉的那一行
run_editor     → 再跑
inspect_ui     → 复查
ui_pixel       → 需要确认颜色时补一个点
```

给模型的提示可以很简单：

```
跑起来之后用 inspect_ui 量一下这个窗口，把每个控件的 client 和我源码里写的
x,y,w,h 逐行对一遍，只报对不上的那几行。
```

## 边界

- 只列**可见**窗口。脚本藏起来的主窗口、`+Hidden` 的 Gui 不会出现在结果里。
- 只列**该会话**的窗口：按脚本线程归属，不会把宿主的界面或别的会话混进来。
- 深度上限 8 层，够 §10 用；更深的结构会被截断，`truncated` 不一定为真（这是笔数上限，不是深度标记）。
- 读不到 `Edit` 正文时不报错，给 `""`——同进程读正文走 `WM_GETTEXT` 且带 40ms 超时，
  脚本线程忙到 40ms 不响应就会空一次，重调即可。
