@echo off
REM ============================================================================
REM 1_check_env.bat - verify the build toolchain is installed (read-only)
REM ----------------------------------------------------------------------------
REM Checks, in order, that everything the later steps need is present:
REM
REM   cmake    - drives the FLTK and AHKDeck builds        (NMake Makefiles)
REM   cl.exe   - resolved by msvc_env.bat (MSVC C++ compiler)
REM   nmake    - NMake generator backend
REM   rc.exe   - Windows resource compiler
REM   tar.exe  - resolved by _common.bat (bsdtar, handles drive letters)
REM   curl.exe - resolved by _common.bat (downloads the libraries)
REM   MSBuild  - NOT fatal yet: the AHK_H kernel build (engine milestone) needs
REM              it; the skeleton itself does not.
REM
REM This script changes NOTHING on disk. It only reports, so it is safe to run
REM first on a fresh machine to learn what has to be installed.
REM
REM Environment switch:
REM   AHKDECK_CHAINED=1   suppress the final "press any key" (set by full.bat)
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [1/5] toolchain check
echo ======================================
echo.

set "ENV_FAIL=0"

REM ---- cmake -----------------------------------------------------------------
where cmake >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] cmake not found
    echo           install CMake 3.16+ and add it to PATH
    set "ENV_FAIL=1"
) else (
    set "CMAKE_VER="
    for /f "delims=" %%v in ('cmake --version 2^>nul') do (
        if not defined CMAKE_VER set "CMAKE_VER=%%v"
    )
    echo   [OK] !CMAKE_VER!
)

REM ---- MSVC compiler / linker / nmake ----------------------------------------
REM msvc_env.bat already ran via _common.bat and put cl.exe on PATH.
where cl.exe >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] cl.exe not found - MSVC environment setup failed
    set "ENV_FAIL=1"
) else (
    echo   [OK] cl.exe  : !AHKDECK_MSVC_BIN!
)

where nmake.exe >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] nmake.exe not found
    set "ENV_FAIL=1"
) else (
    echo   [OK] nmake.exe
)

REM ---- Windows resource compiler ---------------------------------------------
if defined AHKDECK_SDK_BIN if exist "!AHKDECK_SDK_BIN!\rc.exe" (
    echo   [OK] rc.exe   : !AHKDECK_SDK_BIN!
) else (
    where rc.exe >nul 2>&1
    if !ERRORLEVEL! neq 0 (
        echo   [ERROR] rc.exe not found - Windows SDK incomplete
        set "ENV_FAIL=1"
    ) else (
        echo   [OK] rc.exe
    )
)

REM ---- tar / curl (resolved by _common.bat) ----------------------------------
echo   [OK] tar.exe  : %TAR_EXE%
if not defined CURL_EXE (
    echo   [ERROR] curl.exe not found
    echo           ships with Windows 10/11; install curl if missing
    set "ENV_FAIL=1"
) else (
    echo   [OK] curl.exe : %CURL_EXE%
)

REM ---- MSBuild (non-fatal: needed only by the AHK_H kernel build later) ------
set "MSBUILD_EXE="
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%M in (`"%VSWHERE%" -latest -products * -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        if not defined MSBUILD_EXE set "MSBUILD_EXE=%%M"
    )
)
if defined MSBUILD_EXE if exist "!MSBUILD_EXE!" (
    echo   [OK] msbuild : !MSBUILD_EXE!
) else (
    echo   [WARN] msbuild not located - fine until the AHK_H kernel build
)
if not defined MSBUILD_EXE echo   [WARN] msbuild not located - fine until the AHK_H kernel build

REM ---- MSVC / SDK summary ------------------------------------------------------
echo.
echo   MSVC : %AHKDECK_MSVC_ROOT%
echo   SDK  : %AHKDECK_SDK_ROOT%
echo.

if "!ENV_FAIL!"=="1" (
    echo ======================================
    echo    toolchain incomplete - fix the [ERROR] items above
    echo ======================================
    echo.
    cd /d "%SCRIPT_DIR%."
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo   toolchain check passed
echo.

cd /d "%SCRIPT_DIR%."
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0
