@echo off
REM ============================================================================
REM 5_test.bat - run the test suite. This is a GATE, not a report.
REM ----------------------------------------------------------------------------
REM ctest in .temp\cmake_build; ANY failing test returns exit code 1 so chained
REM callers (full.bat) and the human both see a red pipeline.
REM   AHKDECK_CHAINED=1   suppress the final "press any key" (set by full.bat)
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [5/5] tests
echo ======================================
echo.

REM ---- gate: AHK_H-only builtin names must not leak into shipped AHK code -------
REM The 38 names in ahkh_only_names.txt do not exist in official AutoHotkey, so
REM any script using them silently stops being "runs like official".
echo   checking AHK_H-only builtin names (m004 phase C)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\kernel_gate\check_kernel_names.ps1" -Root "%PROJECT_ROOT%"
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] shipped AHK code uses AutoHotkey_H-only builtin names
    echo           see .main\test\kernel_gate\ahkh_only_names.txt
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
echo.

REM ---- gate: Fl_Choice items must be separated by '|', never by a newline -------
REM add("A\nB") is ONE item: value() cannot reach the rest, the callback reports
REM -1 and the box draws the middle line of the single label - so the widget looks
REM alive and is not. Neither the compiler nor a runtime gate notices.
REM ---- gate: the console must stay writable without an open file (m067) ----
REM Four filters used to hide it: Append() required mEditPath, the receipts and
REM records required a matching owner, StartPath only claimed it for the open file.
REM A one-shot run from the list then printed nothing at all while every test
REM stayed green - only a text gate can keep that shape from coming back.
echo   checking console ownership rules (m067)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_console_owner.ps1" -SelfTest
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] the console-owner gate cannot catch its own bad sample
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_console_owner.ps1" -Root "%PROJECT_ROOT%"
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] the console can go silent again - see the rules above
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
echo.

echo   checking Fl_Choice item separators (m024)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_choice_labels.ps1" -SelfTest
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] the choice-label gate cannot catch its own bad sample
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_choice_labels.ps1" -Root "%PROJECT_ROOT%"
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] a dropdown is filled with a newline-separated string
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
echo.

REM ---- gate: every dialog wears the main window frame (m073) -----------------
REM One skeleton (DeckDialog), one ring colour, one scrollbar kit, one centering
REM entry. Before m073 the hotkey dialog was an Fl_Window with a native caption:
REM no glow, no ring, no centering - a second look for the same product.
echo   checking dialog chrome kit (m073)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_chrome_kit.ps1" -SelfTest
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] the chrome-kit gate cannot catch its own bad sample
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%MAIN_DIR%\test\ui_gate\check_chrome_kit.ps1" -Root "%PROJECT_ROOT%"
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] a dialog drifted off the shared chrome kit - see the rules above
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
echo.

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo   [ERROR] %BUILD_DIR% is not configured - run 4_build.bat first
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

cd /d "%BUILD_DIR%"
REM -LE gui keeps the default pipeline headless (the user's standing rule: gates
REM that open a window are opt-in). mcp_stdio (m064) is the labelled one so far.
ctest --output-on-failure -LE gui
if !ERRORLEVEL! neq 0 (
    echo.
    echo   [ERROR] tests failed - build does not pass
    echo           details above; set AHKDECK_SKIP_TESTS=1 to compile without tests
    echo.
    cd /d "%SCRIPT_DIR%."
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- opt-in: the window-launching gates (m064 MCP stdio closed loop) --------
if "!AHKDECK_GATE_UI!"=="1" (
    echo   running GUI-labelled gates ^(AHKDECK_GATE_UI=1^)...
    ctest --output-on-failure -L gui
    if !ERRORLEVEL! neq 0 (
        echo.
        echo   [ERROR] a GUI-labelled gate failed - see the cell list above
        echo.
        cd /d "%SCRIPT_DIR%."
        if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
)

echo   all tests passed
echo.
cd /d "%SCRIPT_DIR%."
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0
