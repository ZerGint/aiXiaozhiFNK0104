$ErrorActionPreference = "Stop"

$bridgeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $bridgeDir)
$python = Join-Path $bridgeDir ".venv\Scripts\python.exe"

if (-not (Test-Path -LiteralPath $python)) {
    throw "Bridge virtual environment not found: $python"
}

Push-Location $repoRoot
try {
    & $python -m pip install -r (Join-Path $bridgeDir "requirements.txt") pyinstaller
    & $python -m PyInstaller --noconfirm --clean --onefile --console `
        --name Yue2Provider `
        --distpath tools\music_bridge\providers\yue2 `
        --workpath build\Yue2Provider `
        --specpath tools\music_bridge\providers\yue2 `
        --collect-data gradio_client `
        --paths $repoRoot `
        tools\music_bridge\providers\yue2\adapter.py
}
finally {
    Pop-Location
}
