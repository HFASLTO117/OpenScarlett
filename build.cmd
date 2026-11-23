@echo off
setlocal
set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "TEMP=%ROOT_DIR%\.tmp"
set "TMP=%ROOT_DIR%\.tmp"
set "OPENSCARLETT_SAVE_ROOT=%ROOT_DIR%\.tmp\saves"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%"

rem Shared sccache directory strictly on E:\ (or repo root drive) across main & worktrees.
if not defined SCCACHE_DIR set "SCCACHE_DIR=%ROOT_DIR%\.sccache"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=10G"
if not defined SCCACHE_IDLE_TIMEOUT set "SCCACHE_IDLE_TIMEOUT=3600"
if not exist "%SCCACHE_DIR%" mkdir "%SCCACHE_DIR%"

if not defined OPENSCARLETT_CTEST_JOBS set "OPENSCARLETT_CTEST_JOBS=4"

rem MSVC: keep an already initialised x64 developer prompt, otherwise ask vswhere
rem for the newest install with the x64 C++ tools (Community, Professional,
rem Enterprise or Build Tools, any version).
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" where cl.exe >nul 2>&1 && goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_DIR="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%I"
)
if not defined VS_DIR if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined VS_DIR (
    echo build.cmd: no Visual Studio install with the x64 C++ tools was found.
    echo Install the "Desktop development with C++" workload, or run this from a
    echo "x64 Native Tools Command Prompt".
    exit /b 1
)
rem vcvars64.bat runs vswhere.exe by name, so put the installer folder on PATH.
if exist "%VSWHERE%" set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
rem Use the CMake and Ninja that come with this Visual Studio install.
set "VS_CMAKE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VS_CMAKE%\CMake\bin\cmake.exe" set "PATH=%VS_CMAKE%\CMake\bin;%PATH%"
if exist "%VS_CMAKE%\Ninja\ninja.exe" set "PATH=%VS_CMAKE%\Ninja;%PATH%"
:have_msvc

rem Optional pinned toolchain; used first when present.
if exist "E:\ScarlettX-dev\tools\sccache\sccache.exe" set "PATH=E:\ScarlettX-dev\tools\sccache;%PATH%"
if exist "E:\ScarlettX-dev\tools\ninja\ninja.exe" set "PATH=E:\ScarlettX-dev\tools\ninja;%PATH%"
if exist "E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe" set "PATH=E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin;%PATH%"

rem dxc.exe from the newest installed Windows 10/11 SDK.
set "SDK_BIN="
for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do (
    if exist "%%~D\x64\dxc.exe" set "SDK_BIN=%%~D\x64"
)
if defined SDK_BIN set "PATH=%SDK_BIN%;%PATH%"

set "BUILD_PAR=--parallel"
if defined OPENSCARLETT_BUILD_JOBS set "BUILD_PAR=--parallel %OPENSCARLETT_BUILD_JOBS%"

cd /d "%ROOT_DIR%"
if /i "%~1"=="--cache-stats" goto :cache_stats
if /i "%~1"=="--build-only" goto :build_only
if /i "%~1"=="--test" goto :test_subset
if not "%~1"=="" goto :custom

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build %BUILD_PAR% && ctest --test-dir build --parallel %OPENSCARLETT_CTEST_JOBS% --output-on-failure
exit /b %ERRORLEVEL%

:cache_stats
sccache --show-stats
exit /b %ERRORLEVEL%

:build_only
if not exist "build\build.ninja" (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b %ERRORLEVEL%
)
shift
if "%~1"=="" (
    cmake --build build %BUILD_PAR%
    exit /b %ERRORLEVEL%
)
set "TARGETS="
:bo_loop
if "%~1"=="" goto :bo_run
set "TARGETS=%TARGETS% %~1"
shift
goto :bo_loop
:bo_run
cmake --build build %BUILD_PAR% --target %TARGETS%
exit /b %ERRORLEVEL%

:test_subset
if "%~2"=="" (
    echo Usage: build.cmd --test ^<ctest_regex^>
    exit /b 1
)
if not exist "build\build.ninja" (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b %ERRORLEVEL%
)
cmake --build build %BUILD_PAR% && ctest --test-dir build --parallel %OPENSCARLETT_CTEST_JOBS% -R "%~2" --output-on-failure
exit /b %ERRORLEVEL%

:custom
%*
exit /b %ERRORLEVEL%
