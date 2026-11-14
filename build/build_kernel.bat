@echo off
REM ============================================================================
REM build_kernel.bat - build the ONE patched official AutoHotkey kernel (m192 P4)
REM ----------------------------------------------------------------------------
REM Outputs (directory contract from AGENTS.md):
REM   .temp\kernel_official\AutoHotkey64.exe  <- the single shipped interpreter
REM   .temp\kernel_build\official_*          <- MSBuild intermediates (never .3rd)
REM
REM m192 P4: the AHK_H pair is RETIRED - MT_ReleaseDll (AutoHotkey64.dll, the
REM in-process kernel) and MT_Release (the tools\ compile base). The engine now
REM spawns the official EXE once per session and ExeCompiler bundles scripts
REM into a copy of that very EXE, so one build serves both and the package
REM carries exactly two PE files. The AHK_H source tree and its patch archive
REM stay in .3rd / patches for history and the m004 parity gate; they are no
REM longer built, staged or shipped.
REM
REM The official v2.0.31 source carries four env-gated patches from
REM patches\AutoHotkey-v2.0.31: version fallback, JSON error/warning sink,
REM NO_TRAY + tray-menu dump, and their startup hook. With no AHKDECK_* env
REM vars set, behaviour is byte-identical to upstream. See that README.
REM
REM The build leaves asm/lib staging dirs inside .3rd; they are deleted here so
REM .3rd stays source-only.
REM
REM Environment:
REM   AHKDECK_CHAINED=1       suppress "press any key"
REM   AHKDECK_FORCE_KERNEL=1  rebuild even when the exe looks up to date
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [4a/5] official kernel
echo ======================================
echo.

set "OFFICIAL_SRC=%THIRDPARTY%\AutoHotkey-v2.0.31-src"
set "OFFICIAL_OUT=%PROJECT_ROOT%\.temp\kernel_official"
set "OFFICIAL_EXE=%OFFICIAL_OUT%\AutoHotkey64.exe"
set "KERNEL_BUILD=%PROJECT_ROOT%\.temp\kernel_build"

if not exist "%OFFICIAL_SRC%\AutoHotkeyx.sln" (
    echo   [ERROR] .3rd\AutoHotkey-v2.0.31-src is missing - run 2_download.bat
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- up-to-date check: the EXE must be newer than every patch --------------
set "OFFICIAL_NEED=0"
if not exist "%OFFICIAL_EXE%" (
    set "OFFICIAL_NEED=1"
) else (
    set "OFFICIAL_TS="
    for %%A in ("%OFFICIAL_EXE%") do set "OFFICIAL_TS=%%~tA"
    for %%f in ("%PATCHES%\AutoHotkey-v2.0.31\source\*") do (
        if "%%~tf" gtr "!OFFICIAL_TS!" set "OFFICIAL_NEED=1"
    )
)
if "!AHKDECK_FORCE_KERNEL!"=="1" set "OFFICIAL_NEED=1"

if "!OFFICIAL_NEED!"=="0" (
    echo   official kernel is up to date
    goto :done
)

REM ---- MSBuild location -----------------------------------------------------
REM AHKDECK_MSVC_ROOT is "<vs>\VC\Tools\MSVC\<ver>", so MSBuild sits four levels
REM up ("<vs>\MSBuild\Current\Bin"). The three-level guess is kept as a fallback.
set "MSBUILD_EXE="
if defined AHKDECK_MSVC_ROOT (
    if exist "!AHKDECK_MSVC_ROOT!\..\..\..\..\MSBuild\Current\Bin\MSBuild.exe" (
        set "MSBUILD_EXE=!AHKDECK_MSVC_ROOT!\..\..\..\..\MSBuild\Current\Bin\MSBuild.exe"
    ) else if exist "!AHKDECK_MSVC_ROOT!\..\..\..\MSBuild\Current\Bin\MSBuild.exe" (
        set "MSBUILD_EXE=!AHKDECK_MSVC_ROOT!\..\..\..\MSBuild\Current\Bin\MSBuild.exe"
    )
)
if not defined MSBUILD_EXE (
    for %%A in (MSBuild.exe) do set "MSBUILD_EXE=%%~$PATH:A"
)
if not exist "!MSBUILD_EXE!" (
    echo   [ERROR] MSBuild.exe not found next to the VC toolset
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

mkdir "%OFFICIAL_OUT%" 2>nul
mkdir "%KERNEL_BUILD%\official_obj" 2>nul

echo   building official kernel - Release / x64...
REM OutDir/IntDir must end in a backslash (cl.exe's /Fo needs it), so the quoted
REM value ends with a doubled backslash: the pair resolves to one literal "\".
"%MSBUILD_EXE%" "%OFFICIAL_SRC%\AutoHotkeyx.sln" /p:Configuration=Release /p:Platform=x64 /p:OutDir="%KERNEL_BUILD%\official_bin\\" /p:IntDir="%KERNEL_BUILD%\official_obj\\" /m /v:minimal
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] official kernel build failed
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

if not exist "%KERNEL_BUILD%\official_bin\AutoHotkey64.exe" (
    echo   [ERROR] official kernel build produced no AutoHotkey64.exe
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
copy /y "%KERNEL_BUILD%\official_bin\AutoHotkey64.exe" "%OFFICIAL_EXE%" >nul

REM ---- .3rd must stay source-only ------------------------------------------
for /d /r "%OFFICIAL_SRC%" %%D in (temp) do (
    if exist "%%D" rmdir /s /q "%%D"
)

echo   official kernel: .temp\kernel_official\AutoHotkey64.exe
echo.

:done
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0
