@echo off
setlocal
set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

set "WT_DIR=%~1"
if "%WT_DIR%"=="" (
    echo Usage: build_wt.cmd ^<worktree_dir^> [--build-only [targets...] ^| --test ^<regex^> ^| --cache-stats ^| command...]
    exit /b 1
)
for %%I in ("%WT_DIR%") do set "WT_NAME=%%~nxI"
set "TEMP=%ROOT_DIR%\.tmp\%WT_NAME%"
set "TMP=%ROOT_DIR%\.tmp\%WT_NAME%"
set "OPENSCARLETT_SAVE_ROOT=%ROOT_DIR%\.tmp\%WT_NAME%\saves"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%"

rem Share a single sccache store on E:\ across main and ALL worktrees so fresh
rem worktree builds reuse compiled .obj files instead of rebuilding from scratch.
if not defined SCCACHE_DIR set "SCCACHE_DIR=%ROOT_DIR%\.sccache"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=10G"
if not defined SCCACHE_IDLE_TIMEOUT set "SCCACHE_IDLE_TIMEOUT=3600"
if not exist "%SCCACHE_DIR%" mkdir "%SCCACHE_DIR%"

if not defined OPENSCARLETT_BUILD_JOBS set "OPENSCARLETT_BUILD_JOBS=6"
if not defined OPENSCARLETT_CTEST_JOBS set "OPENSCARLETT_CTEST_JOBS=4"

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" where cl.exe >nul 2>&1 && goto :have_msvc
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
:have_msvc

if exist "E:\ScarlettX-dev\tools\sccache\sccache.exe" set "PATH=E:\ScarlettX-dev\tools\sccache;%PATH%"
set "PATH=E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin;E:\ScarlettX-dev\tools\ninja;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%"

cd /d "%WT_DIR%"

if "%~2"=="" goto :default_build
if /i "%~2"=="--cache-stats" goto :cache_stats
if /i "%~2"=="--build-only" goto :build_only
if /i "%~2"=="--test" goto :test_subset

rem Preserve '=' characters in custom commands (e.g. -DCMAKE_BUILD_TYPE=Release)
rem by stripping only the first token from %* instead of iterating shift/%1.
for /f "tokens=1,* delims= " %%A in ("%*") do set "CMDLINE=%%B"
%CMDLINE%
exit /b %ERRORLEVEL%

:default_build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel %OPENSCARLETT_BUILD_JOBS% && ctest --test-dir build --parallel %OPENSCARLETT_CTEST_JOBS% --output-on-failure
exit /b %ERRORLEVEL%

:cache_stats
sccache --show-stats
exit /b %ERRORLEVEL%

:build_only
if not exist "build\build.ninja" (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b %ERRORLEVEL%
)
shift
shift
if "%~1"=="" (
    cmake --build build --parallel %OPENSCARLETT_BUILD_JOBS%
    exit /b %ERRORLEVEL%
)
set "TARGETS="
:wt_bo_loop
if "%~1"=="" goto :wt_bo_run
set "TARGETS=%TARGETS% %~1"
shift
goto :wt_bo_loop
:wt_bo_run
cmake --build build --parallel %OPENSCARLETT_BUILD_JOBS% --target %TARGETS%
exit /b %ERRORLEVEL%

:test_subset
if "%~3"=="" (
    echo Usage: build_wt.cmd ^<worktree_dir^> --test ^<ctest_regex^>
    exit /b 1
)
if not exist "build\build.ninja" (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b %ERRORLEVEL%
)
cmake --build build --parallel %OPENSCARLETT_BUILD_JOBS% && ctest --test-dir build --parallel %OPENSCARLETT_CTEST_JOBS% -R "%~3" --output-on-failure
exit /b %ERRORLEVEL%
