$ErrorActionPreference = "Stop"

$bridgeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $bridgeDir)
$python = Join-Path $bridgeDir ".venv\Scripts\python.exe"

if (-not (Test-Path $python)) {
    throw "Bridge virtual environment not found: $python"
}

& $python -c "import tkinter"
if ($LASTEXITCODE -ne 0) {
    throw "This Python environment has no Tcl/Tk support. Create the bridge .venv with a full Windows CPython installation before building the desktop EXE."
}

Push-Location $repoRoot
try {
    & $python -m pip install -r (Join-Path $bridgeDir "requirements.txt") pyinstaller
    & $python -m PyInstaller --noconfirm --clean --onefile --windowed `
        --name FNKMusicBridge `
        --specpath tools\music_bridge `
        --collect-all pygame `
        --collect-all pystray `
        --collect-all PIL `
        --collect-data gradio_client `
        --paths $repoRoot `
        tools\music_bridge\desktop_app.py
}
finally {
    Pop-Location
}
