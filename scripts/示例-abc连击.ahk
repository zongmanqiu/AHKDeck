#Requires AutoHotkey v2.0
#UseHook

; ============================================================================
; 示例-abc连击.ahk —— 实时监听：连续按 a → b → c（相邻两下间隔 ≤ 1 秒），
; 立刻往当前窗口输入 1234567890。常驻；关宿主即停。
;
; 带参数启动＝自测模式，不装钩子、不输入任何键：
;     AHKDeck.exe --run 示例-abc连击.ahk selftest
;   退出码 0＝判序全过；3＝有断言失败（原因写进 示例-abc连击自测失败.txt）
; ============================================================================

GapLimit := 1000 ; 相邻两下的最大间隔（毫秒）
SelfEcho := 200  ; fire 之后这段时间内收到的键视为自己发出的

; ---- 自测模式：先于钩子返回 -----------------------------------------------
if (A_Args.Length > 0 && A_Args[1] = "selftest") {
    fail := SeqSelfTest()
    f := A_ScriptDir "\示例-abc连击自测失败.txt"
    if (fail != "") {
        h := FileOpen(f, "w", "UTF-8")
        h.Write(fail)
        h.Close()
        ExitApp 3
    }
    if FileExist(f)
        FileDelete f
    ExitApp 0
}

; ---- 序列状态 -------------------------------------------------------------
seqCount := 0  ; 已连上的下数：0 空 / 1 收到 a / 2 收到 ab
seqKey := ""   ; 上一下的键
seqTick := 0   ; 上一下的时刻
fireTick := 0  ; 上一次 fire 的时刻

; ---- 判序：时间戳由调用方给，所以能离线喂合成数据 --------------------------
SeqStep(key, tick, gap, echo) {
    global
    if (tick - fireTick < echo)
        return "reset" ; 刚发出去的那串数字，不算用户按的
    if (key = "a") {   ; a 永远是重来一遍
        seqKey := "a"
        seqTick := tick
        seqCount := 1
        return "count"
    }
    next := (seqCount = 1) ? "b" : (seqCount = 2) ? "c" : ""
    if (next = "" || key != next || tick - seqTick > gap) {
        seqKey := ""
        seqTick := 0
        seqCount := 0
        return "reset"
    }
    seqKey := key
    seqTick := tick
    seqCount += 1
    if (seqCount = 3) {
        seqKey := ""
        seqTick := 0
        seqCount := 0
        fireTick := tick
        return "fire"
    }
    return "count"
}

; ---- 每次按键都落一行：你能当场看到它在不在听、听到了什么 -------------------
Mark(what, key) {
    h := FileOpen(A_ScriptDir "\示例-abc连击记录.txt", "a", "UTF-8")
    h.Write(FormatTime(, "HH:mm:ss") "  " what "  " key "  tick=" A_TickCount "`r`n")
    h.Close()
}

SeqHit(key) {
    global
    r := SeqStep(key, A_TickCount, GapLimit, SelfEcho)
    Mark(r, key)
    if (r = "fire")
        SendText "1234567890"
}

; ---- 钩子本体：~ ＝ 放行，a/b/c 照常打进你正在用的窗口 ---------------------
~a:: SeqHit("a")
~b:: SeqHit("b")
~c:: SeqHit("c")

; ---- 常驻的凭据：装好钩子就落一行 ------------------------------------------
h := FileOpen(A_ScriptDir "\示例-abc连击在听.txt", "w", "UTF-8")
h.Write("钩子已装 " FormatTime(, "yyyy-MM-dd HH:mm:ss") "  间隔上限 " GapLimit " ms`r`n")
h.Close()

; ============================================================================
; 判序自测：合成时间戳，断言"该触发的触发、不该触发的一个都不触发"
; ============================================================================
ResetSeq() {
    global
    seqCount := 0
    seqKey := ""
    seqTick := 0
    fireTick := 0
}

SeqRun(pairs) {
    ResetSeq()
    out := ""
    t0 := 100000
    for kv in pairs {
        r := SeqStep(kv[1], t0 + kv[2], 1000, 200)
        out := out = "" ? r : out "," r
    }
    return out
}

NumFire(s) {
    n := 0
    pos := 1
    while (pos := InStr(s, "fire", true, pos)) {
        ++n
        pos += 4
    }
    return n
}

SeqSelfTest() {
    ; 1) 完美三连 ⇒ 恰好一次 fire
    r1 := SeqRun([["a", 0], ["b", 400], ["c", 800]])
    if (r1 != "count,count,fire")
        return "断言1失败：a→b→c 每下都 ≤1s，应 fire，实际 " r1 "`r`n"
    ; 2) 顺序对，但 b→c 超 1 秒 ⇒ 不许触发
    r2 := SeqRun([["a", 0], ["b", 400], ["c", 1900]])
    if (NumFire(r2) != 0)
        return "断言2失败：超间隔仍触发 " r2 "`r`n"
    ; 3) 顺序错（a→c→b）⇒ 不许触发
    r3 := SeqRun([["a", 0], ["c", 200], ["b", 400]])
    if (NumFire(r3) != 0)
        return "断言3失败：错序仍触发 " r3 "`r`n"
    ; 4) 中途重来（a,b,a,b,c）⇒ 只有最后一次 fire
    r4 := SeqRun([["a", 0], ["b", 200], ["a", 400], ["b", 600], ["c", 800]])
    if (NumFire(r4) != 1)
        return "断言4失败：应恰好 1 次 fire，实际 " r4 "`r`n"
    ; 5) fire 之后 200ms 内的按键视为自己发的，不许接着算第二轮
    r5 := SeqRun([["a", 0], ["b", 200], ["c", 400], ["a", 500]])
    if (NumFire(r5) != 1)
        return "断言5失败：fire 后回声应被忽略，实际 " r5 "`r`n"
    ; 6) 边界：正好 1000ms 算"以内"，1001ms 算超
    r6 := SeqRun([["a", 0], ["b", 1000], ["c", 2000]])
    if (NumFire(r6) != 1)
        return "断言6失败：1000ms 整应算以内，实际 " r6 "`r`n"
    r7 := SeqRun([["a", 0], ["b", 1001], ["c", 2002]])
    if (NumFire(r7) != 0)
        return "断言7失败：1001ms 应算超，实际 " r7 "`r`n"
    return ""
}
