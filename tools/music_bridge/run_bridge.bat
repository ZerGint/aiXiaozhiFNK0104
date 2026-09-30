@echo off
setlocal
cd /d "%~dp0\..\.."

if exist "%~dp0.venv\Scripts\python.exe" (
  "%~dp0.venv\Scripts\python.exe" -m tools.music_bridge.app
) else (
  echo Bridge venv not found. Create it with:
  echo   python -m venv tools\music_bridge\.venv
  echo   tools\music_bridge\.venv\Scripts\python.exe -m pip install -r tools\music_bridge\requirements.txt
  exit /b 1
)
