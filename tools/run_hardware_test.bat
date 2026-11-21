@echo off
setlocal enabledelayedexpansion

set "BUNDLE_DIR=%~dp0"
if "%BUNDLE_DIR:~-1%"=="\" set "BUNDLE_DIR=%BUNDLE_DIR:~0,-1%"
set "REPORT=%BUNDLE_DIR%\hardware_test_report.txt"
set "NO_PAUSE=0"
if /I "%~1"=="--no-pause" set "NO_PAUSE=1"

set "OPENSCARLETT_SAVE_ROOT=%TEMP%\openscarlett_hw_test_saves"
set "OPENSCARLETT_AUDIO_NULL=1"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%" >nul 2>&1

pushd "%BUNDLE_DIR%" >nul

> "%REPORT%" (
    echo ============================================================
    echo OpenScarlett Portable Hardware ^& Self-Test Report
    echo ============================================================
    ver
    echo.
    echo --- GPU ^& Toolchain Probe ---
)
"%BUNDLE_DIR%\openscarlett.exe" probe-gpu >> "%REPORT%" 2>&1
set "PROBE_RC=!ERRORLEVEL!"

>> "%REPORT%" (
    echo.
    echo --- Automated Test Binaries ---
)

set /a TOTAL=0
set /a PASSED=0
set /a FAILED=0

if not "!PROBE_RC!"=="0" (
    set /a TOTAL+=1
    set /a FAILED+=1
    echo [FAIL] openscarlett.exe probe-gpu ^(exit !PROBE_RC!^)
) else (
    set /a TOTAL+=1
    set /a PASSED+=1
    echo [PASS] openscarlett.exe probe-gpu
)

for %%T in ("%BUNDLE_DIR%\test_*.exe") do (
    set "TNAME=%%~nxT"
    if /I not "!TNAME!"=="test_guest_dummy.exe" (
        set /a TOTAL+=1
        echo Running !TNAME!...
        >> "%REPORT%" echo [RUN] !TNAME!
        "%%~fT" >> "%REPORT%" 2>&1
        set "RC=!ERRORLEVEL!"
        if "!RC!"=="0" (
            set /a PASSED+=1
            echo [PASS] !TNAME!
            >> "%REPORT%" echo [RESULT] !TNAME!: PASS
        ) else (
            set /a FAILED+=1
            echo [FAIL] !TNAME! ^(exit !RC!^)
            >> "%REPORT%" echo [RESULT] !TNAME!: FAIL ^(exit !RC!^)
        )
    )
)

>> "%REPORT%" (
    echo.
    echo ============================================================
    echo Summary: !PASSED!/!TOTAL! checks passed ^(!FAILED! failed^)
    echo ============================================================
)

echo.
echo ============================================================
echo OpenScarlett Hardware Test Summary: !PASSED!/!TOTAL! passed ^(!FAILED! failed^)
echo Report saved to: %REPORT%
echo ============================================================

popd >nul

if "!NO_PAUSE!"=="0" (
    echo.
    pause
)

if !FAILED! GTR 0 exit /b 1
exit /b 0
