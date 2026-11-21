@echo off
setlocal
set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "TEMP=%ROOT_DIR%\.tmp"
set "TMP=%ROOT_DIR%\.tmp"
set "OPENSCARLETT_SAVE_ROOT=%ROOT_DIR%\.tmp\saves"
if not exist "%TEMP%" mkdir "%TEMP%"
if not exist "%OPENSCARLETT_SAVE_ROOT%" mkdir "%OPENSCARLETT_SAVE_ROOT%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=E:\ScarlettX-dev\tools\cmake\cmake-4.5.1-windows-x86_64\bin;E:\ScarlettX-dev\tools\ninja;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%"
cd /d "%ROOT_DIR%"
if not "%~1"=="" goto :custom
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel && ctest --test-dir build --output-on-failure
exit /b %ERRORLEVEL%

:custom
%*
exit /b %ERRORLEVEL%
