@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_hardware_test.ps1" %*
exit /b %ERRORLEVEL%

