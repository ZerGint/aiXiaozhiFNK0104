# Mega Drive PAPP: экспериментальный native-resolution видеотракт

Дата: 2026-10-25. Реализация и проверки выполнены в отдельном worktree `experiment/megadrive-native-video`. Устройство не прошивалось; SD и установленный release не изменялись.

## Результат

Добавлен экспериментальный вариант Mega Drive PAPP, который сохраняет активную геометрию Gwenesis (H40: 320×224 NTSC / 320×240 PAL; H32: 256×224), преобразует indexed framebuffer в RGB565 без предварительного ресайза и передаёт descriptor общего Game OS сервиса. Game OS выполняет конечный 2× transform/rotation в неактивный LCD framebuffer, затем использует существующие overlay, cache sync и present.

Старый callback и ABI v2 поля сохранены; новый callback добавлен в хвост структуры. Native PAPP проверяет `struct_size` и при отсутствии поля использует legacy путь. Legacy и Native варианты собираются раздельно. Native путь ограничен centered integer 2× геометрией текущего LCD и не меняет emulator core, pacing или аудио.

## Изменённые файлы

- `papp/include/papp_services.h`: tail descriptor с размером/stride/viewport и optional ABI size guard.
- `papp/megadrive/megadrive_papp.c`: native RGB565 conversion, legacy fallback и редкая агрегированная telemetry.
- `game_os/main/megadrive_native_video_geometry.h`: проверка bounds и геометрии logical landscape → physical portrait после rotation 270°.
- `game_os/main/waveshare_game_hw.c`: native callback в inactive framebuffer с PPA scale/rotate и CPU nearest fallback; callback синхронный, source pointer не сохраняется.
- `tools/build_megadrive_papp.ps1`: `-VideoMode Legacy|Native` и отдельный output metadata.
- `tests/test_megadrive_native_video.c`: host проверки геометрии NTSC/PAL/H32, stride/bounds и ABI guard.

Native ROI в логическом 800×480: NTSC 320×224 → (80,16), 640×448; PAL 320×240 → (80,0), 640×480; H32 256×224 → (144,16), 512×448. Physical ROI после rotation: NTSC (16,80), 448×640; PAL (0,80), 480×640; H32 (16,144), 448×512.

## Проверки и артефакты

- Host test с GCC `-std=c11 -Wall -Wextra -Werror` завершился успешно.
- Legacy PAPP: сборка GCC 15.2.0 и PAPP v2 verification успешно; 737194 байта, CRC32 `9787635e`, SHA-256 `86D22D7CE8…`.
- Native PAPP: сборка GCC 15.2.0 и PAPP v2 verification успешно; 737434 байта, CRC32 `5ac79234`, SHA-256 `0E05F3289F…`.
- Game OS успешно собран штатным скриптом `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build_game_os.ps1 -NoClean` в этом worktree, на закреплённом ESP-IDF 6.0.2 / GCC 15.2.0. Артефакт `game_os/build/waveshare_p4_game_os.bin`: 1,111,712 байт (`0x10f6a0`), SHA-256 `CB37B180BD5FFE1D75C182CA787CBBC6E4A53429B8FEECB711241B772BA21E54`. `esptool image-info`: ESP32-P4, DIO/80 MHz/32 MB, min chip revision v3.1, checksum и validation hash valid; app partition 8 MiB, свободно 87%.
- Не выполнялись flash, monitor, запись SD или аппаратный runtime A/B. Сборка подтверждает компиляцию, но не визуальную/временную корректность PPA на панели.

PAPP артефакты в `out/megadrive-native-ab/{legacy,native}`. Прежний baseline: PAPP 730202 байта, SHA-256 `6DF26578BC1470E45BE88904DAD57E7A1A2F93EF03B13685BE560EBE34EEDA60`.

## Осталось проверить на устройстве

Проверить изображение и границы для H40 NTSC/PAL и H32, fallback с ABI v2 без нового callback, стабильность длительного запуска, callback latency, LCD cadence, PPA wait и heap high-water. Сравнить резкость pixel art с legacy путём. До такого A/B нельзя заключать, что native path повышает FPS; 60 FPS не обещается.
