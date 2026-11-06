@echo off
REM ============================================================================
REM full.bat - one-click pipeline: 1_check_env : 2_download : 3_patch :
REM            4_build : 5_test. Any step failing stops the chain.
REM ----------------------------------------------------------------------------
REM Each numbered script suppresses its "press any key" when AHKDECK_CHAINED=1.
REM ============================================================================

setlocal

set "AHKDECK_CHAINED=1"

echo.
echo ==========================================
echo    AHKDeck full pipeline
echo ==========================================

call "%~dp01_check_env.bat"
if %ERRORLEVEL% neq 0 goto :fail

call "%~dp02_download.bat"
if %ERRORLEVEL% neq 0 goto :fail

call "%~dp03_patch.bat"
if %ERRORLEVEL% neq 0 goto :fail

call "%~dp04_build.bat"
if %ERRORLEVEL% neq 0 goto :fail

call "%~dp05_test.bat"
if %ERRORLEVEL% neq 0 goto :fail

echo.
echo ==========================================
echo    AHKDeck pipeline: ALL GREEN
echo    output: [project root]\.build\AHKDeck.exe
echo ==========================================
echo.
endlocal
exit /b 0

:fail
echo.
echo ==========================================
echo    AHKDeck pipeline: FAILED (see above)
echo ==========================================
echo.
endlocal
exit /b 1
