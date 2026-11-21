@echo off
setlocal
set "WT_DIR=%~1"
shift
if "%WT_DIR%"=="" (
    echo Usage: build_wt.cmd ^<worktree_dir^> [command...]
    exit /b 1
)
for %%I in ("%WT_DIR%") do set "WT_NAME=%%~nxI"
set "TEMP=E:\OpenScarlett\.tmp\%WT_NAME%"
set "TMP=E:\OpenScarlett\.tmp\%WT_NAME%"
set "OPENSCARLETT_SAVE_ROOT=E:\OpenScarlett\.tmp\%WT_NAME%\saves"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin;E:\ScarlettX-dev\tools\ninja;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%"
cd /d "%WT_DIR%"
if "%~1"=="" goto :default_build
set "CMDLINE="
:loop
if "%~1"=="" goto :run_cmd
set "CMDLINE=%CMDLINE% %1"
shift
goto :loop
:run_cmd
%CMDLINE%
exit /b %ERRORLEVEL%

:default_build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 4 && ctest --test-dir build --output-on-failure
exit /b %ERRORLEVEL%
