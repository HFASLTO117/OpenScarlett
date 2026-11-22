@echo off
setlocal
set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "TEMP=%ROOT_DIR%\.tmp"
set "TMP=%ROOT_DIR%\.tmp"
set "OPENSCARLETT_SAVE_ROOT=%ROOT_DIR%\.tmp\saves"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%"

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
if exist "E:\ScarlettX-dev\tools\ninja\ninja.exe" set "PATH=E:\ScarlettX-dev\tools\ninja;%PATH%"
if exist "E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin\cmake.exe" set "PATH=E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin;%PATH%"

rem dxc.exe from the newest installed Windows 10/11 SDK.
set "SDK_BIN="
for /d %%D in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do (
    if exist "%%~D\x64\dxc.exe" set "SDK_BIN=%%~D\x64"
)
if defined SDK_BIN set "PATH=%SDK_BIN%;%PATH%"

cd /d "%ROOT_DIR%"
if not "%~1"=="" goto :custom
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel && ctest --test-dir build --output-on-failure
exit /b %ERRORLEVEL%

:custom
%*
exit /b %ERRORLEVEL%
