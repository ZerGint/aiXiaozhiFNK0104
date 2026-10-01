@echo off
setlocal
cd /d "%~dp0\..\.."

if not exist "%~dp0.venv\Scripts\python.exe" (
  echo Bridge venv not found. Create it with:
  echo   python -m venv tools\music_bridge\.venv
  echo   tools\music_bridge\.venv\Scripts\python.exe -m pip install -r tools\music_bridge\requirements.txt
  exit /b 1
)

"%~dp0.venv\Scripts\python.exe" -m tools.music_bridge.desktop_app
