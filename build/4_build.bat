@echo off
REM ============================================================================
REM 4_build.bat - build FLTK (as needed) and AHKDeck itself
REM ----------------------------------------------------------------------------
REM Steps:
REM   [1/4] dependency presence check (deps.txt is the single source of truth)
REM   [2/4] re-apply patches (3_patch.bat) unless AHKDECK_SKIP_PATCH=1
REM   [3/4] FLTK static lib: rebuild if missing / older than patches / missing
REM         patch hooks (check_fltk_patch.ps1 probes the ARCHIVE, not mtimes -
REM         a library built from unpatched sources links and runs fine and
REM         silently loses features; the menu shadow was lost that way)
REM         or on the wrong CRT: the exe is /MT (single exe, no redistributable)
REM         and FLTK's FLTK_MSVC_RUNTIME_DLL defaults to ON = /MD, which ignores
REM         CMAKE_MSVC_RUNTIME_LIBRARY - so the configure line passes both
REM   [4/4] AHKDeck: CMake configure + incremental NMake build : .build\
REM
REM Directory policy (project AGENTS.md):
REM   FLTK build tree : .temp\fltk-build      (intermediate, never in .3rd)
REM   CMake tree      : .temp\cmake_build     (intermediate)
REM   exe             : .build\AHKDeck.exe    (the only shipped artifact)
REM
REM Environment switches:
REM   AHKDECK_CHAINED=1     suppress the final "press any key"
REM   AHKDECK_SKIP_PATCH=1  do not re-run 3_patch.bat (full.bat already did)
REM   AHKDECK_FORCE_FLTK=1  wipe the FLTK build tree and rebuild from scratch
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [4/5] build
echo ======================================
echo.

where cmake >nul 2>&1
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] cmake not found - run 1_check_env.bat
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- [1/4] dependency presence check ----------------------------------------
set "MISSING_DEPS="
for /f "usebackq tokens=1,* delims==" %%a in ("%DEPS_FILE%") do (
    set "dep_name=%%a"
    if not "!dep_name:~0,1!"=="#" if not "!dep_name!"=="" (
        set "dep_rest=%%b"
        set "dep_marker="
        for /f "tokens=1,* delims=|" %%u in ("!dep_rest!") do set "dep_marker=%%v"

        if not exist "%THIRDPARTY%\!dep_name!" (
            echo   [ERROR] .3rd\!dep_name! does not exist
            set "MISSING_DEPS=1"
        ) else if not "!dep_marker!"=="" (
            if not exist "%THIRDPARTY%\!dep_name!\!dep_marker!" (
                echo   [ERROR] .3rd\!dep_name! incomplete - missing !dep_marker!
                set "MISSING_DEPS=1"
            )
        )
    )
)
if defined MISSING_DEPS (
    echo   run 2_download.bat first.
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- [2/4] re-apply patches ---------------------------------------------------
if "%AHKDECK_SKIP_PATCH%"=="1" (
    echo [2/4] patches - skipped ^(AHKDECK_SKIP_PATCH=1^)
) else (
    set "AHKDECK_CHAINED_SAVE=!AHKDECK_CHAINED!"
    set "AHKDECK_CHAINED=1"
    call "%SCRIPT_DIR%3_patch.bat"
    if !ERRORLEVEL! neq 0 (
        echo   [2/4] [ERROR] 3_patch.bat failed
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
    set "AHKDECK_CHAINED=!AHKDECK_CHAINED_SAVE!"
)
echo.

REM ---- [3/4] FLTK ----------------------------------------------------------------
echo [3/4] FLTK...
set "FLTK_SRC=%THIRDPARTY%\fltk-1.4.5"
set "FLTK_LIB=%FLTK_BUILD_DIR%\lib\fltk.lib"

if not exist "%FLTK_SRC%" (
    echo   [ERROR] .3rd\fltk-1.4.5 does not exist - run 2_download.bat first
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

if "!AHKDECK_FORCE_FLTK!"=="1" (
    echo   AHKDECK_FORCE_FLTK=1 - wiping %FLTK_BUILD_DIR%
    if exist "%FLTK_BUILD_DIR%" rmdir /s /q "%FLTK_BUILD_DIR%"
)

set "FLTK_NEED=0"
if not exist "%FLTK_LIB%" (
    set "FLTK_NEED=1"
) else (
    set "FLTK_TS="
    for %%A in ("%FLTK_LIB%") do set "FLTK_TS=%%~tA"
    for %%f in ("%PATCHES%\fltk-1.4.5\FL\*" "%PATCHES%\fltk-1.4.5\src\*") do (
        if "%%~tf" gtr "!FLTK_TS!" set "FLTK_NEED=1"
    )
    REM Probe the archive for the patch hooks regardless of mtimes: a copy
    REM refreshes mtime, and a lib built from the wrong sources still links.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%check_fltk_patch.ps1" -Lib "!FLTK_LIB!" -PatchDir "%PATCHES%\fltk-1.4.5"
    if !ERRORLEVEL! neq 0 (
        echo   [WARN] fltk.lib is missing patch hooks - forcing rebuild
        set "FLTK_NEED=1"
    )
)

if "!FLTK_NEED!"=="1" (
    echo   FLTK needs building...
    REM The build tree lives in .temp; if it exists but was not configured with
   REM the NMake generator - mixing generators in one dir is not allowed - wipe
   REM and reconfigure. Reuse only a cache that still has the generated rules.
    set "REUSE="
    if exist "%FLTK_BUILD_DIR%\CMakeCache.txt" (
        findstr /C:"CMAKE_GENERATOR:INTERNAL=NMake Makefiles" "%FLTK_BUILD_DIR%\CMakeCache.txt" >nul 2>&1
        if !ERRORLEVEL! equ 0 set "REUSE=1"
    )
    if defined REUSE (
        cd /d "%FLTK_BUILD_DIR%"
        echo   reusing NMake build tree
    ) else (
        if exist "%FLTK_BUILD_DIR%" rmdir /s /q "%FLTK_BUILD_DIR%"
        mkdir "%FLTK_BUILD_DIR%"
        cd /d "%FLTK_BUILD_DIR%"
        echo   configuring FLTK - NMake Makefiles...
        cmake "%FLTK_SRC%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DFLTK_MSVC_RUNTIME_DLL=OFF -DFLTK_BUILD_EXAMPLES=OFF -DFLTK_BUILD_TEST=OFF -DFLTK_BUILD_FLUID=OFF -DFLTK_BUILD_FLTK_OPTIONS=OFF -DOPTION_USE_SYSTEM_ZLIB=OFF -DOPTION_USE_SYSTEM_LIBJPEG=OFF -DOPTION_USE_SYSTEM_LIBPNG=OFF
        if !ERRORLEVEL! neq 0 (
            echo   [ERROR] FLTK CMake configure failed
    if not "!AHKDECK_CHAINED!"=="1" pause
            exit /b 1
        )
    )
    cmake --build .
    if !ERRORLEVEL! neq 0 (
        echo   [ERROR] FLTK build failed
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
    REM The rebuilt lib must contain the patch hooks - a wrong library fails
    REM the build instead of shipping silently.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%check_fltk_patch.ps1" -Lib "!FLTK_LIB!" -PatchDir "%PATCHES%\fltk-1.4.5"
    if !ERRORLEVEL! neq 0 (
        echo   [ERROR] rebuilt fltk.lib still missing patch hooks
        echo           .3rd\fltk-1.4.5 does not match patches\fltk-1.4.5?
        echo           run 3_patch.bat and retry
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
) else (
    echo   FLTK lib is up to date
)
echo.

REM ---- [3.5/4] kernel DLL -----------------------------------------------------------
REM 4a in build_kernel.bat: patched AHK_H ReleaseDll -> .temp\kernel\AutoHotkey64.dll
set "AHKDECK_CHAINED_SAVE=!AHKDECK_CHAINED!"
set "AHKDECK_CHAINED=1"
call "%SCRIPT_DIR%build_kernel.bat"
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] kernel build failed
    if not "!AHKDECK_CHAINED_SAVE!"=="1" pause
    exit /b 1
)
set "AHKDECK_CHAINED=!AHKDECK_CHAINED_SAVE!"
echo.

REM ---- [4/4] AHKDeck --------------------------------------------------------------
echo [4/4] CMake build AHKDeck...
set "NEED_CONFIGURE=1"
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=NMake Makefiles" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if !ERRORLEVEL! equ 0 (
        set "NEED_CONFIGURE=0"
        echo   reusing existing NMake build tree
    ) else (
        echo   generator mismatch - wiping stale build tree
        rmdir /s /q "%BUILD_DIR%"
    )
)
mkdir "%BUILD_DIR%" 2>nul
cd /d "%BUILD_DIR%"

if "!NEED_CONFIGURE!"=="1" (
    echo   configuring AHKDeck - NMake Makefiles...
    cmake "%MAIN_DIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
    if !ERRORLEVEL! neq 0 (
        echo   [ERROR] CMake configure failed
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
) else (
    cmake "%MAIN_DIR%"
    if !ERRORLEVEL! neq 0 (
        echo   [ERROR] CMake reconfigure failed
    if not "!AHKDECK_CHAINED!"=="1" pause
        exit /b 1
    )
)

echo   building - Release...
cmake --build .
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] build failed
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- ship the kernel next to the exe ----------------------------------------------
REM One directory, two files: AHKDeck.exe + the patched interpreter it hosts.
if exist "%PROJECT_ROOT%\.temp\kernel\AutoHotkey64.dll" (
    copy /y "%PROJECT_ROOT%\.temp\kernel\AutoHotkey64.dll" "%OUTPUT_DIR%\AutoHotkey64.dll" >nul
    set "KERNEL_SHIPPED=%OUTPUT_DIR%\AutoHotkey64.dll"
) else (
    echo   [WARN] .temp\kernel\AutoHotkey64.dll missing - --run will not work
    set "KERNEL_SHIPPED=(missing)"
)

echo.
echo   AHKDeck.exe       : %OUTPUT_DIR%\AHKDeck.exe
echo   kernel DLL       : !KERNEL_SHIPPED!
echo   fltk.lib          : %FLTK_BUILD_DIR%\lib\fltk.lib
echo   scripts copied    : %OUTPUT_DIR%\Scripts\ by the deck_scripts target (CMakeLists.txt)
echo.

cd /d "%SCRIPT_DIR%."
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0
