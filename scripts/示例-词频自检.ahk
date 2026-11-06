#Requires AutoHotkey v2.0
; 示例-词频自检.ahk —— 词频统计 + 四组内部断言（给 AI 和人都能判"跑通"）
;
; 退出码：0＝四组断言全过；3＝有断言失败（哪一条写进 freq-fail.txt）；1＝环境失败（写不出文件）
; 为什么自带断言：AHKDeck 里脚本 FileAppend 到 stdout 的文字与 MsgBox 内容都不进控制台，
; 能机器可读地证明"它真做对了"的只有退出码 + 它写出的文件。
;
; v2 的三个坑（本文件三轮报错换来的）：
;   1. `=` / `<>` 不是比较运算符，只能用 `==` / `!=`；
;   2. `<` `>` 只比数字，比字符串会抛 "Expected a Number but got a String"
;      ⇒ 字典序交给不带回调的 Sort()，回调里只允许数字相减；
;   3. 表达式里任何一个函数名不存在，整行赋值失败，报的是"变量没赋值"而不是"函数不存在"
;      ⇒ 这里一个字符码函数都不用。

class Lexicon {
    static MinLen := 2
    static TopN := 5
    static Corpus := "The Quick brown fox jumps over the lazy dog, and the lazy dog sleeps. "
        . "A quick brown fox is not a slow cat; the slow cat naps while the dog sleeps. "
        . "Brown bread and butter, butter and jam, jam and tea: the tea is hot, the bread is brown."

    ; 只留字母与空格 → 小写 → 折叠空格 → 切词 → 丢弃长度 < MinLen
    static Tokenize(text) {
        cleaned := RegExReplace(text, "[^A-Za-z ]", " ")
        cleaned := StrLower(cleaned)
        cleaned := RegExReplace(cleaned, "[ ]+", " ")
        cleaned := Trim(cleaned, " ")
        words := []
        if cleaned == ""
            return words
        for piece in StrSplit(cleaned, " ")
            if StrLen(piece) >= Lexicon.MinLen
                words.Push(piece)
        return words
    }

    static Count(words) {
        tally := Map()
        for w in words
            tally[w] := tally.Has(w) ? tally[w] + 1 : 1
        return tally
    }

    ; 次数降序；并列按字典序。
    ; 字典序交给内置函数 Sort(串, 分隔符)：实测 Sort("brown`nand`ndog`nis", "`n")
    ; 返回 and/brown/dog/is。Array.Sort() 不带回调**不是**字典序（实测给出 and dog brown is），
    ; 回调里比字符串会抛，而 Asc/IsFunc 在 v2 已被删除。
    static Rank(tally) {
        byCount := Map()
        for w, n in tally {
            if !byCount.Has(n)
                byCount[n] := ""
            byCount[n] .= w . "`n"
        }
        counts := []
        for n, blob in byCount
            counts.Push(n)
        counts.Sort((a, b) => b - a)
        rows := []
        for c in counts
            for w in StrSplit(Sort(byCount[c], "`n"), "`n")
                if w != ""
                    rows.Push([w, c])
        return rows
    }

    static IsAscending(first, second) {
        return StrSplit(Sort(first . "`n" . second, "`n"), "`n")[1] == first
    }
}

scriptDir := A_ScriptDir
outPath := scriptDir "\freq-top5.txt"
failPath := scriptDir "\freq-fail.txt"

if FileExist(failPath)
    FileDelete(failPath)

words := Lexicon.Tokenize(Lexicon.Corpus)
tally := Lexicon.Count(words)
rows := Lexicon.Rank(tally)

if rows.Length == 0 {
    FileAppend("assert0: 语料切出 0 个词`r`n", failPath)
    ExitApp 3
}

lines := ""
for i, row in rows {
    if i > Lexicon.TopN
        break
    lines .= row[2] . "`t" . row[1] . "`r`n"
}

try {
    ; FileDelete 在文件不存在时会抛（错误 2），所以用 "w" 打开即截断
    handle := FileOpen(outPath, "w", "UTF-8")
    if !handle
        throw Error("FileOpen 返回空")
    handle.Write(lines)
    handle.Close()
} catch as e {
    FileAppend("assert-env: 写不出 " . outPath . " —— " . e.Message . "`r`n", failPath)
    ExitApp 1
}

; 断言一：Top-1 词频 ≥ 3
a1 := rows[1][2] >= 3

; 断言二：输出文件正好 TopN 行（逐字节回读，不数内存里的字符串）
body := FileExist(outPath) ? Trim(FileRead(outPath), "`r`n") : ""
a2 := body != "" && StrSplit(body, "`n").Length == Lexicon.TopN

; 断言三：空字符串走同一条分词路径，必须 0 词且不崩
a3 := false
try
    a3 := Lexicon.Tokenize("").Length == 0
catch
    a3 := false

; 断言四：次数相同的相邻词必须字典序升（第四轮就是缺这条，rc=0 却把 is 排在 dog 前面）
a4 := true
shown := rows.Length < Lexicon.TopN ? rows.Length : Lexicon.TopN
loop shown - 1 {
    cur := rows[A_Index]
    nxt := rows[A_Index + 1]
    if cur[2] == nxt[2] && !Lexicon.IsAscending(cur[1], nxt[1])
        a4 := false
}

if !(a1 && a2 && a3 && a4) {
    why := ""
    if !a1
        why .= "断言一失败：Top-1 是 " . rows[1][1] . " 次数 " . rows[1][2] . "（要求 ≥3）`r`n"
    if !a2
        why .= "断言二失败：" . outPath . " 行数不是 " . Lexicon.TopN . "`r`n"
    if !a3
        why .= "断言三失败：空字符串没有安全地切出 0 词`r`n"
    if !a4
        why .= "断言四失败：并列次数的词没有按字典序`r`n"
    FileAppend(why, failPath)
    ExitApp 3
}

ExitApp 0
