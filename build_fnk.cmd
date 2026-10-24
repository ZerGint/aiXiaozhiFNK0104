@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_fnk.ps1" %*
exit /b %errorlevel%
