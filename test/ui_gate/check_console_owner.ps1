# check_console_owner.ps1 - text gate for the console's ownership rule (m067 ->
# m076), the shape of one run receipt (m075) and the row light (m068).
# ----------------------------------------------------------------------------
# m067: four separate filters used to hide the console, so a one-shot script
# printed nothing at all while every compile and ctest stayed green.
# m076 REVERSES part of that answer on the user's instruction: the console is the
# pane for correcting the script open in the editor, so running some OTHER script
# must not write into it (and must not claim it). That script's receipts and
# kernel records live in its own session JSONL, and the row light shows its
# result. What must NOT come back is the old "no file open => nothing printed"
# behaviour for the script that IS open in the editor.
# m068: the receipt also has to show in the ROW, where a finished one-shot used
# to fall back to the glyph a row that never ran shows.
# ----------------------------------------------------------------------------
param([switch]$SelfTest, [string]$Root = "")

$ErrorActionPreference = "Stop"
if ($Root -eq "") { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) | Split-Path -Parent }
$ui = Join-Path $Root ".main\ui\DeckUI.cpp"

function Get-Body([string]$text, [string]$marker, [int]$maxLines = 40) {
    $i = $text.IndexOf($marker)
    if ($i -lt 0) { return $null }
    $lines = $text.Substring($i) -split "`n"
    $out = @()
    $depth = 0
    $seen = $false
    for ($n = 0; $n -lt [Math]::Min($maxLines, $lines.Count); $n++) {
        $out += $lines[$n]
        $depth += ([regex]::Matches($lines[$n], "\{")).Count
        $depth -= ([regex]::Matches($lines[$n], "\}")).Count
        if ($depth -gt 0) { $seen = $true }
        if ($seen -and $depth -le 0) { break }
    }
    return ($out -join "`n")
}

function Test-Rules([string]$text) {
    $bad = @()

    # 1. Append() must gate on the column being shown, never on an open file.
    $append = Get-Body $text "void DeckWindow::Append(const std::string &aLine)"
    if (-not $append) { $bad += "Append() not found" }
    elseif ($append -match "if \(mEditPath\.empty\(\)\)") {
        $bad += "Append() still drops every line when no file is open in the editor"
    }
    elseif ($append -notmatch "mShowEditor") {
        $bad += "Append() does not gate on mShowEditor (the console is a column)"
    }

    # 2. (m076) A kernel record is shown only for the script the console belongs
    # to. The m067 answer ("an unclaimed console shows everything") is what the
    # user rejected: running another script must not write into this pane.
    # 90 lines: the m075 formatter is longer than the helper's 40-line default.
    $record = Get-Body $text "void DeckWindow::AppendRecord(const ErrorRecord &aRecord)" 90
    if (-not $record) { $bad += "AppendRecord() not found" }
    elseif ($record -match "mConsolePath\.empty\(\) \|\|") {
        $bad += "AppendRecord() still shows foreign scripts' records in an unclaimed console"
    }
    elseif ($record -notmatch "SamePathI\(path, mConsolePath\)") {
        $bad += "AppendRecord() no longer filters records by the console's owner"
    }

    # 3. (m076) Starting a script has no console-ownership parameter any more, and
    # nothing may hand the console to a run path.
    if ($text -match "StartPath\(const std::wstring &aPath, bool") {
        $bad += "StartPath grew an ownership flag again"
    }
    if ($text -match "mConsolePath = aPath") {
        $bad += "a run path claims the console again"
    }

    # 4. (m076) The owner is set in exactly two places: the editor-follow in
    # SyncConsoleSelection, and the rename that moves a binding with the file.
    $owners = ([regex]::Matches($text, "mConsolePath = ")).Count
    if ($owners -ne 2) {
        $bad += ("console ownership is assigned in " + $owners + " places (must be 2: editor-follow + rename)")
    }

    # 5. The comment that once lied must still describe the code.
    if ($text -match "The console belongs to whatever row is selected for testing") {
        $bad += "stale comment re-appeared: ownership is not 'the selected row'"
    }

    # 6. (m077) The row cell is a button, so a running script must never be drawn
    # with the idle glyph - that was the original m026 bug shape.
    $light = Get-Body $text "DeckWindow::RowLight DeckWindow::LightOf"
    if (-not $light) { $bad += "LightOf() not found" }
    elseif ($light -notmatch "return[^;]*RowLight::Running") {
        $bad += "LightOf() folds a running session back into the idle glyph"
    }

    # 7. (m077) The LAST RESULT still has to be visible in the list - but on the
    # row's edge, not inside the button cell. m068 put it in the cell as a grey
    # tick and the user read that as a special running mode and stopped clicking.
    if ($text -notmatch "bool DeckWindow::LastRunFailed") {
        $bad += "no LastRunFailed() probe: the last run's result has nowhere to live"
    }
    if ($text -notmatch "mOwner->LastRunFailed\(") {
        $bad += "the row painter never draws the last-run result"
    }

    # 8. (m075) One record = one short line: no kernel severity/phase words, no
    # full path (a load error prints the whole Scripts path), and "Specifically"
    # joins the same line instead of a second 35-space-indented one.
    if (-not $record) { $bad += "AppendRecord() not found for the format rules" }
    else {
        if ($record -match "aRecord\.severity|aRecord\.phase") {
            $bad += "AppendRecord prints the kernel's severity/phase words"
        }
        if ($record -notmatch "find_last_of") {
            $bad += "AppendRecord prints the record's file verbatim (load errors show an absolute path)"
        }
        if (([regex]::Matches($record, "Append\(")).Count -ne 1) {
            $bad += "AppendRecord emits more than one console line (the indented Specifically line is back)"
        }
    }

    # 9. (m075/m077) Dead vocabulary must stay dead: no session id in a receipt,
    # no English lifecycle words, and no result glyph back inside the button cell.
    foreach ($literal in @('"  started"', '"  exited"', '"  stopping"', '" (#"', '"  (fatal)"',
            'stop all ->', 'session(s)', 'RowLight::Finished', 'RowLight::Failed', 'DrawPen(')) {
        if ($text.Contains($literal)) {
            $bad += ("stale console literal: " + $literal)
        }
    }
    return $bad
}

if ($SelfTest) {
    # The gate must be able to go red: feed it the pre-m067 shape and demand a hit.
    $broken = @'
void DeckWindow::Append(const std::string &aLine)
{
    // The console is the test view of the file open in the editor.
    if (mEditPath.empty())
        return;
    mBuffer->append(aLine.c_str());
}

void DeckWindow::AppendRecord(const ErrorRecord &aRecord)
{
    std::string severity = aRecord.severity;
    std::string where = aRecord.file;
    std::string line = severity + aRecord.phase + where;
    Append(line);
    Append("                                   Specifically: " + aRecord.extra);
    const bool show = path.empty() || mConsolePath.empty() || SamePathI(path, mConsolePath);
    DWORD StartPath(const std::wstring &aPath, bool aOwnConsole);
    mConsolePath = aPath;
    void ToggleFromShortcut(int aIndex) { ToggleRow(aIndex); }
    ToggleRow(index);
    // The console belongs to whatever row is selected for testing
}

Append("[" + UiClock() + "] " + name + "  started (#2)");
Append("[" + UiClock() + "] " + name + "  exited  rc=2  (fatal)");

DeckWindow::RowLight DeckWindow::LightOf(int aIndex) const
{
    if (!row.session)
        return RowLight::Idle;
    if (session.state != SessionState::Finished)
        return RowLight::Idle;
    DrawPen(cx, cy, d, AlertRed());
    return session.fatal ? RowLight::Failed : RowLight::Finished;
}
'@
    $hits = Test-Rules $broken
    if ($hits.Count -eq 0) {
        Write-Host "selftest: the bad sample passed every rule - this gate is blind"
        exit 1
    }
    Write-Host ("selftest: bad sample caught by " + $hits.Count + " rule(s); real file checked below")
    if (-not (Test-Path $ui)) { Write-Host "selftest: real file missing: $ui"; exit 1 }
    $real = Test-Rules (Get-Content $ui -Raw -Encoding UTF8)
    if ($real.Count -gt 0) { Write-Host "selftest: the real file is already violating:"; $real | ForEach-Object { Write-Host "  - $_" }; exit 1 }
    Write-Host "console-owner: selftest OK (can go red, real file clean)"
    exit 0
}

if (-not (Test-Path $ui)) { Write-Host "console-owner: cannot find $ui"; exit 1 }
$violations = Test-Rules (Get-Content $ui -Raw -Encoding UTF8)
foreach ($v in $violations) { Write-Host "  [X] $v" }
if ($violations.Count -gt 0) {
    Write-Host ("console-owner: " + $violations.Count + " violation(s) - the run receipt can go invisible again")
    exit 1
}
Write-Host "console-owner: OK (9 rules hold over DeckUI.cpp)"
exit 0
