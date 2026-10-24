$ErrorActionPreference = 'Stop'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$idf = 'C:\Users\zergi\.platformio\packages\framework-espidf'
$python = 'C:\Users\zergi\.platformio\penv\Scripts\python.exe'
$gccDir = 'C:\Users\zergi\.platformio\packages\toolchain-xtensa-esp-elf\bin'
$cmakeDir = 'C:\Users\zergi\.platformio\packages\tool-cmake\bin'
$ninjaDir = 'C:\Users\zergi\.platformio\packages\tool-ninja'
$idfPy = Join-Path $idf 'tools\idf.py'

foreach ($path in @($idf, $python, $gccDir, $cmakeDir, $ninjaDir, $idfPy)) {
    if (-not (Test-Path $path)) { throw "Required FNK tool path is missing: $path" }
}
$version = (Get-Content (Join-Path $idf 'version.txt') -Raw).Trim()
if ($version -ne '6.1.0') { throw "Expected PlatformIO ESP-IDF 6.1.0, found '$version'" }
$gcc = Join-Path $gccDir 'xtensa-esp-elf-g++.exe'
$gccVersion = (& $gcc --version 2>&1 | Select-Object -First 1)
if ($gccVersion -notmatch '15\.2\.0|esp-15\.2\.0_20251204') { throw "Unexpected Xtensa compiler: $gccVersion" }
if ((& $python --version 2>&1) -notmatch 'Python 3\.11') { throw 'Unexpected FNK Python version' }
& $python -c 'import yaml' 2>$null
if ($LASTEXITCODE -ne 0) { throw 'FNK Python environment lacks PyYAML; refusing to change or install dependencies' }
$env:IDF_PATH = $idf
$env:IDF_PYTHON_ENV_PATH = Split-Path $python -Parent | Split-Path -Parent
$env:ESP_IDF_VERSION = '6.1.0'
$env:PYTHONUTF8 = '1'
$env:PATH = "$gccDir;$cmakeDir;$ninjaDir;$(Split-Path $python -Parent);$env:PATH"
if (-not (Test-Path (Join-Path $project 'sdkconfig.defaults'))) { throw 'FNK sdkconfig.defaults is missing' }
$cache = Join-Path $project 'build\CMakeCache.txt'
if (Test-Path $cache) {
    $cacheText = Get-Content $cache -Raw
    if ($cacheText -match 'F:/espP4_AI/.toolchains|esp-idf-v6\.0\.2') {
        throw 'FNK build cache belongs to the P4 IDF 6.0.2 environment. Use a clean FNK build directory explicitly; the script will not delete it.'
    }
}
Push-Location $project
$lock = Join-Path $project 'dependencies.lock'
$lockBackup = Join-Path $env:TEMP 'fnk0104s-dependencies.lock.backup'
if (Test-Path $lock) { Copy-Item $lock $lockBackup -Force }
try {
    & $python 'scripts/build.py' 'freenove-fnk0104s' '--name' 'freenove-fnk0104s'
    if ($LASTEXITCODE -ne 0) { throw "FNK build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
    if (Test-Path $lockBackup) { Copy-Item $lockBackup $lock -Force }
}
Write-Host "FNK build completed: $project\build\xiaozhi.bin"
