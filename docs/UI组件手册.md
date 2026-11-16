# AHKDeck 官方脚本 UI 组件手册

> **定位**：按**组件**组织的成熟用法手册——每个组件「怎么用才对、哪些错别再犯、做完怎么验」。
> 配色 / 字号 / 坐标 / 框线的**设计标准**在 [`UI规范.md`](UI规范.md)（令牌 §10.2、版式 §10.3、框线 §10.4）；
> **本组件的配方代码在本文末尾附录**；踩坑证据在 [`UI踩坑.md`](UI踩坑.md)。
>
> 所有结论都有实测证据，出处是 [`UI踩坑.md`](UI踩坑.md) 的 K1–K50 表，本文按组件把它们
> 重新编组，方便「我要用 X，X 有什么坑」式查阅。**K 编号指 [`UI踩坑.md`](UI踩坑.md) 的表项。**
>
> 通用前提（§10.1）：脚本跑在自带 v6 manifest 的独立子进程里；自绘不是因为缺 manifest，
> 而是原生主题给不出封版色值与 1px 框线，控件内绘区和悬停态也不可控（K2/K17/K28）。
> 按钮、勾选框、列头、滚动条、日期框一律自绘，不直接用系统控件外观。

---

## 0. 画面不刷新：一条总规律（先读这个）

自绘色块的重绘时机不受控，这是大多数「画面不对」的根源（K20/K28/K33）：

1. **静态控件的底色改不动**：运行期 `Opt("Background...")` 不触发重绘，
   `InvalidateRect` / `UpdateWindow` / `RedrawWindow` 都救不回来（K28）。要变色就**重建控件**
   或用两层显隐，但显隐也有坑（见下）。
2. **叠放不可靠**：谁在上由 z 序决定，`SetWindowPos(HWND_TOP)` 压不住（K20）。
   自绘结构一律**几何互不重叠**（拆段、细缝铺满），不依赖 z 序。
3. **显隐后旧像素会残留**：把一块控件藏掉，它腾出的区域不重绘，旧色留在屏幕上（K28③、K33）。
   对策：腾出的区域底下永远垫一块**会正常重绘的静态控件**（同色底衬/白底块）。
4. **一次重排一次提交**：多个色块逐个 `Move` 会露出中间帧（K24、K33）。
   用 `BeginDeferWindowPos` / `DeferWindowPos` / `EndDeferWindowPos` 批量提交；
   若 `DeferWindowPos` 整批返回 NULL（列表刚 `Move` 后实测出现过），退化为逐块 `SetWindowPos`。
5. **长任务期间定时器不来**：AHK 单线程，同步块（填表/扫描）里 `SetTimer` 回调挤不进来（K38）。
   「事后补一刀」的清理必须放进**同一个同步块**、排在所有触发重算的动作之后。
6. **擦背景 = 闪**：一切重画调用（`InvalidateRect` 第三参、`RedrawWindow` 的 `RDW_ERASE`）
   都不要擦背景（K44、K47、K50）。列表类再加 `LVS_EX_DOUBLEBUFFER`。

**验证刷新类问题的唯一方法**：真机截图逐像素读（方法见 [`脚本开发流程.md`](脚本开发流程.md) §6），
不要靠肉眼和「代码看起来对」。

---

## 1. 按钮：`Text` 两层色块，不用系统 Button

**成熟用法**（配方代码：本文附录）：

- 两层结构：外块 `FRAME` 描边 + 内块缩进 1px `SOFT` 底；**两层都挂 `Click`**，否则 1px 边上点不动。
- 静态色块必须 `+0x100`（`SS_NOTIFY`），否则命中测试返回 `HTTRANSPARENT`，点击穿透到父窗口（K19）。
- 文字层 `+0x200`（垂直居中）固定高度。
- 宽度 = 文字宽 + 左右各 16px，**整组右对齐**；不要为铺满一行撑宽（K25）。
- 主次不用颜色分，一律 1px 描边（§10.7）。

**禁做**：

- 不做 hover 变色——重绘时机不受控、四条路全堵（K28），这是结案结论，别再试。
- 不手写 `GWL_STYLE` 美化系统 Button（K1：写错 `GWL_ID(-12)` 当样式位，按钮全哑）。

**验证**：真实点击（不是截图）+ 确认无错误框。回调用对象属性时必须 `this.cb.Call(v)`，
`this.cb(v)` 会多传 this 直接崩进程（K32）——**只有真点一下才暴露**。

---

## 2. 勾选框：`class FlatCheck` 三层 Text 色块

**成熟用法**（配方：本文附录）：

- 系统 `CheckBox` 的勾与底色是控件自绘的，样式位改不了（K16）——直接用 `FlatCheck`。
- 勾形用 `✓` + `s11 Bold`；`√` 太细、`✔` 太糊。
- 选中底色 `SOFT`；同排多枚勾选框注意点击热区覆盖内块与文字。

**禁做**：不要试图给系统 CheckBox 换主题/改样式位（K2、K16）。

**验证**：点选/取消各一次看状态色变化；`Tab` 键序不影响其它控件。

---

## 3. 输入框：`Multi` + 裁边 + `EM_SETRECT`

**成熟用法**（配方：本文附录）：

- `-E0x200` 抵消 AHK 默认加的 `WS_EX_CLIENTEDGE` 3D 凹陷边（K5）；别用 `+Border`。
- 要垂直居中必须 `Multi`（`EM_SETRECT(0xB3)` 对单行无效），但 `Multi` 会自动附
  `WS_VSCROLL|ES_WANTRETURN|ES_AUTOVSCROLL`，必须 `-VScroll -WantReturn` 去掉，
  想要单行不换行再加 `-Wrap`（它会补回 `ES_AUTOHSCROLL`）（K6）。
- 换过字号/高度后**重新抓图核 `top`**——字体度量变了垂直居中会漂。

**禁做**：不要省略三个减号选项——缺一个就出滚动条/回车换行/贴顶。

---

## 4. ListView：外框、列头、双缓冲、重绘时机

这是坑最密集的组件，分四块说。

### 4.1 外框与列头

- 带标题的列表区是**一个整体框**：外框从标题带顶边起、标题带与列表内缩 1px 落进同一个框，
  别让两者各带各的边（K27）。
- **外框不要铺 `FRAME` 底色**——铺了以后列表没重绘的区域会露出 16~17px 竖条（K42）。
  正解：白底块 + 四边各一条 1px `FRAME` 线。
- 列头自绘（`-Hdr`），原生列头是系统样式；最大化后列头带要**逐控件** `InvalidateRect(hwnd, 0, false)`
  跟随（K47，整窗 `Redraw` 会闪）。
- 底块与列头/按钮同 x/y/w/h，原生控件内缩 1px；底块**不要外扩 1px**（K7）。
- 改列宽后必须 `S.lv.Redraw()` 整表重绘，否则行底色停在旧列宽（K31）。

### 4.2 滚动：自绘滚动条

配方：本文附录；多页版差异（`硬件信息`）：`LVM_SCROLL` 定位、
`LVM_GETCOUNTPERPAGE` 取可视行数、逐块 `SetWindowPos`。

- `ShowScrollBar(FALSE)` 藏原生条**会顺带摘掉 `WS_VSCROLL` 样式位**：之后滚轮/翻页键全哑、
  滚动范围塌 0（K29）。所以行数/可视行数/首行号**自己维护**，滚轮与翻页键 `OnMessage` 接管。
- 隐藏动作放在会重算滚动范围的动作（`Move`/`Add`/`Delete`/`ModifyCol`）**之后**，且每次滚动
  （`LVM_ENSUREVISIBLE` 会把样式位置回，K34）结束当场再藏一次；`Sync` 60ms 只作兜底。
- **可视行数只在一个地方算**：`Layout()` 里 `Move` 完列表后再取 `LVM_GETCOUNTPERPAGE`，
  别处读缓存——`Move` 后立刻取拿到的是过期值（K43）。
- **横向滚动是死路**：横向 `LVM_SCROLL` 不生效且参数语义不同（K30）。撤掉横向条，列宽做进视口。
- 滚轮消息只发给**焦点**控件：焦点在输入框时列表收不到，按「窗口内任意控件」处理（K29）。

### 4.3 闪烁与残影

- 滚轮重绘**去掉 `RDW_ERASE`**；位移为 0 直接 return；列表开 `LVS_EX_DOUBLEBUFFER`
  （`0x1036` 发 `0x10000/0x10000`）（K44）。
- `LVM_ENSUREVISIBLE` 靠位块拷贝搬旧像素，交界处留半截字：每次滚动末尾
  `InvalidateRect(hwnd, NULL, FALSE)` + `UpdateWindow()` 整块重画（K50）。
  **第二参必须 NULL**——传全 0 的 RECT 是空区域，什么也不作废，重画悄悄失效。
- 批量填表（几百行以上）：填表期间整个 `ShowWindow(lv, SW_HIDE)`（一个像素都不画），
  填完摘条 → `WM_SETREDRAW 1` → `SW_SHOW`（K36）。`WM_SETREDRAW(0)` 压不住非客户区滚动条。
- `SW_HIDE` 丢焦点：隐藏前记 `hadFocus`，显示后补 `lv.Focus()`（K39）。

### 4.4 自定义绘制（改行文字颜色）

- **只改颜色（`clrText`）就返回 `CDRF_DODEFAULT(0)`**。返回 `CDRF_NEWFONT(2)` 等于宣称
  「字体已换」，控件按自己认定的字体重排该行 → 文字被行边界裁掉半截（K49，打开方式清理 1.5.6 实案）。
  真要换字体，必须先在 hdc 里 `SelectObject` 再返回 2。
- 行底色斑马纹可以在 custom draw 里画，但要与双缓冲配合验证。

### 4.5 右键菜单

- `Menu.Show([Callback, X, Y])` **第一参是回调不是 X**：要指定位置写 `Menu.Show(, x, y)`；
  一般直接 `Menu.Show()` 不传参，让它跟光标（K45）。
- ListView 右键按下后进内部 `DragDetect` 循环，`WM_RBUTTONUP` 收不到：菜单挂**按下**
  （0x204）时弹，且用 `SetTimer(cb, -30)` 异步（模态菜单卡住重绘）（K46）。

**ListView 整体验证**（对应 `UI规范.md` §10.11 多页/长列表清单）：
不超屏/超屏两种情况滚动条表现都实测；滚轮不闪；最大化跟随；右键能弹且跟光标；
逐行量文字墨迹完整性（K49/K50 的验收手段）。

---

## 5. 日期 / 时间框：裁剪 + 自绘按钮

**成熟用法**（配方：本文附录，`ClipField` + `MakeDateField`/`MakeTimeField`）：

- 第三参是**显示格式串**不是初始值；`.Value` 恒 14 位（K14）。
- 边框与右侧按钮都是控件内绘，样式位只影响一层（K17）：裁到只剩文字区（裁剪点 115），
  按钮改 `Text` 色块，点击按原生按钮坐标 `PostMessageW` 回去。
- 去下拉用 `+0x1`（DTS_UPDOWN）。

---

## 6. 滑块（可拖动指示）：`class FlatTrack`

出处：`夜灯`。要点：

- **几何拆段，永不叠放**（K20）：轨道 = 滑块前段 + 滑块后段；滑块变窄后槽两侧露底色，
  补与滑块同长的同色细缝（K23）。
- 滑块比槽高会上下溢出：溢出区垫**同高底衬**，否则移开后旧像素残留（K33）。
- 拖动用 `DeferWindowPos` 一次提交全部块；拖动中 `Sync` 只兜原生条、别重复 `Paint`（K24）。

---

## 7. 多页导航（`硬件信息` 范本）

- 导航项是自绘 Text 块，**选中态要有明确色差**（`BAND` 底），不是只改文字粗细。
- 切页重置：滚动位置与选中行都清零，上一页状态不残留。
- 结构性结论（别重走）：`DeferWindowPos` 在列表刚 `Move` 后可能整批失败 → 逐块 `SetWindowPos`；
  `IsChild(parent, parent)` 为假 → 补 `hwnd = gGui.Hwnd` 分支；最大化重排逐控件
  `InvalidateRect`（K47）；可视行数只在 `Layout()` 算（K43）。
- 自绘列头带（页内分区标题）同理：宽度跟随窗口，`OnSize` 逐控件重画。

---

## 8. 状态条 / 信息行 / 标题

- 状态条 h30；原生 `StatusBar` 没有 `.Value`，用 `.SetText()` / `.Text`；Win11 下
  `Background` 需 `-Theme`，要精确同色就用 `Text` 色块（K15）。
- **状态栏文字是阶段的指纹**：填表/扫描各阶段必须更新状态栏，否则调试时阶段判读会反
  （[`UI踩坑.md`](UI踩坑.md)「取证方法学」的实案：状态栏停在「正在扫描」而列表空，实际在填表）。
- 信息行只放事实不放说明文案（§10.5）。
- 系统标题栏染色：`DwmSetWindowAttribute(hwnd, 35/36/34, 0x00BBGGRR, 4)` **只写不读**，
  验证只能看像素（K11）。

---

## 9. 窗口本身

- 主窗类名 `#32770`，跨进程只按 `ahk_pid` 匹配（K15）。
- `Gui.Move()` 的 w/h 是**外框**，`Gui.Show("wN hM")` 才是客户区（K48）——尺寸断言一律量客户区。
- 类声明必须在 auto-execute 调用之前（K18），否则模态报错卡死 `/validate`。
- 关闭窗口先 `Stop()` 停定时器再 `Destroy()`，否则下一拍碰已销毁控件（K21）。
- 只有托盘入口 + 弹窗面板要 `Persistent(true)`（K10）。
- 工具行分三段：左固定、右贴缘、中间弹性，`OnGuiSize` 里重算（K35）；`FlatButton` 两层都要挪。
- 可缩放窗口记得 `+Resize +MinSize`，`OnEvent("Size", ...)`。

---

## 附录：控件配方代码（原 `UI规范.md` §10.5，可直接复制）

> 下面每段都是**真源逐字**。复制后只改坐标与文案，不改结构、不改色值、不改选项顺序。

**窗口骨架**

```ahk
gGui := Gui("", "窗口标题")
gGui.BackColor := "F7F9FC"
gGui.SetFont("s10", "Microsoft YaHei UI")
gGui.OnEvent("Close", (*) => (gScroll.Stop(), gGui.Destroy()))
gGui.OnEvent("DropFiles", OnDropFiles)   ; 可选
```

**按钮：`Text` 两层色块 + 1px 描边，不用系统 `Button`**

浅底黑字之后，按钮不能靠「底色比别人深」立形 —— 一层纯色块只会像一段色带。改成一圈 1px 描边：
外块铺描边色、内块四周缩进 1px 铺底色并写字。

```ahk
gBtnApply := FlatButton(gGui, 228, 344, 84, 28, "更新时间", "EBF2FB", "B7C8E0", (*) => ApplyAll())
gBtnClear := FlatButton(gGui, 320, 344, 84, 28, "清空列表", "EBF2FB", "B7C8E0", (*) => ClearList())

; 浅色底 + 黑字之后，按钮不再靠底色深浅立形，改靠 1px 描边：外块铺边框色、内块四周
; 缩进 1px 铺底色。两块都挂 Click —— 否则点在 1px 边上没反应，手感像坏了。
; 同样不用系统 Button：原生主题给不出封版的底色/1px 描边（§10.1）。
FlatButton(g, x, y, w, h, text, fill, edge, cb) {
    f := g.Add("Text", "x" x " y" y " w" w " h" h " Background" edge)
    b := g.Add("Text", "x" (x + 1) " y" (y + 1) " w" (w - 2) " h" (h - 2)
        " Background" fill " c202B3A +0x200 Center", text)
    f.OnEvent("Click", (*) => cb())
    b.OnEvent("Click", (*) => cb())
    return b
}
```

- **所有按钮同一套配色**：`edge = FRAME`、`fill = SOFT`，**不分主次**。中蓝描边 + 浅蓝底
  配近黑字会发闷、显脏（K26）。主次交给**位置**（收尾动作、常用动作靠右，离鼠标近）和文案。
- **外块也要挂 `Click`**：只挂内块时，点在 1px 描边上没反应，手感像坏了。
- **不做悬停变色**：自绘色块的重绘时机不受控，四条路全堵（K28）。别在这上面再花时间。
- `OnEvent("Click")` 依赖 AHK 注册时自动加的 `SS_NOTIFY`；**不要**手写 `GWL` 样式位（K1）。

**扁平勾选框：三层 `Text` 色块自绘，不用系统 `CheckBox`**

系统 `CheckBox` 的原生外观给不出封版色值，样式位也改不掉（K16），一律自绘。

```ahk
; 系统 CheckBox 的原生外观不受封版色值控制，样式位也去不掉，改成三层 Text 色块自绘；
; Value 保持 0/1 语义，方框与文字都能点击切换。类声明是运行期语句、不会被提升，
; 必须写在 BuildGui() 调用之前，否则加载时报「This line will never execute」。
class FlatCheck {
    __New(g, x, y, w, h, text, checked := false) {
        this.Value := checked ? 1 : 0
        this.cb := 0
        boxY := y + (h - 16) // 2
        this.frame := g.Add("Text", "x" x " y" boxY " w16 h16 BackgroundB7C8E0")
        this.fill := g.Add("Text", "x" (x + 1) " y" (boxY + 1) " w14 h14 +0x200 Center", "")
        ; 14px 小方块里 s10 的「√」只有 1px 细线、看不出形状；实测 ✓ + s11 Bold 笔画约 2px，
        ; 与系统勾选框观感最接近，且不溢出方块（✔ U+2714 会糊成一团）。
        this.fill.SetFont("s11 Bold", "Microsoft YaHei UI")
        this.label := g.Add("Text", "x" (x + 22) " y" y " w" (w - 22) " h" h
            " BackgroundF7F9FC c202B3A +0x200", text)
        this.Paint()
        for c in [this.frame, this.fill, this.label]
            c.OnEvent("Click", (*) => this.Toggle())
    }
    ; 浅色方案下「勾上」只表态、不抢眼：白块 -> 次按钮底（#EBF2FB），勾仍是近黑。
    ; 选中的色块不再比周围按钮深，勾形本身才是状态标识。
    Paint() {
        if (this.Value) {
            this.fill.Opt("BackgroundEBF2FB c202B3A")
            this.fill.Value := "✓"
        } else {
            this.fill.Opt("BackgroundFFFFFF c202B3A")
            this.fill.Value := ""
        }
    }
    Toggle() {
        this.Value := !this.Value
        this.Paint()
        if (this.cb)
            this.cb()
    }
    OnEvent(name, cb) {
        if (name = "Click")
            this.cb := cb
    }
}
```

用法 `chkRec := FlatCheck(gGui, 16, 344, 168, 28, "包含子文件（夹）", false)`，
之后 `chkRec.Value`、`chkRec.OnEvent("Click", cb)` 与原生 `CheckBox` 完全一致。

- 尺寸固定：外框 16×16、内块 14×14（各缩进 1px）、标签从 `x+22` 起（框 16 + `CELL_PAD` 6）。
- `boxY := y + (h - 16) // 2` 自动对中，不要手写偏移。实测文字墨迹与方块中心同为
  85.5（h26）/ 388.5（h28）。
- 选中态**不加深**：内块换 `SOFT`，只多一个勾形。比按钮还深的选中块会在三行竖排时连成一串，
  抢过按钮的注意力。

**扁平输入框 + 真正垂直居中**

```ahk
g.Add("Text", "x16 y12 w456 h28 BackgroundB7C8E0")
gPathEdit := g.Add("Edit", "x17 y13 w454 h26 -E0x200 Multi -VScroll -WantReturn -Wrap BackgroundFFFFFF c202B3A", "")
SetEditRect(gPathEdit.Hwnd, 8, 2, 448, 26)
```

```ahk
SetEditRect(hwnd, left, top, right, bottom) {
    rc := Buffer(16, 0)
    NumPut("Int", left, rc, 0)
    NumPut("Int", top, rc, 4)
    NumPut("Int", right, rc, 8)
    NumPut("Int", bottom, rc, 12)
    DllCall("user32\SendMessageW", "Ptr", hwnd, "UInt", 0xB3, "Ptr", 0, "Ptr", rc, "Ptr")
}
```

- 上例是 **672 窗**（输入行宽 640，输入框 w456）；**420 小窗**把 `w456` 换成 `w388`、
  `w454` 换成 `w386`、`setEditRect` 的 `right` 换成 `380`（= w − 8）。
- `left/right/top/bottom` 相对**控件客户区**，`right` 取 `w − 8`。
- **单行 `Edit` 不能靠 `EM_SETRECT` 居中**（`0xB3` 对单行无效），必须写成
  `Multi -VScroll -WantReturn -Wrap` 凑出 `ES_MULTILINE`，再设 `top=2`。
- `Multi` 会自动附 `WS_VSCROLL | ES_WANTRETURN | ES_AUTOVSCROLL`，必须 `-VScroll -WantReturn`
  去掉，`-Wrap` 加回 `ES_AUTOHSCROLL`；实测样式 `0x500100C4`，回车不插换行、长路径单行横滚。
- `top=2` **只对 `s10` + h26 实测有效**（墨迹中心 57.0 / 框与按钮中心 56.5，上下留白 8/7）。
- `-E0x200` 抵消 AHK 默认给 `Edit`/`ListView` 加的 `WS_EX_CLIENTEDGE`（K5），别用 `+Border`。

**带标题的列表区（整体框）+ 自绘列头**

```ahk
; 路径区是一个整体：外框 #B7C8E0 从标题带顶边一直包到列表底边（y132..338），
; 标题带与列表内缩 1px 落进框里。原先标题带是独立的一条浅蓝、列表自带一圈外框，
; 两条边不共用，看着像「一块色条压在一个框上」而不是同一个区块。
gGui.SetFont("s10 Bold", "Microsoft YaHei UI")
gGui.Add("Text", "x16 y132 w388 h206 BackgroundB7C8E0")
gGui.Add("Text", "x17 y133 w386 h26 BackgroundDCE8F7")
gGui.Add("Text", "x23 y133 w380 h26 BackgroundDCE8F7 c202B3A +0x200", "路径")
gGui.SetFont("s10 Norm", "Microsoft YaHei UI")

; AHK 默认给 ListView 加 WS_EX_CLIENTEDGE，经典样式下就是 3D 凹陷；-E0x200 抵消。
; 列表与外框内缘同 x/w，靠父块露出的 1px 当边框；顶边紧贴标题带底，不要留缝。
gList := gGui.Add("ListView", "x17 y159 w386 h178 -Hdr -E0x200 BackgroundFFFFFF c202B3A", ["路径"])
gList.ModifyCol(1, 600)
gScroll := FlatScroll(gGui, gList, 17, 159, 386, 178)
```

- 外框高 = 标题带 26 + 列表高 + 2（上下各 1px 线）。上例 `26 + 178 + 2 = 206`。
- 标题带与列表**同宽**（386 = 388 − 2），文字块再比内缘缩进 `CELL_PAD`（17 → 23）。
- 多列时列头是独立的 `Text` 色块拼出来的，每个列头块 `x = 列起点 + CELL_PAD`、
  `h = 26`、`BackgroundDCE8F7`；列头行整体再用 `s10 Bold` 包住，写完立刻还原 `Norm`。
- `-Hdr` 去掉原生列头（原生列头的外观不受封版口径控制，还能拖宽）。列表顶边与列头直接相接，
  中间不画横线 —— 相接处的 `BAND` / `WHITE` 分界就是线。

**扁平滚动条（自绘，顶掉 ListView 的原生滚动条）**

```ahk
gScroll := FlatScroll(gGui, gList, 17, 159, 386, 178)   ; 后四个数 = 列表可用内空
; …Show() 之后…
gScroll.Layout()
SetTimer(ObjBindMethod(gScroll, "Sync"), 60)
```

原生滚动条由 comctl32 主题引擎绘制：`WM_CTLCOLORSCROLLBAR` 实测一次都不触发，
`SetWindowTheme(lv,"DarkMode_Explorer","")` 只换浅/深两套外观，给不了任意颜色。所以整条自绘。

固定参数（不要改，除非重新做一轮像素实证）：

| 参数 | 值 | 含义 |
|---|---:|---|
| 槽厚 `t` | 10 | 轨道整条宽度，色 `BAND` |
| 滑块厚 `tn` | 4 | `t − 6`，居中，两侧各露 3px 槽 |
| 滑块色 | `ACCENT` | 唯一允许的中等深度 |
| 端部留白 `pad` | 2 | 滑块行程两端各让 2px |
| 同步周期 | 60ms | 只查样式位 + 兜原生条 |
| 拖动周期 | 20ms | 轮询 `GetCursorPos` + `GetKeyState("LButton","P")` |

- **隐藏**用 `ShowScrollBar(lv, SB_VERT, false)`，**不要** `-VScroll`：去掉样式后控件不再维护
  滚动范围，只隐藏则范围与滚动都保留。
- **隐藏不是一次性的**（K22）：`Move`/`Add`/`Delete`/`ModifyCol`/`LVM_SCROLL` 每次都会把
  `WS_HSCROLL`/`WS_VSCROLL` 置回来，原生条会和自绘条**同时出现**。这些动作之后都要再
  `ShowScrollBar(...false)` 一次，`Sync` 每拍按样式位（`GWL_STYLE & 0x00300000`）兜底。
- **读**范围 `GetScrollInfo`（竖=行，横=像素）；滑块 `len = 轨道长 × page / (max−min+1)`，
  下限 24px；起点 `frac = pos / (max−page+1)`。
- **滚**统一走 `LVM_SCROLL(0x1014)` 像素增量（竖 = Δ行 × 行高，行高由 `LVM_GETITEMRECT` 取），
  滚轮、点槽、拖滑块都汇到这一条通路。
- **占位**跟原生一致：内容超出才缩列表，最多三轮收敛。
- **一次重排的所有色块必须批量提交**（K24）：`BeginDeferWindowPos` → `DeferWindowPos` ×N
  （`SWP_NOZORDER|SWP_NOACTIVATE`，显隐用 `SWP_SHOWWINDOW`/`SWP_HIDEWINDOW`）→ `EndDeferWindowPos`。
- 轨道拆「滑块前 / 后两段」+ 四条同宽细缝（`vGapL/vGapR/hGapT/hGapB`）+ 角落方块，
  几何上**永不重叠** —— `SetWindowPos(HWND_TOP)` 抬不起 z 序（K20）。
- 色块必须 `+0x100`（`SS_NOTIFY`），否则命中测试透明、点击穿透到父窗口（K19）。
- 滑块两端各让 `pad=2`，但**槽不要跟着缩** —— 缩了两端会露父窗口底色，像被咬掉一口。
  `Paint` 与 `BeginDrag` 必须共用同一个 `TrackLen()`。
- **0 行时不出横条**：空列表的横向溢出只是列宽造成的，画出来就是一条居中空滑块。
  清空列表时把横向位置归零。
- 关闭窗口时**先 `Stop()` 再 `Destroy()`**，否则下一拍报「The control is destroyed」（K21）。

完整实现（拖动、翻页、滚轮转发、定时同步、每次动作后补隐藏）见
`文件时间修改` **2.4.6** 的 `class FlatScroll`，整段复制即可。

> **2.4.6 起这份配方换了内核**（K29/K30）：不再依赖原生滚动条的范围 —— 摘掉
> `WS_VSCROLL` 之后 ListView 既不响应滚轮也不响应翻页键。改为类内自己维护
> 行数 / 可视行数 / 首行号，滚动走 `LVM_ENSUREVISIBLE`，滚轮与翻页键由脚本侧
> `OnMessage(0x20A)` / `OnMessage(0x100)` 接管；横向条撤掉、列宽做进视口。
> 复制时连同脚本侧的 `OnBarWheel` / `OnBarKey` 一起复制，别只抄类。

**批量填表（几百行以上）：填表期间把列表整个藏起来**

`WM_SETREDRAW` 只压得住客户端绘制，**压不住非客户区**：行一多 comctl32 就把
`WS_VSCROLL` 置回并当场把原生条刷出来；竖条让客户区窄 16px，按整幅宽算好的列宽立刻
溢出，横条跟着再冒一条 —— 屏幕上就是「空列表 + 两条原生灰条」（K36）。而填表本身是
几百毫秒的长任务，定时器挤不进来（K38）。唯一干净的做法是让窗口在这段时间里**一个像素都不画**：

```ahk
FillList() {
    hadFocus := S.gui.FocusedCtrl && S.gui.FocusedCtrl.Hwnd = S.lv.Hwnd
    DllCall("user32\ShowWindow", "Ptr", S.lv.Hwnd, "Int", 0)   ; SW_HIDE
    SendMessage(0x100B, 0, 0, S.lv.Hwnd)                       ; WM_SETREDRAW 0
    ; …Delete / Add / LVIF_PARAM / LVM_ENSUREVISIBLE / Focus 全在这段里做…
    gScroll.HideNative()                                        ; 摘原生条
    SendMessage(0x100B, 1, 0, S.lv.Hwnd)
    DllCall("user32\ShowWindow", "Ptr", S.lv.Hwnd, "Int", 5)    ; SW_SHOW
    if (hadFocus || !S.gui.FocusedCtrl)
        S.lv.Focus()                                            ; 隐藏会丢焦点（K39）
    gScroll.Sync()                                              ; 重算占位 + 画自绘条
    S.lv.Redraw()
}
```

顺带的好处：这段时间不产生任何绘制，填表本身也会快一些（少一次整表重绘）。
**用户看得见的是干净白底**（父块 `lvFrame` 的白），比原来「灰底 + 两条灰条」体面得多。
列表行数少（<200）时不必这么绕，直接 `WM_SETREDRAW` 就够了。

**扁平日期 / 时间框：裁掉按钮区 + 自绘按钮 + 消息转发**

`SysDateTimePick32` 的右侧按钮区是**控件内绘**的：经典模式画 3D 凸起按钮（124..139），
主题模式在字段内画日历小图标（116..121）。样式位和主题都改不掉，唯一干净的办法是把控件
**裁到只剩文字区**（裁剪点取两者左侧 115），按钮改由 `Text` 色块承担，点击按控件自己的
按钮坐标转发回去。

```ahk
ClipField(hwnd, right, h) {
    rgn := DllCall("gdi32\CreateRectRgn", "Int", 2, "Int", 2, "Int", right, "Int", h - 2, "Ptr")
    if (rgn && !DllCall("user32\SetWindowRgn", "Ptr", hwnd, "Ptr", rgn, "Int", 1, "Int"))
        DllCall("gdi32\DeleteObject", "Ptr", rgn)
}

; 日期框：裁到 115 —— 有主题时 DTP 会在 116..121 自绘一个日历小图标，必须整块裁掉，
; 否则会看到「图标 + 三角」两个重复入口。115..121 用白块补齐当右内边距，
; 122 画分隔线，123..139 才是 ▼ 色块，按钮宽度在两种环境下保持一致（17px）。
MakeDateField(g, x, y) {
    g.Add("Text", "x" x " y" y " w140 h26 BackgroundB7C8E0")
    c := g.Add("DateTime", "x" (x - 1) " y" (y - 1) " w142 h28 -0x800000", "yyyy/MM/dd")
    ClipField(c.Hwnd, 115, 28)
    g.Add("Text", "x" (x + 114) " y" (y + 1) " w7 h24 BackgroundFFFFFF")
    g.Add("Text", "x" (x + 121) " y" (y + 1) " w1 h24 BackgroundB7C8E0")
    b := g.Add("Text", "x" (x + 122) " y" (y + 1) " w17 h24 BackgroundFFFFFF c202B3A +0x200 Center", "▼")
    b.SetFont("s9", "Microsoft YaHei UI")
    b.OnEvent("Click", (*) => PopCalendar(c))
    return c
}

; 时分秒框：134 宽控件裁到 115，115..131 交给自绘的 ▲▼ 两块。
MakeTimeField(g, x, y) {
    g.Add("Text", "x" x " y" y " w132 h26 BackgroundB7C8E0")
    c := g.Add("DateTime", "x" (x - 1) " y" (y - 1) " w134 h28 -0x800000 +0x1", "HH:mm:ss")
    ClipField(c.Hwnd, 115, 28)
    g.Add("Text", "x" (x + 114) " y" (y + 1) " w1 h24 BackgroundB7C8E0")
    up := g.Add("Text", "x" (x + 115) " y" (y + 1) " w16 h11 BackgroundFFFFFF c202B3A +0x200 Center", "▲")
    dn := g.Add("Text", "x" (x + 115) " y" (y + 12) " w16 h13 BackgroundFFFFFF c202B3A +0x200 Center", "▼")
    up.SetFont("s9", "Microsoft YaHei UI")
    dn.SetFont("s9", "Microsoft YaHei UI")
    up.OnEvent("Click", (*) => StepTime(c, 0x26))
    dn.OnEvent("Click", (*) => StepTime(c, 0x28))
    return c
}

; 必须 Post 而不是 SendMessage：日历下拉会在控件内部跑嵌套消息循环，
; 同步发送会把脚本卡在那个循环里，直到用户关掉日历才返回。
PopCalendar(ctrl) {
    ctrl.Focus()
    l := (14 << 16) | 131
    DllCall("user32\PostMessageW", "Ptr", ctrl.Hwnd, "UInt", 0x201, "Ptr", 1, "Ptr", l)
    DllCall("user32\PostMessageW", "Ptr", ctrl.Hwnd, "UInt", 0x202, "Ptr", 0, "Ptr", l)
}

; 上下键沿用控件自己的语义（改当前选中的那一段），不自己算时间、不覆盖用户的输入游标。
StepTime(ctrl, vk) {
    ctrl.Focus()
    DllCall("user32\PostMessageW", "Ptr", ctrl.Hwnd, "UInt", 0x100, "Ptr", vk, "Ptr", 0)
    DllCall("user32\PostMessageW", "Ptr", ctrl.Hwnd, "UInt", 0x101, "Ptr", vk, "Ptr", 0)
}
```

- **必须发消息、不要自己算日期**：`ctrl.Value` 是 14 位串，自己加减会踩月末 / 闰年；
  `VK_UP` / `VK_DOWN` 与日历弹出都走控件原生逻辑。
- **转发一律 `PostMessageW`**：日历下拉是模态的，它开着时脚本语句不往下走（宿主窗口仍响应），
  选完即续，与原生一致；`SendMessage` 会把调用栈压进那个模态循环，实测更难恢复。
- 裁剪点必须取两种模式的**更左者**（115），否则用户独立运行时能看到「日历图标 + 自绘三角」
  两个重复入口。
- `-0x800000` 去 `WS_BORDER`，`+0x1` = `DTS_UPDOWN`（时分秒框不要日历下拉）。
- **自绘色块盖不住控件内绘**：后创建也会被控件盖住，只能先裁再补画。

**信息行（只放事实，不放说明文案）**

```ahk
gInfo := g.Add("Text", "x16 y48 w640 h24 BackgroundF7F9FC c202B3A +0x200", "未选择文件")
```

内容只写算完才有的事实（文件名 · 大小 · 算法数）；不重复窗口标题，不写操作说明。

**状态条 + 系统标题栏染色**

```ahk
gGui.Add("Text", "x0 y378 w420 h30 BackgroundDCE8F7")
gStatus := gGui.Add("Text", "x12 y378 w396 h30 BackgroundDCE8F7 c202B3A +0x200", "")
gGui.Show("w420 h408 Center")
PaintCaption()

PaintCaption() {
    global gGui
    c := Buffer(4, 0)
    NumPut("UInt", 0x00F7E8DC, c)        ; #DCE8F7 → COLORREF
    t := Buffer(4, 0)
    NumPut("UInt", 0x3A2B20, t)          ; #202B3A → COLORREF
    try {
        DllCall("dwmapi.dll\DwmSetWindowAttribute", "Ptr", gGui.Hwnd, "UInt", 35, "Ptr", c, "UInt", 4)
        DllCall("dwmapi.dll\DwmSetWindowAttribute", "Ptr", gGui.Hwnd, "UInt", 36, "Ptr", t, "UInt", 4)
        DllCall("dwmapi.dll\DwmSetWindowAttribute", "Ptr", gGui.Hwnd, "UInt", 34, "Ptr", c, "UInt", 4)
    }
}
```

- 状态条**自己铺底块**（`x0 w客户区`）而不是用原生 `StatusBar`：原生条在 Win11 下
  `Background` 要 `-Theme` 才生效、`c` 文字色无效，还会带自己的一条分隔线（K15）。
- 状态条文字与底块**同色同高**、只差 `STATUS_PAD` 内缩；用 `Text` 的 `.Value` 更新（没有 `.SetText`）。
- `PaintCaption()` 必须在 `Show()` **之后**调用（窗口未创建时 DWM 属性无效），
  且要在 `try` 里 —— Win10 与关闭进程主题时静默降级，不能因此报错。
- DWM 属性：35 底色 / 36 标题文字 / 34 边框；**只写不读**（读返回 `E_INVALIDARG`），
  验证只能看截图像素（K11）。
- **不画自绘标题带**：窗口标题已有名字，再叠一条带说明的蓝底属冗余。

---

## 10. 组件选型速查

| 要什么 | 用什么 | 配方位置 | 高频坑 |
|---|---|---|---|
| 按钮 | Text 两层色块 | 本文附录 | K19/K25/K26/K28/K32 |
| 勾选框 | FlatCheck | 本文附录 | K16 |
| 输入框 | Multi+裁边+SETRECT | 本文附录 | K5/K6 |
| 列表 | ListView+外框+自绘列头/滚动条 | 本文附录 | K7/K27/K29–K31/K34–K36/K39/K42–K50 |
| 日期时间 | ClipField+Text 按钮 | 本文附录 | K14/K17 |
| 滑块 | FlatTrack | 夜灯源码 | K20/K23/K24/K33 |
| 右键菜单 | Menu.Show() 无参 | 硬件信息源码 | K45/K46 |
| 多页导航 | BuildNav + FlatScroll | 硬件信息源码 | K43/K47 |
| 状态条 | Text 色块（h30） | 本文附录 | K15 |

做完任何窗口，过一遍 `UI规范.md` §10.11 自检清单（踩坑证据查 [`UI踩坑.md`](UI踩坑.md)）；验证与取证方法见
[`脚本开发流程.md`](脚本开发流程.md)。
