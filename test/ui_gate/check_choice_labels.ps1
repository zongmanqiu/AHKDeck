# check_choice_labels.ps1 - no dropdown may be filled with a "\n"-separated string.
#
# Why this exists (2026-11-04, m024): Fl_Menu_::add(const char*) splits a string
# into items at every '|' - a newline is NOT a separator (Fl_Menu_add.cxx:445).
# So add("A\nB\nC") creates ONE item whose label happens to contain three lines:
# value() can never select the 2nd or 3rd entry (Fl_Choice::value() rejects
# v >= size()-1), the callback reports -1, and the box just draws the middle line
# of that single label. Two widgets were written that way - the new sort choice
# and, since m015, the encoding choice - and both looked fine on screen.
#
# The pattern is invisible to the compiler and to every runtime gate that does not
# actually drive the dropdown, so it gets its own text gate.
#
# m088 (2026-11-07): the deck's menu is no longer built with literal
# mMenu->add("...") calls - it comes from one command table, and the table is
# what the shortcut dialog lists. That removed the last literal add() calls, so
# the old "the gate is blind" self-check fired even though nothing regressed.
# The gate now also reads the table form, which is where the deck's menu labels
# live now:
#   {IdNew, "new", "文件/新建", "菜单", "新建", FL_CTRL + 'n', 0, false},
# A path there must use "/" for the sub-menu and must not carry a newline or a
# '|', for the same reason as above.
#
# Usage:  powershell -File check_choice_labels.ps1 [-Root <dir>] [-SelfTest]
# Exit 0 = clean, 1 = a violation (or the gate itself stopped reading).

param(
    [string]$Root = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

function Find-Violations([string]$text) {
    # One add() call per match; a literal may be split across adjacent strings.
    $hits = @()
    foreach ($line in ($text -split "`n")) {
        if ($line -notmatch '->add\(') { continue }
        # The argument list of this add() call, up to the closing paren.
        $call = [regex]::Match($line, '->add\((.*)')
        if (-not $call.Success) { continue }
        if ($call.Groups[1].Value.Contains('\n')) {
            $hits += , $line.Trim()
        }
    }
    return $hits
}

function Find-TableViolations([string]$text) {
    # The command table's path field: the 3rd field of a row.
    $hits = @()
    foreach ($line in ($text -split "`n")) {
        $row = [regex]::Match($line, '\{Id\w+\s*,\s*"[^"]*"\s*,\s*"([^"]*)"')
        if (-not $row.Success) { continue }
        $path = $row.Groups[1].Value
        if ($path.Contains('\n') -or $path.Contains('|')) {
            $hits += , $line.Trim()
        }
    }
    return $hits
}

if ($SelfTest) {
    $bad = '	mEncoding->add("UTF-8\nUTF-8 BOM\nUTF-16LE");'
    $good = '	mEncoding->add("UTF-8|UTF-8 BOM|UTF-16LE");'
    $caught = @(Find-Violations $bad)
    $missed = @(Find-Violations $good)
    if ($caught.Count -ne 1) {
        Write-Output "selftest: the bad sample was NOT caught ($($caught.Count) hits)"
        exit 1
    }
    if ($missed.Count -ne 0) {
        Write-Output "selftest: the good sample was flagged ($($missed.Count) hits)"
        exit 1
    }
    # m088: the table form, and the literal form it replaced.
    $badTable = '		{IdNew, "new", "文件/新建|另一个", "菜单", "新建", FL_CTRL + ''n'', 0, false},'
    $goodTable = '		{IdNew, "new", "文件/新建", "菜单", "新建", FL_CTRL + ''n'', 0, false},'
    $caughtTable = @(Find-TableViolations $badTable)
    $missedTable = @(Find-TableViolations $goodTable)
    if ($caughtTable.Count -ne 1) {
        Write-Output "selftest: the bad table row was NOT caught ($($caughtTable.Count) hits)"
        exit 1
    }
    if ($missedTable.Count -ne 0) {
        Write-Output "selftest: the good table row was flagged ($($missedTable.Count) hits)"
        exit 1
    }
    Write-Output "selftest: bad sample caught, good sample clean"
    exit 0
}

if ([string]::IsNullOrEmpty($Root)) {
    $Root = Split-Path (Split-Path (Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent) -Parent) -Parent
}

$files = @(Get-ChildItem -Path (Join-Path $Root ".main\ui") -Filter "*.cpp" -File -ErrorAction SilentlyContinue)
if ($files.Count -eq 0) {
    Write-Output "choice labels: NO SOURCE FILES SCANNED - the gate is blind"
    exit 1
}

# Count the calls and the table rows the rule governs: if both are 0 the
# pattern no longer matches anything the product writes, which is also a blind
# gate.
$scanned = 0
$rows = 0
$violations = @()
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    $scanned += ([regex]::Matches($text, '->add\(')).Count
    $rows += ([regex]::Matches($text, '\{Id\w+\s*,\s*"[^"]*"\s*,\s*"')).Count
    foreach ($hit in @(Find-Violations $text)) {
        $violations += "$($f.Name): $hit"
    }
    foreach ($hit in @(Find-TableViolations $text)) {
        $violations += "$($f.Name): $hit"
    }
}

if ($scanned -eq 0 -and $rows -eq 0) {
    Write-Output "choice labels: scanned $($files.Count) file(s) and found 0 add() calls and 0 table rows - the gate is blind"
    exit 1
}

if ($violations.Count -gt 0) {
    Write-Output "choice labels: $($violations.Count) violation(s) - use '|' to separate Fl_Choice items"
    foreach ($v in $violations) { Write-Output "  - $v" }
    exit 1
}

Write-Output "choice labels: OK ($scanned add() call(s), $rows table row(s) in $($files.Count) file(s), none newline-separated)"
exit 0
