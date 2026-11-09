# check_empty_state.ps1 - the empty script list must say what to do, and Scripts\
# must be able to come back.
#
# Why this exists (m125): the deck ships an EMPTY Scripts folder now - no sample
# script, no menu bridge. That makes "no scripts" the FIRST-RUN state, and the
# list used to be a blank pane that never mentioned where .ahk files belong.
# Two things are easy to lose again and invisible to the compiler:
#
#   1. the hint itself, and the fact that it distinguishes "there are no scripts
#      at all" from "the filter hid every row". The browser's size() cannot tell
#      those apart, so the call site must pass ScriptCount() - text, not pixels.
#   2. ScanScripts() creating <exe>\Scripts before it scans. Without it a zip
#      that dropped the empty directory leaves the list empty forever and
#      File > Open scripts folder does nothing.
#
# Usage:  powershell -File check_empty_state.ps1 [-Root <dir>] [-SelfTest]
# Exit 0 = clean, 1 = a violation (or the gate itself stopped reading).

param(
    [string]$Root = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

function Find-Violations([string]$text) {
    $hits = @()

    # The call site: guarded by "no rows", argument is the SCRIPT COUNT.
    if ($text -notmatch 'if\s*\(size\(\)\s*==\s*0\)\s*[\r\n\s]*DrawEmptyHint\(mOwner->ScriptCount\(\)\s*==\s*0\)') {
        $hits += "ScriptBrowser::draw no longer calls DrawEmptyHint(mOwner->ScriptCount() == 0) inside if (size() == 0)"
    }

    # Both sentences: an empty deck and an empty filter are different states.
    if ($text -notmatch [regex]::Escape('还没有脚本')) {
        $hits += "the first-run sentence ('还没有脚本') is gone"
    }
    if ($text -notmatch [regex]::Escape('没有匹配的脚本')) {
        $hits += "the filtered-out sentence ('没有匹配的脚本') is gone"
    }

    # The folder has to exist before the scan, or a dropped empty directory wins.
    if ($text -notmatch 'CreateDirectoryW\(scriptsDir\.c_str\(\)') {
        $hits += "ScanScripts no longer creates <exe>\Scripts before scanning"
    }

    return $hits
}

if ($SelfTest) {
    # The real file, then the same file with each guard removed by hand.
    $here = Split-Path $MyInvocation.MyCommand.Path -Parent
    $probe = @"
	void draw() {
		if (size() == 0)
			DrawEmptyHint(mOwner->ScriptCount() == 0);
	}
	void DrawEmptyHint(bool aNoScriptsAtAll) {
		fl_draw("还没有脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
		fl_draw("没有匹配的脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
	}
	void ScanScripts() { CreateDirectoryW(scriptsDir.c_str(), nullptr); }
"@
    if ((Find-Violations $probe).Count -ne 0) {
        Write-Output "selftest: the good sample was flagged"
        exit 1
    }

    $badCount = @"
	void draw() {
		if (size() == 0)
			DrawEmptyHint(size() == 0);
	}
	void DrawEmptyHint(bool aNoScriptsAtAll) {
		fl_draw("还没有脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
		fl_draw("没有匹配的脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
	}
	void ScanScripts() { CreateDirectoryW(scriptsDir.c_str(), nullptr); }
"@
    if (@(Find-Violations $badCount).Count -lt 1) {
        Write-Output "selftest: size() instead of ScriptCount() was NOT caught"
        exit 1
    }

    $badNoDir = @"
	void draw() {
		if (size() == 0)
			DrawEmptyHint(mOwner->ScriptCount() == 0);
	}
	void DrawEmptyHint(bool aNoScriptsAtAll) {
		fl_draw("还没有脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
		fl_draw("没有匹配的脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
	}
	void ScanScripts() { FindFirstFileW(pattern.c_str(), &fd); }
"@
    if (@(Find-Violations $badNoDir).Count -lt 1) {
        Write-Output "selftest: a scan without creating the folder was NOT caught"
        exit 1
    }

    $badOneSentence = @"
	void draw() {
		if (size() == 0)
			DrawEmptyHint(mOwner->ScriptCount() == 0);
	}
	void DrawEmptyHint(bool aNoScriptsAtAll) {
		fl_draw("没有匹配的脚本", 0, 0, 1, 1, FL_ALIGN_CENTER);
	}
	void ScanScripts() { CreateDirectoryW(scriptsDir.c_str(), nullptr); }
"@
    if (@(Find-Violations $badOneSentence).Count -lt 1) {
        Write-Output "selftest: a hint with only one sentence was NOT caught"
        exit 1
    }

    Write-Output "selftest: good sample clean, three bad samples caught"
    exit 0
}

if ([string]::IsNullOrEmpty($Root)) {
    $Root = Split-Path (Split-Path (Split-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) -Parent) -Parent) -Parent
}

$files = @(Get-ChildItem -Path (Join-Path $Root ".main\ui") -Filter "*.cpp" -File -ErrorAction SilentlyContinue)
if ($files.Count -eq 0) {
    Write-Output "empty state: NO SOURCE FILES SCANNED - the gate is blind"
    exit 1
}

$seen = 0
$violations = @()
foreach ($f in $files) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    if ($text -notmatch 'DrawEmptyHint') { continue }
    $seen++
    foreach ($hit in @(Find-Violations $text)) {
        $violations += "$($f.Name): $hit"
    }
}

if ($seen -eq 0) {
    Write-Output "empty state: scanned $($files.Count) file(s) and found no DrawEmptyHint - the gate is blind"
    exit 1
}
if ($violations.Count -gt 0) {
    Write-Output "empty state: $($violations.Count) violation(s)"
    foreach ($v in $violations) { Write-Output "  - $v" }
    exit 1
}

Write-Output "empty state: OK (hint driven by ScriptCount, both sentences present, Scripts created before scan)"
exit 0
