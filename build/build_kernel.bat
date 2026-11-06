@echo off
REM ============================================================================
REM build_kernel.bat - build the patched AHK_H kernel DLL (ASCII only)
REM ----------------------------------------------------------------------------
REM Outputs (directory contract from AGENTS.md):
REM   .temp\kernel\AutoHotkey64.dll      <- canonical kernel, copied into .build
REM   .temp\kernel_build\                <- MSBuild intermediates (never in .3rd)
REM
REM Configuration: MT_ReleaseDll (the DLL form that supports in-process script
REM threads). Always build through the SLN: lib_pcre / lib_double have no
REM ReleaseDll configuration of their own and a direct vcxproj build fails with
REM MSB8013.
REM
REM The build leaves asm/lib staging dirs inside .3rd (temp\<platform>, 
REM source\lib_*\temp); they are deleted here so .3rd stays source-only.
REM
REM Environment:
REM   AHKDECK_CHAINED=1       suppress "press any key"
REM   AHKDECK_FORCE_KERNEL=1  rebuild even when the DLL looks up to date
REM ============================================================================

setlocal enabledelayedexpansion

call "%~dp0_common.bat"
if %ERRORLEVEL% neq 0 (
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

echo ======================================
echo    AHKDeck  [4a/5] kernel DLL
echo ======================================
echo.

set "KERNEL_SRC=%THIRDPARTY%\AutoHotkey_H-v2.0.25"
set "KERNEL_OUT=%PROJECT_ROOT%\.temp\kernel"
set "KERNEL_DLL=%KERNEL_OUT%\AutoHotkey64.dll"
set "KERNEL_BUILD=%PROJECT_ROOT%\.temp\kernel_build"

if not exist "%KERNEL_SRC%\AutoHotkeyx.sln" (
    echo   [ERROR] .3rd\AutoHotkey_H-v2.0.25 is missing - run 2_download.bat
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

REM ---- up-to-date check: any patch newer than the DLL forces a rebuild --------
set "KERNEL_NEED=0"
if not exist "%KERNEL_DLL%" (
    set "KERNEL_NEED=1"
) else (
    set "KERNEL_TS="
    for %%A in ("%KERNEL_DLL%") do set "KERNEL_TS=%%~tA"
    for %%f in ("%PATCHES%\AutoHotkey_H-v2.0.25\source\*") do (
        if "%%~tf" gtr "!KERNEL_TS!" set "KERNEL_NEED=1"
    )
)
if "!AHKDECK_FORCE_KERNEL!"=="1" set "KERNEL_NEED=1"

if "!KERNEL_NEED!"=="0" (
    echo   kernel DLL is up to date
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

mkdir "%KERNEL_OUT%" 2>nul
mkdir "%KERNEL_BUILD%\obj" 2>nul

echo   building kernel - MT_ReleaseDll / x64...
REM OutDir/IntDir must end in a backslash (cl.exe's /Fo needs it), so the quoted
REM value ends with a doubled backslash: the pair resolves to one literal "\" and
REM lets the closing quote terminate the argument.
"%MSBUILD_EXE%" "%KERNEL_SRC%\AutoHotkeyx.sln" /p:Configuration=MT_ReleaseDll /p:Platform=x64 /p:OutDir="%KERNEL_BUILD%\bin\\" /p:IntDir="%KERNEL_BUILD%\obj\\" /m /v:minimal
if !ERRORLEVEL! neq 0 (
    echo   [ERROR] kernel build failed
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)

if not exist "%KERNEL_BUILD%\bin\AutoHotkey64.dll" (
    echo   [ERROR] kernel build produced no AutoHotkey64.dll
    if not "!AHKDECK_CHAINED!"=="1" pause
    exit /b 1
)
copy /y "%KERNEL_BUILD%\bin\AutoHotkey64.dll" "%KERNEL_DLL%" >nul

REM ---- .3rd must stay source-only ------------------------------------------
for /d /r "%KERNEL_SRC%" %%D in (temp) do (
    if exist "%%D" rmdir /s /q "%%D"
)

echo   kernel DLL: .temp\kernel\AutoHotkey64.dll
echo.

:done
if not "!AHKDECK_CHAINED!"=="1" pause
exit /b 0
