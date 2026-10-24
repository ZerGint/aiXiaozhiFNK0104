# FNK0104S build guide

This guide records the confirmed original production workflow. It is isolated
from the ESP32-P4 IDF 6.0.2 environment.

## Evidence and pinned paths

Earlier FNK release metadata and build reports identify ESP-IDF 6.1.0. The
installed PlatformIO packages are:

| Item | Value |
|---|---|
| ESP-IDF | `C:\Users\zergi\.platformio\packages\framework-espidf` (6.1.0) |
| Python | `C:\Users\zergi\.platformio\penv\Scripts\python.exe` (contains PyYAML 6.0.3) |
| Xtensa GCC | `C:\Users\zergi\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp-elf-g++.exe` |
| GCC | 15.2.0, esp-15.2.0_20251204 |
| Target | `esp32s3` |
| Board | `freenove/fnk0104s` |

The repository also contains later logs built with the shared P4 IDF 6.0.2
environment. Those logs are not the canonical FNK procedure. This script
deliberately selects PlatformIO's 6.1.0 paths and refuses another IDF.

## Build

From the repository root run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build_fnk.ps1

:: or use the policy-independent wrapper
build_fnk.cmd
```

The script sets only process-local variables, validates the framework version,
Python, Xtensa compiler, target and board, and invokes:

```text
python scripts/build.py freenove-fnk0104s --name freenove-fnk0104s
```

It never flashes, erases, monitors, changes partition tables, or updates the
ESP32-C6. Do not manually merge P4 and FNK PATH entries.

## Known issues

If the script reports IDF 6.0.2, a P4 `.toolchains` path, RISC-V GCC, or a
missing PlatformIO 6.1.0 package, stop. Do not reinstall automatically and do
not change application code. The current working tree contains historical
6.0.2 logs; use them only for diagnosis, not as the selected procedure.
