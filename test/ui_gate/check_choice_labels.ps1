# check_choice_labels.ps1 - no Fl_Choice may be filled with a "\n"-separated string.
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
        if ($line -notmatch '->add\("') { continue }
        $literal = [regex]::Match($line, '->add\((".*?")')
        if (-not $literal.Success) { continue }
        if ($literal.Groups[1].Value.Contains('\n')) {
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

# Count the add() calls the rule governs: if this is 0 the pattern no longer
# matches anything the product writes, which is also a blind gate.
$scanned = 0
$violations = @()
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    $scanned += ([regex]::Matches($text, '->add\("')).Count
    foreach ($hit in @(Find-Violations $text)) {
        $violations += "$($f.Name): $hit"
    }
}

if ($scanned -eq 0) {
    Write-Output "choice labels: scanned $($files.Count) file(s) and found 0 add() calls - the gate is blind"
    exit 1
}

if ($violations.Count -gt 0) {
    Write-Output "choice labels: $($violations.Count) violation(s) - use '|' to separate Fl_Choice items"
    foreach ($v in $violations) { Write-Output "  - $v" }
    exit 1
}

Write-Output "choice labels: OK ($scanned add() call(s) in $($files.Count) file(s), none newline-separated)"
exit 0
