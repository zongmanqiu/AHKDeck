@echo off
REM ============================================================================
REM _common.bat - shared setup for the numbered build scripts (ASCII only)
REM ----------------------------------------------------------------------------
REM Sourced via "call" by 1_check_env.bat / 2_download.bat / 3_patch.bat /
REM 4_build.bat / 5_test.bat / full.bat. Sets the path variables every script
REM needs and makes sure the MSVC environment is live.
REM
REM Layout (we live in [project root]\.main\build\):
REM   %SCRIPT_DIR%    = [project root]\.main\build\
REM   %MAIN_DIR%      = [project root]\.main        <- CMake source dir, patches' parent
REM   %PROJECT_ROOT%  = [project root]              <- .3rd / .temp / .build live here
REM
REM Directory contract (project AGENTS.md):
REM   .3rd   = third-party SOURCES only, never build outputs
REM   .build = shipped artifacts (AHKDeck.exe + Scripts\)
REM   .temp  = every intermediate: download cache, FLTK build tree, CMake tree
REM   .bf    = snapshots (never written by these scripts)
REM
REM IMPORTANT: no setlocal here on purpose - the caller needs the variables.
REM ============================================================================

set "SCRIPT_DIR=%~dp0"
set "MAIN_DIR=%SCRIPT_DIR%.."
set "PROJECT_ROOT=%MAIN_DIR%\.."

REM Normalize away the "..\" segments so every derived path and log line shows
REM a clean absolute path.
for %%A in ("%PROJECT_ROOT%") do set "PROJECT_ROOT=%%~fA"

set "THIRDPARTY=%PROJECT_ROOT%\.3rd"
set "PATCHES=%MAIN_DIR%\patches"
set "DEPS_FILE=%SCRIPT_DIR%deps.txt"
set "DOWNLOAD_CACHE=%PROJECT_ROOT%\.temp\download_cache"
set "BUILD_DIR=%PROJECT_ROOT%\.temp\cmake_build"
set "FLTK_BUILD_DIR=%PROJECT_ROOT%\.temp\fltk-build"
set "OUTPUT_DIR=%PROJECT_ROOT%\.build"

REM ---- Pick a tar that understands Windows drive-letter paths ----------------
REM Must resolve via %SystemRoot%, not PATH: when launched from Git Bash, PATH
REM puts GNU tar first and it reads "E:\..." as a remote host spec ("Cannot
REM connect to E: resolve failed"). Windows' built-in bsdtar handles drive
REM letters natively.
set "TAR_EXE="
if defined SystemRoot if exist "%SystemRoot%\System32\tar.exe" set "TAR_EXE=%SystemRoot%\System32\tar.exe"
if not defined TAR_EXE for %%A in (tar.exe) do set "TAR_EXE=%%~$PATH:A"
if not defined TAR_EXE (
    echo   [ERROR] tar.exe not found
    exit /b 1
)
"%TAR_EXE%" --version >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo   [ERROR] tar.exe not usable: %TAR_EXE%
    exit /b 1
)

REM ---- curl for downloading ---------------------------------------------------
set "CURL_EXE="
if defined SystemRoot if exist "%SystemRoot%\System32\curl.exe" set "CURL_EXE=%SystemRoot%\System32\curl.exe"
if not defined CURL_EXE for %%A in (curl.exe) do set "CURL_EXE=%%~$PATH:A"

REM ---- MSVC environment (needed to compile anything) --------------------------
if not exist "%SCRIPT_DIR%msvc_env.bat" (
    echo   [ERROR] msvc_env.bat not found
    exit /b 1
)
REM msvc_env.bat is harmless to run twice, but skip the noise when a parent
REM script (full.bat) already set it up.
if "%AHKDECK_MSVC_READY%"=="1" exit /b 0
call "%SCRIPT_DIR%msvc_env.bat"
if %ERRORLEVEL% neq 0 (
    echo   [ERROR] MSVC environment setup failed
    exit /b 1
)
set "AHKDECK_MSVC_READY=1"

exit /b 0
