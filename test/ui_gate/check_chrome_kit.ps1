# check_chrome_kit.ps1 - text gate for "every dialog wears the main window's frame".
# ----------------------------------------------------------------------------
# The hotkey dialog used to be an Fl_Window with a native caption: no companion
# glow, no 1px ring, no work-area centering, and a stock Fl_Scroll with FLTK's
# bevelled bar. Every one of those was a second implementation of an element the
# main window already had. m073 replaced the skeleton with DeckDialog and made
# each element have exactly one bottom function; these rules keep it that way.
#
# ASCII only: Windows PowerShell reads a BOM-less .ps1 as GBK and Chinese text
# turns into parse errors.
# ----------------------------------------------------------------------------
param([switch]$SelfTest, [string]$Root = "")

$ErrorActionPreference = "Stop"
if ($Root -eq "") { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) | Split-Path -Parent }
$uiDir = Join-Path $Root ".main\ui"

function Load-Tree([string]$dir) {
    $files = @{}
    Get-ChildItem $dir -File | Where-Object { $_.Extension -eq ".cpp" -or $_.Extension -eq ".h" } | ForEach-Object {
        $files[$_.Name] = Get-Content $_.FullName -Raw -Encoding UTF8
    }
    return $files
}

function Test-Rules($files) {
    $bad = @()

    # R1. Exactly two top-level window classes exist: the deck itself and the
    # shared dialog skeleton. A third one is a second chrome implementation.
    foreach ($name in $files.Keys) {
        foreach ($m in [regex]::Matches($files[$name], "class\s+(\w+)\s*:\s*public\s+Fl_(?:Double_)?Window\b")) {
            $cls = $m.Groups[1].Value
            if ($cls -ne "DeckWindow" -and $cls -ne "DeckDialog") {
                $bad += ("R1 ${name}: class $cls derives from Fl_Window directly instead of DeckDialog")
            }
        }
    }

    # R2. The skeleton must own the three things that were missing: the close-only
    # caption, the shadow detach on hide, and the re-attach on a second show.
    if (-not $files.ContainsKey("DeckDialog.cpp")) {
        $bad += "R2 DeckDialog.cpp is missing"
    }
    else {
        $dd = $files["DeckDialog.cpp"]
        if ($dd -notmatch "setCaption\(DeckTitleBar::kCaptionCloseOnly\)") {
            $bad += "R2 DeckDialog does not give its caption the close-only mask"
        }
        if ($dd -notmatch "windowShadow::detach\(this\)") {
            $bad += "R2 DeckDialog::hide leaves the companion glow on the desktop"
        }
        if ($dd -notmatch "windowShadow::attach\(this\)") {
            $bad += "R2 a re-shown DeckDialog never re-creates its glow (hide detaches it)"
        }
    }

    # R3. The 1px ring has one colour source. A per-dialog copy is how the main
    # window and a dialog end up one shade apart.
    $defs = @()
    foreach ($name in $files.Keys) {
        $n = [regex]::Matches($files[$name], "Fl_Color\s+FrameBorderColour\s*\(\s*\)\s*\{").Count
        if ($n -gt 0) { $defs += ($name + ":" + $n) }
    }
    if ($defs.Count -ne 1 -or $defs[0] -notlike "Theme.cpp:*") {
        $bad += ("R3 FrameBorderColour definitions: " + ($defs -join ", ") + " (must be Theme.cpp only)")
    }

    # R4. Native caption theming is not a second path to the same look: the frame
    # is ours (WM_NCCALCSIZE / NC_PAINT), and DWM only rounds and colours it.
    foreach ($name in $files.Keys) {
        if ($name -ne "WindowFrame.cpp" -and $files[$name] -match "DwmSetWindowAttribute") {
            $bad += "R4 $name calls DwmSetWindowAttribute outside the frame module"
        }
    }

    # R5. Stock Fl_Scroll is the bevelled bar the list and editor replaced
    # (m071 A2). Dialogs use DeckScroll.
    foreach ($name in $files.Keys) {
        if ($name -like "*.cpp" -and $files[$name] -match "new\s+Fl_Scroll\s*\(") {
            $bad += "R5 $name builds a stock Fl_Scroll instead of DeckScroll"
        }
    }

    # R6. The two entry points install the frame through ShowCentered, never
    # through a bare show() that would leave the dialog uncentred and undecorated.
    if ($files.ContainsKey("DeckUI.cpp")) {
        $du = $files["DeckUI.cpp"]
        foreach ($marker in @("void DeckWindow::ShowNameDialog", "void DeckWindow::ShowShortcutDialog")) {
            $i = $du.IndexOf($marker)
            if ($i -lt 0) { $bad += "R6 $marker not found"; continue }
            $body = $du.Substring($i, [Math]::Min(900, $du.Length - $i))
            if ($body -notmatch "ShowCentered\(\)") {
                $bad += "R6 $marker does not call ShowCentered()"
            }
        }
        if ($du -match "ApplyNativeCaptionTheme") {
            $bad += "R6 ApplyNativeCaptionTheme is back: dialogs are not native-captioned"
        }
    }
    else {
        $bad += "R6 DeckUI.cpp is missing"
    }

    # R7. Text sizes are one fact each (m071): labelsize/textsize never carry a
    # literal 16 or 14 again; Theme.h owns kTextSize/kSubTextSize.
    foreach ($name in $files.Keys) {
        foreach ($m in [regex]::Matches($files[$name], "(?:labelsize|textsize|linenumber_size)\s*\(\s*(1[46])\s*\)")) {
            $bad += ("R7 ${name}: literal text size " + $m.Groups[1].Value + " (use TextSize()/SubTextSize())")
        }
    }

    # R8. The dialog row step is one fact too: a stacked row is 26 + 2 = 28,
    # and it must be spoken as RowStep(), not as an addition with a literal.
    foreach ($name in $files.Keys) {
        if ($name -like "*.cpp") {
            foreach ($m in [regex]::Matches($files[$name], "\b(?:y|rowY|top)\s*\+=\s*(?:26|28)\s*;")) {
                $bad += ("R8 ${name}: literal row step '" + $m.Value + "' (use RowStep())")
            }
        }
    }

    # R9. Colours outside the palette have one home: Theme.cpp. A widget that
    # writes fl_rgb_color() is a second source for a colour. WindowFrame.cpp is
    # the one exception: it composites the ring's antialiased pixels, so it must
    # mix channel values; it reads those channels from the theme colours.
    foreach ($name in $files.Keys) {
        if ($name -ne "Theme.cpp" -and $name -ne "WindowFrame.cpp" -and $files[$name] -match "fl_rgb_color\(") {
            $bad += "R9 $name writes fl_rgb_color() outside Theme.cpp"
        }
    }

    return $bad
}

if ($SelfTest) {
    # Every rule must fire on a sample that describes the pre-m073 shape.
    $broken = @{}
    $broken["DeckUI.cpp"] = @'
class NameWindow : public Fl_Window
{
public:
    void Show() { show(); }
};
void DeckWindow::ShowNameDialog() { mNameDialog = new NameWindow(); mNameDialog->show(); }
void DeckWindow::ShowShortcutDialog() { mShortcutDialog = new ShortcutDialogWindow(this); mShortcutDialog->show(); }
static Fl_Color FrameBorderColour() { return DeckColors().text1; }
void Build() { Fl_Scroll *s = new Fl_Scroll(0, 0, 10, 10); }
void Theme() { DwmSetWindowAttribute(h, 1, &v, 4); }
void Old() { ApplyNativeCaptionTheme(win); }
void Sizes() { w->labelsize(16); y += 28; fl_color(fl_rgb_color(1, 2, 3)); }
'@
    $broken["DeckDialog.cpp"] = @'
void DeckDialog::ShowCentered() { show(); }
void DeckDialog::hide() { Fl_Double_Window::hide(); }
'@
    $broken["Theme.cpp"] = 'Fl_Color FrameBorderColour() { return kLight.text1; }'
    $hits = Test-Rules $broken
    $missing = @()
    foreach ($rule in @("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9")) {
        if (-not ($hits | Where-Object { $_ -like ($rule + " *") })) { $missing += $rule }
    }
    if ($missing.Count -gt 0) {
        Write-Host ("selftest: these rules stayed blind on the bad sample: " + ($missing -join ", "))
        exit 1
    }
    Write-Host ("selftest: bad sample caught by " + $hits.Count + " finding(s); real tree checked below")
    if (-not (Test-Path $uiDir)) { Write-Host "selftest: missing directory: $uiDir"; exit 1 }
    $real = Test-Rules (Load-Tree $uiDir)
    if ($real.Count -gt 0) {
        Write-Host "selftest: the real tree is already violating:"
        $real | ForEach-Object { Write-Host ("  - " + $_) }
        exit 1
    }
    Write-Host "chrome-kit: selftest OK (all nine rules can go red, real tree clean)"
    exit 0
}

if (-not (Test-Path $uiDir)) { Write-Host ("chrome-kit: cannot find " + $uiDir); exit 1 }
$violations = Test-Rules (Load-Tree $uiDir)
foreach ($v in $violations) { Write-Host ("  [X] " + $v) }
if ($violations.Count -gt 0) {
    Write-Host ("chrome-kit: " + $violations.Count + " violation(s) - dialogs can drift from the main window again")
    exit 1
}
Write-Host "chrome-kit: OK (9 rules hold over .main\ui)"
exit 0
