@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "PROF=%SCRIPT_DIR%..\build\os_live_profiler.exe"
if not exist "%PROF%" (
    echo [ERROR] Profiler not built yet: %PROF%
    echo Run: cmd /c E:\OpenScarlett\build_wt.cmd E:\OpenScarlett\.worktrees\mine-integ --build-only os_live_profiler
    exit /b 1
)
set "DUR=%~1"
if "%DUR%"=="" set "DUR=30"
echo ============================================================================
echo  OpenScarlett Live Profiler
echo  Duration: %DUR%s (pass 0 for continuous until Ctrl+C)
echo  Outputs : E:\OpenScarlett\.tmp\perf_profile\latest_resources.csv
echo            E:\OpenScarlett\.tmp\perf_profile\latest_hotspots.txt
echo ============================================================================
"%PROF%" --process Minecraft.Windows.exe --duration %DUR% --interval-ms 5
endlocal
