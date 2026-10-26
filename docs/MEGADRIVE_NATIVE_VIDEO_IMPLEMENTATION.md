# Mega Drive PAPP: экспериментальный native-resolution видеотракт

Дата: 2026-10-25. Реализация и проверки выполнены в отдельном worktree `experiment/megadrive-native-video`. Устройство не прошивалось; SD и установленный release не изменялись.

## Цель и границы

Добавлен экспериментальный вариант Mega Drive PAPP, который сохраняет активную геометрию Gwenesis (H40: 320×224 NTSC / 320×240 PAL; H32: 256×224), преобразует indexed framebuffer прямо в RGB565 без предварительного ресайза и передаёт descriptor общего сервиса. Game OS делает конечный 2× transform/rotation в неактивный LCD framebuffer, затем выполняет обычный overlay, cache sync и present.

Старый callback и ABI v2 поля не менялись; новый callback добавлен только в хвост структуры. Native PAPP проверяет `struct_size` и при отсутствии нового callback уходит в legacy преобразование и callback. Legacy и Native варианты собираются раздельно. Native вариант намеренно ограничен centered integer 2× геометрией текущего LCD пути; это не общий renderer для других систем и не изменение emulator core, кадрового pacing или аудио.

## Изменения

- `papp/include/papp_services.h`: tail descriptor с указателем, размером буфера, native width/height/stride, viewport и geometry enum; size guard для optional ABI field.
- `papp/megadrive/megadrive_papp.c`: native RGB565 conversion без canvas resample, ABI fallback, редкая агрегированная runtime telemetry. Legacy путь сохранён.
- `game_os/main/megadrive_native_video_geometry.h`: проверка допустимого источника/ROI и отображение landscape logical координат в portrait physical ROI после rotation 270°.
- `game_os/main/waveshare_game_hw.c`: native callback использует inactive FB и PPA scale/rotate; при ошибке PPA есть software nearest fallback; существующий present/overlay/cache pipeline остаётся единым. Callback синхронен и не сохраняет source pointer.
- `tools/build_megadrive_papp.ps1`: параметр `-VideoMode Legacy|Native`, фиксируемый в отдельном build output.
- `tests/test_megadrive_native_video.c`: host проверки геометрии NTSC/PAL/H32, stride/bounds и ABI tail size guard.

Native ROI в логическом холсте 800×480: NTSC 320×224 → (80,16), 640×448; PAL 320×240 → (80,0), 640×480; H32 256×224 → (144,16), 512×448. После физической rotation 270° соответствующие ROI: NTSC (16,80), 448×640; PAL (0,80), 480×640; H32 (16,144), 448×512.

## Артефакты и воспроизводимость

Ранний baseline, сохранённый до эксперимента в PAPP worktree: PAPP 730202 байта, SHA-256 `6DF26578BC1470E45BE88904DAD57E7A1A2F93EF03B13685BE560EBE34EEDA60`; ELF 833728 байт, SHA-256 `AAFDDA260A753C6F8BACD24A4250AC8A3DB959ED4B724B014F389DB0B7A6820D`; MAP 498230 байт, SHA-256 `BB26A2B7750703803653E2A84111712B6D6107EEBF9CC2B70554CEAB43531E98`.

Изолированные Legacy/Native артефакты находятся в `out/megadrive-native-ab/{legacy,native}`. Сборки обе прошли PAPP v2 verifier, с тем же script/toolchain и разными output dirs:

| Вариант | PAPP bytes | CRC32 | SHA-256 |
|---|---:|---|---|
| Legacy | 737194 | `9787635e` | `86D22D7CE8…` |
| Native | 737434 | `5ac79234` | `0E05F3289F…` |

Хеши выше приведены по выводу build script кратко; полные значения получить командой `Get-FileHash out/megadrive-native-ab/legacy/megadrive.papp -Algorithm SHA256` (аналогично для Native). Эти артефакты не копировались на SD.

## Проверки

- Host тест: `gcc -std=c11 -Wall -Wextra -Werror ... tests/test_megadrive_native_video.c`; завершился успешно.
- PAPP Legacy: собран ESP32-P4 RISC-V GCC 15.2.0, упаковщик подтвердил PAPP v2, размер 737194, CRC `9787635e`.
- PAPP Native: тот же toolchain/script; упаковщик подтвердил PAPP v2, размер 737434, CRC `5ac79234`.
- Game OS ESP-IDF build и физический hardware A/B не выполнены. В доступном окружении обнаружен IDF toolchain cache и прежний build output, но соответствующий ESP-IDF source checkout в ожидаемом каталоге отсутствует (там только `.git`); не использовался чужой build directory и SDK не подменялся.
- Не выполнялись flash, monitor, запись SD или изменение установленного release.

## Ограничения и следующий шаг

Это проверка кода/геометрии и двух PAPP компиляций, не подтверждение runtime корректности PPA. До deployment нужно собрать Game OS тем же ESP-IDF 6.0.2 окружением в отдельном output dir, затем на устройстве проверить картинку/границы для H40 NTSC, PAL и H32, fallback с ABI v2 без нового поля и стабильность нескольких минут. Отдельно измерить callback latency, LCD cadence, PPA wait, PSRAM/internal heap и качество пиксельной графики. Только после такого A/B можно сравнивать FPS; новый видеотракт не обещает 60 FPS.
