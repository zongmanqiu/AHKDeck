@echo off
REM ============================================================================
REM 3_patch.bat - apply our patches from .main\patches\ into .3rd\
REM ----------------------------------------------------------------------------
REM Copies ONE WAY: patches : .3rd only. NEVER copy the other direction -
REM .3rd holds pristine upstream sources, and copying back would overwrite the
REM authoritative patch archive (project AGENTS.md rule 2).
REM
REM   * FLTK          : patches\fltk-1.4.5\FL\*  : .3rd\fltk-1.4.5\FL\
REM                     patches\fltk-1.4.5\src\* : .3rd\fltk-1.4.5\src\
REM                     (whole-file patches guarded by baseline.sha256)
REM
REM Conditional copy (content differs only): copy /Y always refreshes the
REM destination mtime, so a timestamp can never tell "this file was re-patched"
REM from "this file was touched". That ambiguity once cost us the menu
REM shadow. Conditional copy keeps mtime meaningful: it changes only when the
REM bytes change, which is exactly when dependents must rebuild. It also prints
REM only what changed, so a no-op run is visibly a no-op.
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [3/5] apply patches
echo ======================================
echo.

if not exist "%PATCHES%" (
    echo   [ERROR] patches dir not found: %PATCHES%
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- FLTK -------------------------------------------------------------------
echo   FLTK...
if exist "%PATCHES%\fltk-1.4.5\FL" (
    if not exist "%THIRDPARTY%\fltk-1.4.5\FL" mkdir "%THIRDPARTY%\fltk-1.4.5\FL"
    for %%f in ("%PATCHES%\fltk-1.4.5\FL\*") do (
        call :sync "%%f" "%THIRDPARTY%\fltk-1.4.5\FL\%%~nxf" "FL/%%~nxf"
    )
)
if exist "%PATCHES%\fltk-1.4.5\src" (
    if not exist "%THIRDPARTY%\fltk-1.4.5\src" mkdir "%THIRDPARTY%\fltk-1.4.5\src"
    for %%f in ("%PATCHES%\fltk-1.4.5\src\*") do (
        call :sync "%%f" "%THIRDPARTY%\fltk-1.4.5\src\%%~nxf" "src/%%~nxf"
    )
)

REM m192: the official kernel (single-kernel plan). Same whole-file discipline:
REM baseline guard first, then one-way conditional copy.
echo   AutoHotkey v2.0.31 (official)...
if exist "%PATCHES%\AutoHotkey-v2.0.31\source" (
    if exist "%PATCHES%\AutoHotkey-v2.0.31\baseline.sha256" (
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0guard_patch.ps1" -PatchDir "%PATCHES%\AutoHotkey-v2.0.31\source" -DstDir "%THIRDPARTY%\AutoHotkey-v2.0.31-src\source" -Baseline "%PATCHES%\AutoHotkey-v2.0.31\baseline.sha256"
        if errorlevel 1 goto :guard_fail
    )
    for %%f in ("%PATCHES%\AutoHotkey-v2.0.31\source\*") do (
        call :sync "%%f" "%THIRDPARTY%\AutoHotkey-v2.0.31-src\source\%%~nxf" "source/%%~nxf"
    )
)

goto :patch_end

:guard_fail
echo   [ERROR] patch baseline guard failed - kernel left unpatched
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 1

:patch_end
echo.
echo   patches applied (only changed files were copied)
echo.

cd /d "%SCRIPT_DIR%."
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0

REM ---------------------------------------------------------------------------
REM :sync <src> <dst> <label>
REM Copy only when the bytes differ. Silent when identical, so a no-op patch
REM run prints nothing per file.
REM ---------------------------------------------------------------------------
:sync
if not exist "%~2" (
    copy /Y "%~1" "%~2" >nul
    echo     + %~3  ^(new^)
    goto :eof
)
fc /b "%~1" "%~2" >nul 2>&1
if errorlevel 1 (
    copy /Y "%~1" "%~2" >nul
    echo     ~ %~3  ^(updated^)
)
goto :eof
