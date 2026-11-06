@echo off
REM ============================================================================
REM 2_download.bat - download + extract the third-party libraries
REM ----------------------------------------------------------------------------
REM Reads build\deps.txt: each active line is "name=url" or "name=url|marker".
REM
REM   * archives are cached under .temp\download_cache\<name>.tar.gz
REM   * they are extracted into .3rd\<name>\
REM   * "marker" (a path relative to .3rd\<name>\) is the integrity probe - a
REM     library is treated as present only if that file exists, so a
REM     half-extracted directory left by an interrupted run is re-fetched
REM     instead of being silently accepted
REM
REM Environment switches:
REM   AHKDECK_FORCE_DOWNLOAD=1   ignore cache + presence, re-download everything
REM   AHKDECK_CHAINED=1          suppress the final "press any key"
REM
REM Run from .main\build\. Plain cmd is fine.
REM
REM IMPLEMENTATION NOTE: the per-dependency body runs in a subroutine (call
REM :one) instead of inline - cmd cannot nest a "for /f" inside another "for /f"
REM body without losing variables, and the "url|marker" split needs exactly
REM that (structure inherited from pecia, battle-tested there).
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [2/5] third-party download
echo ======================================
echo.

if not defined CURL_EXE (
    echo   [ERROR] curl.exe not found
    echo           ships with Windows 10/11; install curl if missing
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
if not exist "%DEPS_FILE%" (
    echo   [ERROR] deps.txt not found: %DEPS_FILE%
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

if not exist "%THIRDPARTY%" mkdir "%THIRDPARTY%"
if not exist "%DOWNLOAD_CACHE%" mkdir "%DOWNLOAD_CACHE%"

if "!AHKDECK_FORCE_DOWNLOAD!"=="1" echo   AHKDECK_FORCE_DOWNLOAD=1 - force re-download
echo.

set "DL_COUNT=0"
set "SKIP_COUNT=0"
set "FIX_COUNT=0"

for /f "usebackq tokens=1,* delims==" %%a in ("%DEPS_FILE%") do (
    call :one "%%a" "%%b"
    if !ERRORLEVEL! neq 0 exit /b 1
)

echo.
echo   downloaded: %DL_COUNT%, skipped: %SKIP_COUNT%, repaired: %FIX_COUNT%
echo.

cd /d "%SCRIPT_DIR%."
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0


REM ============================================================================
REM :one <name> <url-or-url|marker>
REM   Handles a single deps.txt entry. Returns 1 on any hard failure.
REM ============================================================================
:one
set "NAME=%~1"
set "REST=%~2"

REM Skip comments and blank lines.
if "!NAME:~0,1!"=="#" exit /b 0
if "!NAME!"=="" exit /b 0

REM Split "url|marker" (marker optional).
set "URL=!REST!"
set "MARKER="
for /f "tokens=1,* delims=|" %%u in ("!REST!") do (
    set "URL=%%u"
    set "MARKER=%%v"
)
for /f "tokens=* delims= " %%u in ("!URL!") do set "URL=%%u"

REM Decide whether the extracted tree looks complete.
set "HAVE=0"
if exist "%THIRDPARTY%\!NAME!" (
    set "HAVE=1"
    if not "!MARKER!"=="" if not exist "%THIRDPARTY%\!NAME!\!MARKER!" set "HAVE=0"
)

echo   checking !NAME!...

if "!HAVE!"=="1" if not "!AHKDECK_FORCE_DOWNLOAD!"=="1" (
    echo     present, skipped
    set /a SKIP_COUNT+=1
    exit /b 0
)

if "!HAVE!"=="0" if exist "%THIRDPARTY%\!NAME!" (
    echo     incomplete directory, re-extracting
    set /a FIX_COUNT+=1
)

REM Archive kind: tar.gz by default, .zip when the URL says so (release assets
REM such as the official AutoHotkey portable build are zip-only).
set "EXT=tar.gz"
if /i "!URL:~-4!"==".zip" set "EXT=zip"

set "CACHED=0"
if not "!AHKDECK_FORCE_DOWNLOAD!"=="1" if exist "%DOWNLOAD_CACHE%\!NAME!.!EXT!" set "CACHED=1"

if "!CACHED!"=="0" (
    echo     downloading: !URL!
    "%CURL_EXE%" -L --fail --retry 3 --retry-all-errors --retry-delay 3 -o "%DOWNLOAD_CACHE%\!NAME!.!EXT!" "!URL!"
    if !ERRORLEVEL! neq 0 (
        echo     [ERROR] download failed: !NAME!
        echo             URL: !URL!
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
) else (
    echo     using cached archive
)

REM Extract into a scratch dir first; only promote it once tar reported
REM success, so an interrupted run never leaves a directory that looks complete.
echo     extracting...
if exist "%THIRDPARTY%\!NAME!.extracting" rmdir /s /q "%THIRDPARTY%\!NAME!.extracting"
mkdir "%THIRDPARTY%\!NAME!.extracting"

if /i "!EXT!"=="zip" (
    set "ZIP_SRC=%DOWNLOAD_CACHE%\!NAME!.zip"
    set "ZIP_DST=%THIRDPARTY%\!NAME!.extracting"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; Expand-Archive -LiteralPath $env:ZIP_SRC -DestinationPath $env:ZIP_DST -Force"
) else (
    "%TAR_EXE%" -xzf "%DOWNLOAD_CACHE%\!NAME!.!EXT!" -C "%THIRDPARTY%\!NAME!.extracting"
)
if !ERRORLEVEL! neq 0 (
    echo     [ERROR] extraction failed: !NAME!
    echo             archive: %DOWNLOAD_CACHE%\!NAME!.!EXT!
    echo             hint: delete the archive and retry
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

set "EXTRACTED_DIR="
if /i "!EXT!"=="zip" (
    REM Release zips are flat (payload at the archive root); promote the scratch
    REM directory itself instead of looking for a single top-level folder.
    set "EXTRACTED_DIR=%THIRDPARTY%\!NAME!.extracting"
) else (
    for /d %%d in ("%THIRDPARTY%\!NAME!.extracting\*") do (
        if not defined EXTRACTED_DIR set "EXTRACTED_DIR=%%d"
    )
)

if not defined EXTRACTED_DIR (
    echo     [ERROR] no extracted directory found
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM Verify the marker before promoting - catches a truncated archive.
if not "!MARKER!"=="" if not exist "!EXTRACTED_DIR!\!MARKER!" (
    echo     [ERROR] extracted tree is missing !MARKER!
    echo             the archive may be corrupt: %DOWNLOAD_CACHE%\!NAME!.!EXT!
    echo             delete the archive and retry
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

if exist "%THIRDPARTY%\!NAME!" rmdir /s /q "%THIRDPARTY%\!NAME!"
move "!EXTRACTED_DIR!" "%THIRDPARTY%\!NAME!" >nul
rmdir /s /q "%THIRDPARTY%\!NAME!.extracting" 2>nul
echo     extracted: !NAME!
set /a DL_COUNT+=1
exit /b 0
