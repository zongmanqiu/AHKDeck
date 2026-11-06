@echo off
REM ============================================================================
REM msvc_env.bat - locate MSVC and set up the build environment (ASCII only)
REM ----------------------------------------------------------------------------
REM Called by _common.bat. Must be invoked with "call" so that the environment
REM changes persist in the caller's cmd session. On success sets:
REM   AHKDECK_MSVC_BIN  - dir containing cl.exe / link.exe (Hostx64\x64)
REM   AHKDECK_MSVC_LIB  - dir containing MSVC x64 libs
REM   AHKDECK_MSVC_INC  - dir containing MSVC headers
REM   AHKDECK_SDK_BIN   - dir containing rc.exe (Windows SDK x64 tools)
REM   AHKDECK_SDK_LIB   - x64 lib dirs of the Windows SDK (ucrt + um)
REM   AHKDECK_SDK_INC   - include dirs of the Windows SDK (ucrt + um + shared)
REM and prepends the compiler/SDK bins to PATH, sets INCLUDE / LIB.
REM Exits with errorlevel 1 if MSVC or the Windows SDK cannot be found.
REM
REM Why manual probing instead of vcvars64.bat / the "Visual Studio 17 2022"
REM CMake generator: on some machines the VS generator cannot locate the
REM compiler (it relies on a developer prompt / registry probing that fails in
REM a plain cmd). NMake Makefiles + a manually built environment is stable.
REM ============================================================================

REM NOTE: no setlocal here on purpose - the caller needs the variables.

REM ---- 1. locate MSVC toolchain dir (newest version under each root) ---------
REM Discovery order:
REM   a) AHKDECK_MSVC_ROOT already set by the caller (explicit override)
REM   b) vswhere.exe - the official VS locator; works for ANY install path
REM   c) a short list of default install paths (fallback when vswhere is absent)
if defined AHKDECK_MSVC_ROOT (
    if exist "%AHKDECK_MSVC_ROOT%\bin\Hostx64\x64\cl.exe" goto :msvc_ok
    set "AHKDECK_MSVC_ROOT="
)

REM NOTE: use a literal path instead of %ProgramFiles(x86)% - cmd's parser
REM terminates the variable name at the ")" and the expansion silently breaks.
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "AHKDECK_VS_INSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "AHKDECK_VS_INSTALL=%%I"
    )
    REM Fall back to any install if the VC tools component query returns nothing.
    if not defined AHKDECK_VS_INSTALL (
        for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
            set "AHKDECK_VS_INSTALL=%%I"
        )
    )
)
if defined AHKDECK_VS_INSTALL (
    if exist "%AHKDECK_VS_INSTALL%\VC\Tools\MSVC" (
        for /f "delims=" %%V in ('dir /b /ad /o-n "%AHKDECK_VS_INSTALL%\VC\Tools\MSVC" 2^>nul') do (
            if not defined AHKDECK_MSVC_ROOT set "AHKDECK_MSVC_ROOT=%AHKDECK_VS_INSTALL%\VC\Tools\MSVC\%%V"
        )
    )
)

if not defined AHKDECK_MSVC_ROOT (
    for %%P in (
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    ) do (
        if not defined AHKDECK_MSVC_ROOT (
            if exist "%%~P\VC\Tools\MSVC" (
                for /f "delims=" %%V in ('dir /b /ad /o-n "%%~P\VC\Tools\MSVC" 2^>nul') do (
                    if not defined AHKDECK_MSVC_ROOT set "AHKDECK_MSVC_ROOT=%%~P\VC\Tools\MSVC\%%V"
                )
            )
        )
    )
)
if not defined AHKDECK_MSVC_ROOT (
    echo   [ERROR] MSVC toolchain not found.
    echo           Install Visual Studio 2022 or the MSVC Build Tools,
    echo           or set AHKDECK_MSVC_ROOT to the "..\VC\Tools\MSVC\<ver>" dir.
    exit /b 1
)
:msvc_ok
echo   MSVC: %AHKDECK_MSVC_ROOT%

REM ---- 2. locate Windows SDK (newest version) --------------------------------
if defined AHKDECK_SDK_ROOT (
    if exist "%AHKDECK_SDK_ROOT%\Include" goto :sdk_ok
    set "AHKDECK_SDK_ROOT="
)
for %%P in (
    "C:\Program Files (x86)\Windows Kits\10"
    "C:\Program Files\Windows Kits\10"
) do (
    if not defined AHKDECK_SDK_ROOT if exist "%%~P\Include" set "AHKDECK_SDK_ROOT=%%~P"
)
if not defined AHKDECK_SDK_ROOT (
    echo   [ERROR] Windows SDK not found.
    echo           Install the Windows 10/11 SDK, or set AHKDECK_SDK_ROOT to it.
    exit /b 1
)
:sdk_ok
set "AHKDECK_SDK_VER="
for /f "delims=" %%V in ('dir /b /ad /o-n "%AHKDECK_SDK_ROOT%\Include" 2^>nul') do (
    if not defined AHKDECK_SDK_VER set "AHKDECK_SDK_VER=%%V"
)
if not defined AHKDECK_SDK_VER (
    echo   [ERROR] Windows SDK version dir not found.
    exit /b 1
)
echo   SDK : %AHKDECK_SDK_ROOT% (ver %AHKDECK_SDK_VER%)

REM ---- 3. export the pieces ---------------------------------------------------
set "AHKDECK_MSVC_BIN=%AHKDECK_MSVC_ROOT%\bin\Hostx64\x64"
set "AHKDECK_MSVC_LIB=%AHKDECK_MSVC_ROOT%\lib\x64"
set "AHKDECK_MSVC_INC=%AHKDECK_MSVC_ROOT%\include"
set "AHKDECK_SDK_BIN=%AHKDECK_SDK_ROOT%\bin\%AHKDECK_SDK_VER%\x64"
set "AHKDECK_SDK_LIB=%AHKDECK_SDK_ROOT%\Lib\%AHKDECK_SDK_VER%\ucrt\x64;%AHKDECK_SDK_ROOT%\Lib\%AHKDECK_SDK_VER%\um\x64"
set "AHKDECK_SDK_INC=%AHKDECK_SDK_ROOT%\Include\%AHKDECK_SDK_VER%\ucrt;%AHKDECK_SDK_ROOT%\Include\%AHKDECK_SDK_VER%\um;%AHKDECK_SDK_ROOT%\Include\%AHKDECK_SDK_VER%\shared"

if not exist "%AHKDECK_MSVC_BIN%\cl.exe" (
    echo   [ERROR] cl.exe not found at %AHKDECK_MSVC_BIN%
    exit /b 1
)

set "PATH=%AHKDECK_MSVC_BIN%;%AHKDECK_SDK_BIN%;%PATH%"
set "INCLUDE=%AHKDECK_MSVC_INC%;%AHKDECK_SDK_INC%"
set "LIB=%AHKDECK_MSVC_LIB%;%AHKDECK_SDK_LIB%"

echo   cl  : %AHKDECK_MSVC_BIN%\cl.exe
echo   PATH / INCLUDE / LIB set for NMake builds.
exit /b 0
