# Архитектурное исследование самостоятельной прошивки для Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3

Дата исследования: 2026-10-09  
Статус: исследование; исходный проект FNK0104S, устройства и прошивки не изменялись.

## Executive Summary

Однозначная рекомендация: начинать новый независимый репозиторий от официального `78/xiaozhi-esp32`, зафиксированного на проверенной ревизии, и использовать его поддержку `waveshare/esp32-p4-wifi6-touch-lcd` как первый аппаратный baseline. Не следует брать заводскую Brookesia-прошивку за основу приложения и не следует проектировать полностью новый voice-assistant core.

Причины:

1. XiaoZhi уже содержит нужные границы `Application` / state machine / `Board` / `AudioService` / `Protocol`, WebSocket и MQTT/UDP, AFE/AEC/VAD/wake word, MCP, OTA и вариант именно `esp32-p4-wifi6-touch-lcd-4.3`.
2. Вариант XiaoZhi уже включает ESP32-P4, ESP-Hosted/ESP32-C6, ES8311+ES7210, MIPI-DSI/ST7701, GT911, OV5647 и device-side AEC. Это резко уменьшает риск первого bring-up.
3. Waveshare Brookesia полезна как аппаратная и мультимедийная референсная реализация: BSP, SDMMC, camera preview, MP4, LVGL 9, tear avoidance, PSRAM-backed GUI task, приложение-оболочка. Но factory snapshot привязан к IDF 5.5.5, имеет второй application framework и собственную модель приложений/аудио. Перенос FNK-функций туда потребует переписать большую часть интеграции XiaoZhi.
4. Стратегия «чистый проект из выборочных компонентов» преждевременно создаёт ответственность за протокол, state machine, OTA, provisioning, AFE, transport parity и lifecycle. Она имеет смысл лишь как будущая крупная переработка после получения стабильного продукта, а не как начальная база.
5. UI следует строить на LVGL 9 с собственной навигацией/моделью экранов. Текущий `Display` XiaoZhi надо сохранить как совместимый фасад для assistant-событий, но не как архитектуру всего 480×800 UI. Brookesia целиком подключать не рекомендуется; из неё выгодно перенять patterns и, при необходимости, отдельные компоненты с проверенной лицензией/API.
6. Перенос FNK должен быть семантическим: выделить сервисы радио, локальной медиатеки, погоды, генерации музыки, персонажа и настроек из board-specific запуска. Пины, драйверы FT6336/SPI display и предположения ESP32-S3 переносить нельзя.

Целевой SDK: ESP-IDF 6.0.2. Для первого аппаратного checkpoint допустима отдельная диагностическая ветка на 5.5.5 только если конкретная ревизия платы или компонент блокирует 6.0.2. В новый продукт нельзя незаметно смешивать бинарные профили P4 rev1.3 и rev3.x.

## Объём и зафиксированные источники

### Локальный FNK0104S

- Репозиторий: `ZerGint/aiXiaozhiFNK0104`, ветка `FINAL`.
- Исследованный `HEAD`: `d74b53b1c18e3dca5563d9abc05872864fd55a3b` (`release: prepare firmware 1.1.11`, 2026-10-07).
- В дереве на момент исследования существовали пользовательские изменения в `main/music/generated_music_job_storage.*`, `main/music/music_service.cc`, а также неотслеживаемые логи/аудит. Они сохранены, не редактировались и не считаются частью указанного `HEAD`; там, где текущий файл упомянут, это отмечено как состояние worktree.
- Просмотрены исходники, конфигурации, история Git, архивные отчёты и логи. Generated/vendor/build каталоги не использовались как архитектурная истина.

### Официальный XiaoZhi

- URL: <https://github.com/78/xiaozhi-esp32>
- Исследованная ревизия: `4f7c00fb8432e92b08732f5a3cd6c58f9f7ca0cd` от 2026-10-08.
- Ключевая плата: `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/`, вариант `esp32-p4-wifi6-touch-lcd-4.3`.

### Waveshare product repository

- URL: <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3>
- Исследованная ревизия: `dc1b2085381c9cc3a2badf8d317673cca9079c1f` от 2026-08-21.
- Заводская Brookesia: `firmware/brookesia/`.
- Примеры: `examples/esp-idf/01...12`, в особенности `04_wifistation`, `05_sdmmc`, `06_I2SCodec`, `08_lvgl_demo_v9`, `09_video_lcd_display`, `10_mp4_player`, `11_esp_brookesia_phone`.
- Официальное описание платы и ограничения ревизий: [README репозитория](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3), [документация платы](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3), [схема](https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4.3/ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf).

### Waveshare components

- URL: <https://github.com/waveshareteam/Waveshare-ESP32-components>
- Использовать опубликованный BSP `waveshare/esp32_p4_wifi6_touch_lcd_4_3==1.0.1`, а не копировать драйверы из примеров. Product examples 06–12 уже выбирают эту версию.

## Подтверждённые аппаратные факты и ограничения

| Узел | Подтверждено | Архитектурное следствие |
|---|---|---|
| SoC | ESP32-P4NRW32, dual-core high-performance RISC-V + low-power core | P4 не имеет встроенного radio; нельзя использовать ESP32-S3 Wi-Fi/BLE код напрямую |
| Память | 32 MB in-package PSRAM, 32 MB external NOR flash | Frame buffers, cover art, media buffers, каталоги и некритичные stacks размещать в PSRAM; DMA/internal оставлять для драйверов и hot paths |
| Wireless | ESP32-C6-MINI-1 по SDIO, Wi-Fi 6 + BLE 5 LE | Сеть должна идти через ESP-Hosted/`esp_wifi_remote`; C6 firmware/version — часть совместимости продукта |
| Display | 4.3", 480×800, 2-lane MIPI-DSI, ST7701 | Нужны DSI frame buffers, tear-avoidance и контролируемый формат RGB565/RGB888 |
| Touch | GT911, до пяти касаний | Использовать BSP touch; не переносить FNK FT6336 driver/координаты |
| Audio | ES8311 playback codec + ES7210 ADC, два onboard microphone | Хорошая база для AFE/device AEC, но акустическая задержка/reference routing требуют реального теста |
| Storage | microSD, 4-line SDIO 3.0 | Не переносить 1-bit FNK pin config; определить ownership шины и hot-remove policy |
| Camera | 15-pin, 2-lane MIPI-CSI; optional OV5647 | Camera должна быть optional capability/service; preview и encode требуют memory budgets |
| USB | USB 2.0 High-Speed OTG | Возможны UVC/UAC/extended display, но не включать в MVP |

ESP32-P4 поддерживает MIPI-CSI/DSI, аппаратные JPEG и H.264 блоки, PPA и 2D-DMA; это делает preview, scale/color conversion и видео практически достижимыми, но не означает, что произвольный codec/container будет аппаратно ускорен. См. [Espressif ADF project guidance](https://docs.espressif.com/projects/esp-adf/en/latest/knowledge-center/project-design.html) и [ESP32-P4 getting started](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32p4/get-started/index.html).

Критичное ограничение silicon revision:

- Waveshare default `rev3_x`: `CONFIG_ESP32P4_REV_MIN_300=y`, PSRAM 250 MHz.
- Совместимый профиль `rev1_3`: `CONFIG_ESP32P4_REV_MIN_100=y`, `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, PSRAM 200 MHz.
- Эти бинарники нельзя считать взаимозаменяемыми. Перед первой сборкой необходимо считать маркировку/ревизию SoC и зафиксировать её в паспорте устройства.
- Upstream XiaoZhi имеет отдельные `esp32-p4-...` и `esp32-p4x-...` варианты; правильный вариант выбирается по фактическому silicon, а не по внешнему виду PCB.

## Сравнение трёх кодовых баз

| Область | FNK0104S | XiaoZhi upstream | Waveshare factory/Brookesia |
|---|---|---|---|
| SDK | manifest `idf >=5.5.2`; проект мигрирован и документирует IDF 6.0.x | текущий основной каркас ориентирован на IDF 6 с legacy 5.5 paths | factory snapshot: IDF 5.5.5, manifest `>=5.5,<6.0`; product examples CI: 5.5.5 и 6.0.2 |
| Application | XiaoZhi core, сильно расширенный media/weather/UI lifecycle | зрелый assistant lifecycle и state machine | Brookesia `Phone`, registry/apps, launcher lifecycle |
| Hardware abstraction | `Board`, но FNK-specific сервисы запускаются из board class | `Board`/optional capabilities и reusable common-board helpers | опубликованный C BSP + `bsp_extra`; application теснее связан с BSP/Brookesia |
| Tasks | XiaoZhi audio tasks + отдельные SD/radio/weather/music tasks | audio input/output/codec/AFE/wake tasks; main event loop | LVGL task, Brookesia app tasks, GMF/audio/network/media tasks |
| Audio/AFE | XiaoZhi AFE + доработанный routing/media interruption; FNK external I2S option | ESP-SR AFE, AEC/VAD/wake word, ES8311/ES7210 support на P4 board | codec init и GMF/audio framework; factory XiaoZhi app отдельна внутри shell |
| Network | native Wi-Fi на S3 | ESP-Hosted + esp_wifi_remote на P4/C6 | тот же класс transport через board BSP/ESP-Hosted |
| Display/UI | LVGL 9, custom 3-tab UI, AnimeFace, small landscape assumptions | `Display` facade + LCD/LVGL screens, assistant-first UI | полноценная 480×800 phone shell, app registry, status/navigation, LVGL |
| Storage/OTA | SD music, radio catalog, generated music, verified SD-staged OTA | flash assets/NVS/OTA; board-dependent storage | SPIFFS, SDMMC examples, media assets; factory/recovery image flow |
| Camera/media | shared camera abstractions; FNK product не является P4 media baseline | OV5647 enabled for target board; camera MCP | strongest camera preview, MP4, music/video player reference |

### Application и состояния

XiaoZhi/FNK запускается через [`Application::Initialize`](../main/application.cc), меняет состояния только через `Application::SetDeviceState()` и [`DeviceStateMachine`](../main/device_state_machine.cc), а callbacks возвращает в главный контекст через `Application::Schedule()`. Это правильная основа голосового устройства: transport не владеет UI, board не владеет conversation state, audio имеет отдельный сервис.

Factory Brookesia `app_main()` создаёт display, монтирует SPIFFS, запускает codec, создаёт `Phone`, активирует stylesheet 480×800 и устанавливает calculator/draw/spec analyzer/music/camera/video/settings/XiaoZhi apps. Это desktop/phone-like lifecycle, а не расширение XiaoZhi state machine. Одновременное сохранение обоих верхнеуровневых managers породит два владельца audio focus, screen navigation, app state и памяти.

### FreeRTOS и concurrency

FNK/XiaoZhi подтверждённо использует отдельные задачи audio input/output, Opus codec, AFE processing, wake encoding, radio stream, SD decode, weather HTTP и music job polling. Примеры: [`audio_service.cc`](../main/audio/audio_service.cc), [`afe_audio_engine.cc`](../main/audio/engines/afe_audio_engine.cc), [`internet_radio_player.cc`](../main/media/internet_radio_player.cc), [`sd_music_player.cc`](../main/media/sd_music_player.cc), [`weather_service.cc`](../main/weather/weather_service.cc), [`music_service.cc`](../main/music/music_service.cc).

Положительные FNK patterns, которые следует сохранить:

- bounded buffers/results и потоковый JSON framing для Radio Browser;
- PSRAM-backed radio/Opus/task buffers там, где latency допускает;
- сериализация UI/application mutations через `Schedule()`;
- audio focus: voice может приостановить media и затем безопасно возобновить;
- lazy UI creation и ограничение одновременно живущих экранов;
- явные memory diagnostics для internal/DMA/PSRAM largest block.

Новая архитектура должна добавить единый `TaskPolicy`/таблицу бюджетов: имя, priority, affinity, stack location, максимальная очередь, watchdog expectation. Запрещены новые бесконечные detached задачи без владельца и stop/join semantics.

### Audio, AFE, AEC, VAD и wake word

Upstream board config включает `CONFIG_USE_DEVICE_AEC=y` и OV5647. XiaoZhi `Kconfig.projbuild` допускает AFE/wake/AEC на ESP32-P4 с PSRAM, а `AudioService` разделяет capture, playback, encoding и engine. Это готовее к переносу FNK audio focus, чем Brookesia/GMF rewrite.

Но наличие ES7210, двух микрофонов и `CONFIG_USE_DEVICE_AEC` не является доказательством качества hands-free AEC. Нужно проверить:

- фактический channel order ES7210;
- clean speaker reference до/после software volume/resample;
- acoustic/reference delay;
- sample rates и clock drift;
- VAD false positives при музыке;
- AEC при 8 Ω/2 W speaker и разных уровнях громкости;
- wake word во время playback и после interruption/reconnect.

### Сеть ESP32-P4 + ESP32-C6

P4 не содержит Wi-Fi/BLE. В XiaoZhi зависимости для P4 включают `espressif/esp_hosted` и `espressif/esp_wifi_remote`; board source использует network abstraction. В Waveshare примере `04_wifistation` и factory firmware применяют hosted C6. Для нового проекта это должно остаться ниже `Network` interface; Radio/Weather/Music/XiaoZhi protocol не должны знать о C6 или SDIO.

Необходимо pin/version lock для P4 host component и C6 slave image, плюс recovery/update процедура C6. Теоретическая доступность BLE через C6 не равна готовой продуктовой BLE-функции.

### Display, LVGL, MIPI-DSI и GT911

Подтверждено:

- XiaoZhi board source и `lcd_init_cmds.h` дают рабочую интеграционную точку ST7701/DSI.
- Waveshare BSP 1.0.1 содержит обновления Rev3.x и GT911 probing без обязательных INT/RST.
- Factory snapshot использует 500 Mbps/lane, DPI 30 MHz и свой GPIO23 reset/polling вариант; эти значения нельзя смешивать с новым BSP без осциллограммы/теста конкретной ревизии.
- Brookesia demo запускает LVGL task на core 1, stack 16 KiB в PSRAM, triple-partial tear avoidance и stylesheet 480×800.

Рекомендация: один LVGL instance/port, один display lock, BSP-owned panel/touch и собственный UI coordinator. `Display` XiaoZhi остаётся адаптером (`SetStatus`, emotion, chat, notifications), а navigation, radio/library/weather/settings принадлежат новому UI слою.

### SRAM, PSRAM и DMA

Назначение памяти:

- internal/DMA: I2S descriptors/buffers, DSI/PPA descriptors по требованиям драйвера, ISR data, task control blocks, небольшие realtime queues;
- PSRAM: decoded covers, animation assets/cache, radio HTTP input, SD decoder buffers, catalogs/playlists, GUI task stack (если поддержано), camera frame buffers;
- flash mmap/assets: immutable UI assets/fonts, если формат/partition позволяет;
- SD: user media, generated music, optional cover cache and logs with bounded retention.

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` в upstream board build показывает намерение не съедать internal heap обычными allocations. Однако placement не должен полагаться только на global allocator threshold: крупные владельцы обязаны явно выбирать caps и иметь fallback/error behavior.

### Filesystem, flash и OTA

FNK имеет SD music и отдельный verified SD-staging OTA/minimal updater flow ([`main/ota`](../main/ota), [`storage_manager.cc`](../main/managers/storage_manager.cc)). Это ценно, но partition layout FNK 16 MB и updater assumptions не переносятся на 32 MB автоматически.

Для нового проекта нужен новый 32 MB partition contract с минимумом: NVS, PHY/coredump при необходимости, dual OTA app slots, immutable/recoverable assets, optional writable cache. Пользовательскую медиатеку хранить на SD, не во flash. OTA обязана проверять product/board identity, silicon profile, image size/hash/signature policy и rollback. C6 firmware update проектировать отдельно.

## Инвентаризация доработок FNK

Оценки ниже относятся к функциональной логике. «Переносим» не означает копирование файлов; это означает перенос контракта, тестов и алгоритмов в новый owning layer.

| Функция | Реализация и зависимости | Переносимость | Решение |
|---|---|---|---|
| Internet radio / Radio Browser | [`radio_browser.cc`](../main/media/radio_browser.cc), `radio_storage.*`, `radio_json_framer.h`, `internet_radio_player.*`; HTTP/cJSON, storage, audio output, MCP | Высокая для domain/storage; средняя для player | Сохранить bounded incremental parsing, UUID favorites/catalog и MCP semantics. Переподключить Network/Storage/AudioFocus; заменить plain HTTP endpoints на HTTPS/DNS discovery policy |
| Асинхронный radio playback | `InternetRadioPlayer` task, ESP HTTP client, stream sniff/decode, PSRAM stack/buffers | Средняя | Перепроектировать как cancellable session с bounded queue; сохранить retry/prebuffer/terminal failure semantics |
| SD music player | [`sd_music_player.cc`](../main/media/sd_music_player.cc), `MediaPlayer`, decoder/resampler, `StorageManager` | Средняя | Scanner/library interface сохранить; mount, 4-bit SDMMC, hot-remove, decoder capabilities и path policy адаптировать |
| Metadata / MusicBrainz | [`musicbrainz_metadata.cc`](../main/media/musicbrainz_metadata.cc) | Высокая логически | Сделать optional background enrichment с rate limit/cache; не блокировать local playback |
| ID3/APIC/cover art | SD player/media/UI path и история cover improvements | Средняя | Новый streaming parser с жёсткими limits; cover decode/cache service; hardware JPEG/`esp_new_jpeg` benchmark вместо полного переноса |
| Media audio focus | [`media_player.cc`](../main/media/media_player.cc), `AudioService` source flags | Высокая по semantics | Выделить `AudioFocusManager`: assistant/system/media/radio, pause/resume/duck; не оставлять cross-service singleton calls |
| Generated music / YuE bridge | [`music_service.cc`](../main/music/music_service.cc), `music_bridge_client.*`, generated storage/job storage; mDNS/HTTP/SD/MCP | Высокая для protocol, средняя интеграционно | Сохранить job recovery, Unicode/path validation, resumable lifecycle. Перенести в service с injected Network/Storage/Playback; не привязывать к board |
| Weather lifecycle | [`weather_service.cc`](../main/weather/weather_service.cc); Open-Meteo, NVS, HTTP, MCP, UI | Высокая | Сохранить deferred refresh, bounded response/cache, idle/audio-ready gates. UI получает immutable model/event |
| AnimeFace | [`anime_face.cc`](../main/display/anime_face.cc), layered LVGL images, emotions, eye/mouth/blink/gaze/reactions | Средняя | Сохранить state/expression engine и asset concepts; переразметить под portrait 480×800, новый asset budget и LVGL ownership |
| Assistant UI states | `Application::OnStateChanged`, `LcdDisplay`, AnimeFace | Высокая | Сохранить mapping state→presentation через model, не прямые service→widget calls |
| Touch/navigation/custom UI | FNK board touch driver + `lcd_display.cc` tabs/pages/quick settings | Низкая для driver/layout, средняя для UX | FT6336/pins не переносить. Перенести user flows, затем реализовать заново на GT911/portrait UI |
| Home Assistant | [`home_assistant.cc`](../main/home_assistant.cc), FNK settings HTTP server, quick settings, MCP-related integrations | Средняя | Core client/config можно адаптировать; web server и board startup вынести в optional integration service |
| MCP extensions | [`mcp_server.cc`](../main/mcp_server.cc), Radio/Weather/Music registrations | Высокая | Сохранить tool contracts после ревизии validation/ownership; registration делать feature modules, не board constructor |
| Memory/fragmentation fixes | PSRAM stacks/buffers, lazy pages, bounded catalogs, emoji skip; `docs/archive/*` | Высокая как engineering policy | Перенести тесты, checkpoints и budgets, но заново измерить P4 — абсолютные числа S3 неприменимы |
| OTA SD staging/minimal updater | [`main/ota`](../main/ota), release/version scripts | Средняя/низкая | Перенести threat model и state transitions; partition, display, board identity и P4 revision checks реализовать заново |
| External I2S/audio routing | FNK board `external_i2s_audio_codec.*`, `audio_service` source routing | Низкая hardware, высокая semantic | FNK pins/codec subclass не переносить. Сохранить source tagging, interruption и volume contract поверх ES8311/ES7210 BSP |
| Dynamic glyph/font/UI optimizations | `dynamic_glyph_cache.*`, font subsets, lazy pages | Средняя | Re-benchmark с 480×800 and 32 MB PSRAM; сохранять cache limits и Unicode correctness |
| BLE initialization FNK | board-specific NimBLE startup | Низкая | Не переносить: на P4 BLE находится на C6 и требует отдельного supported remote-BT design |

Дополнительно обнаружены: verified GitHub OTA staging, Home Assistant settings portal, IP/status bar, auto-dimming, quick settings, reactions (wink/love/angry/smile), voice/media resume logic, binary radio catalogs, deletion/hold protection, persistent generation jobs, Unicode-safe provider and bridge discovery. Их следует внести в product backlog, но не включать все в первый bring-up.

## Оценка трёх стратегий

Шкала: 1 — плохо/дорого, 5 — хорошо/дёшево. Для «объёма работ» 5 означает наименьший объём.

| Критерий | A: XiaoZhi base | B: Waveshare Brookesia base | C: чистый selective project |
|---|---:|---:|---:|
| Объём работ до stable assistant | 5 | 2 | 1 |
| Совместимость с FNK services | 5 | 2 | 3 |
| Предсказуемость audio/voice | 5 | 2 | 2 |
| Multimedia baseline | 3 | 5 | 4 |
| Удобство собственного UI | 4 | 4 | 5 |
| Upstream updates | 4 | 2 | 2 |
| Долгосрочная сопровождаемость | 4 | 2 | 3 |
| Риск двойного framework/ownership | 5 | 1 | 4 |
| Итог | **35/40** | **20/40** | **24/40** |

### A. Новый репозиторий от XiaoZhi — рекомендуется

Работа: низкая для voice/hardware baseline, средняя для нового UI/multimedia. Основной риск — upstream churn и то, что текущая board support всё равно требует реального hardware validation. Mitigation: vendor/upstream branch, узкие adapters, регулярный merge cadence, integration tests.

### B. Новый репозиторий от factory Brookesia — не рекомендуется

Сильные стороны: красивый app shell, 480×800 stylesheet, media/camera apps, board-tuned demo. Недостатки: factory snapshot IDF 5.5.5, второй верхний framework, тяжёлые компоненты, сложный audio focus, XiaoZhi как вложенное приложение. Почти все FNK services придётся оборачивать под Brookesia apps и GMF lifecycle. Upstream factory source — dated product snapshot, не хороший core для длительной эволюции voice product.

### C. Полностью самостоятельный selective project — не сейчас

Он даёт лучшую теоретическую чистоту, но команда сразу становится владельцем protocol compatibility, OTA, provisioning, MCP, AFE, state machine, board abstraction, error recovery и media integration. Это полезная возможная конечная архитектура, но достигать её лучше постепенным выделением модулей из A, сохраняя работающий продукт на каждом этапе.

## Рекомендуемая архитектура нового проекта

```text
app/
  application_controller       # единственный владелец product lifecycle
  state_machine                # assistant + product-level states
platform/
  board                        # Waveshare P4 implementation, capabilities
  network                      # ESP-Hosted/C6 behind interface
  storage                      # flash/NVS/SD, mount and hot-remove events
  audio_hal                    # ES8311/ES7210 BSP adapter
services/
  xiaozhi_session              # Protocol/WebSocket/MQTT adapters
  audio_focus                  # assistant/system/media/radio arbitration
  radio                        # Radio Browser/catalog/playback session
  media_library                # SD scan, metadata, playlists, cover cache
  generated_music             # bridge/jobs/download/recovery
  weather                     # fetch/cache/policy
  ota                         # P4 image policy; separate C6 concern
  integrations                # Home Assistant and future optional services
ui/
  ui_controller               # only public UI mutation entry point
  models                      # immutable snapshots/events
  screens                     # home/assistant/radio/library/weather/settings
  character                   # AnimeFace state engine + renderer
  assets                      # versioned/mmap/cache policy
media/
  demux_decode_resample       # bounded pipelines, hardware acceleration adapters
```

Правила границ:

- board создаёт только hardware capabilities; не регистрирует Radio/Weather/Music business tools;
- services не обращаются к LVGL напрямую;
- UI callbacks отправляют typed commands в application/services;
- сетевые callbacks не меняют application state напрямую;
- один `AudioFocusManager` решает coexistence assistant/radio/music/system sounds;
- camera/display/SD/BLE являются optional capabilities;
- каждое background действие имеет owner, cancel, timeout, bounded memory and teardown;
- protocol semantics остаются transport-neutral и проверяются для WebSocket и MQTT/UDP.

## Новый интерфейс 480×800

### Выбор технологии

Рекомендация: LVGL 9 + `esp_lvgl_port`/board BSP + собственная UI architecture. Не использовать «существующий XiaoZhi display» как весь UI и не внедрять целиком Brookesia.

- От XiaoZhi сохранить `Display` compatibility adapter и assistant semantics.
- От FNK сохранить AnimeFace behavior/state mapping, но перерисовать layout/assets для portrait.
- От Brookesia перенять status/navigation patterns, display locking, tear avoidance, stylesheet discipline, lazy app/screen lifecycle.
- Brookesia components подключать только если конкретная функция (например, animation player) объективно выигрывает benchmark и не вводит второй lifecycle owner.

### Предлагаемая навигация

1. Home: персонаж, состояние ассистента, компактные weather/media cards.
2. Assistant: transcript/status, mute/listen controls, connection/error state.
3. Radio: favorites/search/now playing; cover/logo cache optional.
4. Library: folders/artists/albums/tracks, generated music section.
5. Weather: current + forecast from cached model.
6. Settings: network, audio, display, integrations, updates, diagnostics.
7. Future gated screens: camera preview, gallery/video.

Память UI: держать живыми shell/home/current screen; остальные создавать лениво и освобождать после перехода. Covers должны иметь фиксированный pixel/byte limit, LRU и cancellation при уходе со страницы. Character assets — mmap/immutable where possible, без runtime duplication.

## План миграции и критерии готовности

### 1. Выбор baseline и фиксация ревизий

Готово, когда: выбран commit XiaoZhi; записаны P4 silicon revision, PCB revision, BSP 1.0.1, C6 image/component versions, IDF 6.0.2; оформлена upstream/update policy.  
Риски: неверно выбран P4/P4X profile, несовместимый C6 image, floating dependencies.

### 2. Создание независимого репозитория

Готово, когда: новый history/identity, лицензии и notices сохранены; build воспроизводим; board identity/OTA namespace уникальны; никакой код FNK механически не скопирован.  
Риски: потеря attribution, OTA collision, случайное наследование secrets/generated files.

### 3. Hardware bring-up

Готово, когда на реальной плате отдельно проверены boot/reboot, 32 MB PSRAM/flash, DSI/backlight, GT911 multi-touch, ES8311 playback, два ES7210 mic channels, SDMMC read/write/removal, C6 Wi-Fi reconnect, buttons/power; camera probe отдельно.  
Риски: revision-specific clocks/pins, panel tearing, touch transform, DMA starvation.

### 4. Stable XiaoZhi baseline

Готово, когда пройдены provisioning, WebSocket и MQTT/UDP, 30+ reconnect cycles, OTA rollback dry run, wake/listen/speak/interrupt, 8-hour idle and conversation soak.  
Риски: AEC instability, hosted network reset, transport-specific regressions.

### 5. Перенос основных FNK services

Порядок: audio focus → storage/media library → SD playback → radio → weather → generated music → integrations/MCP.  
Готово, когда каждый сервис имеет interface, host/unit tests где возможно, bounded memory, cancellation and no direct LVGL/board dependency.  
Риски: singleton coupling, NVS key conflicts, path/Unicode bugs, fragmented DMA heap.

### 6. Новый UI

Готово, когда реализованы shell/home/assistant/radio/library/weather/settings; 60/30 FPS target формально выбран; touch/navigation soak; no screen leaks over 500 transitions; assistant remains responsive during covers/scan.  
Риски: oversized assets, LVGL cross-thread calls, full-screen redraw bandwidth, PSRAM latency.

### 7. Multimedia

Готово, когда определены supported containers/codecs/resolutions; camera preview стабильна; JPEG/PPA benchmark; MP4/H.264 test set; audio focus and thermal/memory budgets satisfied.  
Риски: container/codec mismatch, SD throughput, frame buffer pressure, decoder licensing/maintenance.

### 8. Performance optimization

Готово, когда есть repeatable metrics: boot time, UI frame time, audio underruns, hosted throughput, SD throughput, internal/DMA/PSRAM free+largest block, CPU/task runtime, temperature. Оптимизация принимается только по A/B evidence.  
Риски: перемещение realtime buffers в PSRAM, global allocator tweaks, скрытые regressions.

### 9. Длительная стабильность

Готово, когда: 24–72 h mixed workload; repeated network loss, SD removal, failed HTTP, OTA interruption, camera open/close, 1000 screen switches; no monotonic memory loss, watchdog reset, audio deadlock or corrupted storage.  
Риски: редкие races, C6 desync, filesystem corruption, queue buildup.

## Риски и ограничения

| Риск | Вероятность/влияние | Контроль |
|---|---|---|
| Неверный silicon profile | средняя/критическое | считать revision, отдельные artifacts/names, reject incompatible OTA |
| AEC работает функционально, но плохо акустически | высокая/высокое | calibrated reference/delay test matrix на физическом устройстве |
| Два UI/application framework | высокая/высокое | не встраивать Brookesia shell; один controller/LVGL owner |
| DMA/internal fragmentation | высокая/высокое | budgets, largest-block telemetry, bounded allocation, early init order |
| ESP-Hosted/C6 recovery | средняя/высокое | version pin, reset/reconnect soak, documented C6 recovery/update |
| SD corruption/hot removal | средняя/высокое | mount state machine, write atomicity, removal events, fs tests |
| UI bandwidth/tearing | средняя/среднее | RGB565 first, partial/triple buffering benchmark, PPA where useful |
| Camera/video scope explosion | высокая/среднее | feature gate after stable voice/media; explicit codec matrix |
| Upstream divergence | высокая/среднее | upstream branch, adapters, small patch set, scheduled merge validation |
| Security of MCP/HA/music bridge/OTA | средняя/критическое | validation, authentication/TLS policy, local approval, signed/hash-checked images |

## Вопросы, требующие аппаратной проверки

1. Точная маркировка ESP32-P4: rev1.3, rev3.0 или rev3.1+?
2. PCB revision и соответствие приложенной схеме.
3. Какие GT911 INT/RST линии реально разведены и нужны ли они данной ревизии.
4. Стабильны ли default BSP DSI clocks/tear mode на конкретной панели.
5. Реальный порядок каналов ES7210 и доступность playback reference для AEC.
6. Максимальная громкость без clipping/feedback; задержка acoustic path.
7. SDMMC 4-bit throughput и coexistence с C6 SDIO/DMA under load.
8. Поведение power button, RTC/battery rail и backlight при reboot/sleep.
9. Установлена ли OV5647 и какой sensor/format нужен продукту.
10. Internal/DMA largest block после display + AFE + network + SD + one cover + camera.
11. Hosted Wi-Fi reconnect/reset behavior и версия firmware ESP32-C6.
12. Thermal stability при DSI + AFE + Wi-Fi + video decode/encode.

## Конкретные исходные файлы для следующего этапа

### XiaoZhi base

- `main/application.cc`, `main/application.h`
- `main/device_state_machine.*`
- `main/boards/board.*`, `main/boards/common/*`
- `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/{config.json,config.h,esp32-p4-wifi6-touch-lcd.cc,lcd_init_cmds.h,README.md}`
- `main/audio/{audio_service.*,engines/afe_audio_engine.*,codecs/es8311_audio_codec.*}`
- `main/protocols/{protocol.*,websocket_protocol.*,mqtt_protocol.*}`
- `main/display/*`, `main/mcp_server.*`, `main/ota.*`
- `main/idf_component.yml`, `main/Kconfig.projbuild`, `main/CMakeLists.txt`, `scripts/build.py`

### Waveshare hardware/media references

- `firmware/brookesia/{README.md,sdkconfig.defaults,main/idf_component.yml,main/main.cpp}`
- `examples/esp-idf/04_wifistation`
- `examples/esp-idf/05_sdmmc`
- `examples/esp-idf/06_I2SCodec`
- `examples/esp-idf/08_lvgl_demo_v9`
- `examples/esp-idf/09_video_lcd_display`
- `examples/esp-idf/10_mp4_player`
- `examples/esp-idf/11_esp_brookesia_phone`
- registry component `waveshare/esp32_p4_wifi6_touch_lcd_4_3==1.0.1`

### FNK semantics to extract

- [`main/media`](../main/media)
- [`main/music`](../main/music)
- [`main/weather`](../main/weather)
- [`main/display/anime_face.cc`](../main/display/anime_face.cc)
- [`main/display/lcd_display.cc`](../main/display/lcd_display.cc)
- [`main/audio/audio_service.cc`](../main/audio/audio_service.cc)
- [`main/managers/storage_manager.cc`](../main/managers/storage_manager.cc)
- [`main/ota`](../main/ota)
- [`main/home_assistant.cc`](../main/home_assistant.cc)
- [`main/mcp_server.cc`](../main/mcp_server.cc)
- [`main/boards/freenove-fnk0104s`](../main/boards/freenove-fnk0104s) — только как источник UX/integration semantics, не аппаратного кода.

## Итоговая рекомендация

Начинать с нового независимого репозитория на базе официального XiaoZhi commit, с его board variant `esp32-p4-wifi6-touch-lcd-4.3`, ESP-IDF 6.0.2 и опубликованным Waveshare BSP. Сначала добиться проверенного hardware + stable XiaoZhi baseline на конкретной silicon revision. Затем последовательно переносить FNK services через новые интерфейсы, начиная с audio focus/storage/radio, и параллельно строить собственный portrait LVGL UI. Waveshare Brookesia использовать как проверяемый справочник по BSP, памяти, display/camera/media и UX patterns, но не как верхнеуровневую основу и не как второй runtime framework.

Это минимизирует самый опасный класс риска — одновременную переработку аппаратуры, голосового протокола, AFE, UI и мультимедиа — и при этом не запирает продукт в маленьком UI XiaoZhi: после стабильного baseline его `Display` становится адаптером внутри полноценной собственной LVGL-архитектуры.
