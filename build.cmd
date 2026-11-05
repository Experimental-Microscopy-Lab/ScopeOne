@echo off
rem Runs scripts\build.ps1 without changing the PowerShell execution policy of the machine, which
rem blocks .ps1 files on a new Windows installation. All arguments are passed on to the script.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" %*
exit /b %ERRORLEVEL%
