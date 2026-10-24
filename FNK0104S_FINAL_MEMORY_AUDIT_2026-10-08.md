# FNK0104S — аудит внутренней памяти и стабильности

Дата: 2026-10-08  
Ветка: FINAL  
Цель: read-only анализ production-прошивки FNK0104S на ESP32-S3 с ESP-IDF 6.1.0.

## Ограничения и исходные данные

Аудит выполнен без изменения исходников, конфигурации, NVS, прошивки и истории Git. Файл, указанный в исходном задании как «Вставленный текст(20261008-161119).txt», в рабочем окружении отсутствует. Поэтому численные выводы сделаны по доступному serial_log_20261008_170830.txt. Он содержит тот же тип длительного теста: загрузку, Wi-Fi, MQTT, Weather, mDNS, MusicBridge, несколько загрузок музыки, SD-плеер и UI. На момент чтения лог продолжал расти, поэтому цифры являются read-only снимком, а не результатом остановки устройства.

В снимке разобрано 13 296 строк и 817 строк с периодическими показателями RAM. Для владельцев памяти, находящихся внутри ESP-IDF, Wi-Fi, MQTT, TLS, LVGL и аудиокомпонентов, приведены только выводы, подтверждаемые конфигурацией, исходниками и логом. Точные размеры сторонних объектов без runtime heap tracing определить нельзя.

## Executive summary

1. Признаков исчерпания памяти, повреждения heap или аварийного завершения в доступном логе нет: отсутствуют malloc failed, out of memory, heap assertion, panic и stack overflow.
2. После инициализации сервисов рабочий запас INTERNAL RAM держится примерно в диапазоне 49–53 KiB. В более тяжёлых сочетаниях аудио, Weather, UI и загрузок текущий свободный объём временно опускается до 18,7 KiB, а крупнейший непрерывный блок — до 7,5 KiB.
3. Число 10 544 байта — исторический минимум, который накапливается с начала работы и не является текущим свободным объёмом. К концу снимка текущий свободный INTERNAL RAM снова около 34,9 KiB, крупнейший блок около 19 KiB.
4. PSRAM используется эффективно: в конце снимка остаётся примерно 7,29 MiB, а 16 KiB буфер MusicBridge во всех показанных загрузках размещён в PSRAM и освобождается после скачивания.
5. Одноразовое падение INTERNAL RAM примерно на 5,2 KiB при mdns_init похоже на штатную постоянную стоимость mDNS, а не на повторяющуюся утечку.
6. Weather создаёт временную задачу на 8 KiB. После удаления задачи около 8 KiB возвращается. В трёх наблюдавшихся обновлениях не видно накопления, но глубокие пики становятся опаснее по мере снижения baseline.
7. MusicDownload освобождает HTTP-клиент, файл и PSRAM-буфер. После четырёх успешных загрузок baseline не падает монотонно. У задачи остаётся всего 512–532 байта stack high-water mark; это главный эксплуатационный риск, хотя переполнения в логе не зафиксировано.
8. Потери MQTT-аудиопакетов происходят при десятках килобайт свободной INTERNAL RAM и миллионах байт свободной PSRAM. По этим данным они больше похожи на UDP/Wi-Fi jitter или недостаток очереди, чем на нехватку памяти.
9. Уменьшение largest free block доказано как факт временного давления и фрагментации, но не как утечка. Для доказательства утечки нужны lifecycle-снимки с владельцами allocations или controlled heap tracing.

## Версия, плата и состояние Git

По build/project_description.json:

| Поле | Значение |
|---|---|
| Проект | xiaozhi |
| Версия приложения | 1.1.11 |
| Цель | esp32s3 |
| ESP-IDF | 6.1.0 |
| ELF | xiaozhi.elf |
| Ветка | FINAL |
| HEAD | d74b53b1c18e3dca5563d9abc05872864fd55a3b |

На момент аудита ветка была FINAL...origin/FINAL [ahead 10]. В рабочем дереве уже находились изменения, которые не относятся к этому отчёту: правки Wi-Fi и MusicService, а также несколько неотслеживаемых логов. Они сохранены без изменений. Отчёт — единственный добавленный этим аудитом файл.

## Конфигурация памяти и разделов

В build/config/sdkconfig.json и build/config/sdkconfig.h обнаружены:

| Настройка | Значение | Практический смысл |
|---|---:|---|
| CONFIG_IDF_TARGET_ESP32S3 | 1 | целевой чип ESP32-S3 |
| CONFIG_SPIRAM | 1 | PSRAM включена |
| CONFIG_SPIRAM_MODE_OCT | 1 | Octal PSRAM |
| CONFIG_SPIRAM_SPEED_80M | 1 | 80 MHz |
| CONFIG_SPIRAM_USE_MALLOC | 1 | общий allocator может использовать PSRAM |
| CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL | 2048 | небольшие allocations предпочитают INTERNAL |
| CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL | 98304 | резерв 96 KiB для internal-only/DMA задач |
| CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM | 1 | стеки задач могут находиться во внешней памяти |
| CONFIG_MBEDTLS_DYNAMIC_BUFFER | 1 | TLS-буферы выделяются динамически |
| CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC | 1 | TLS может использовать внешний allocator |
| CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN | 16384 | максимум входного TLS-фрагмента |
| CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN | 4096 | максимум выходного TLS-фрагмента |
| CONFIG_LWIP_TCPIP_TASK_STACK_SIZE | 3072 | стек tcpip_task |
| CONFIG_LWIP_TCPIP_RECVMBOX_SIZE | 16 | mailbox входящих сетевых сообщений |

Для платы выбрана CONFIG_BOARD_TYPE_FREENOVE_FNK0104S. Используется partitions/v2/16m_fnk0104s.csv: два OTA-раздела приложений по 0x480000, SPIFFS assets 0x6E0000, NVS 0x4000. Размер flash/OTA не увеличивает heap; PSRAM и heap-capability настройки влияют на runtime-запас.

## Полная временная картина RAM

В строках RAM поле free — текущий свободный объём, поле min — накопленный с момента старта минимум, largest — крупнейший непрерывный свободный блок. Исторический минимум не возвращается вверх после освобождения памяти.

| Этап | Internal free | Historical min | Largest block | PSRAM free | Изменение и интерпретация |
|---|---:|---:|---:|---:|---|
| Wi-Fi найден, до подключения (t=5.845 s) | 94 231 | 39 828 | 51 200 | 7 977 948 | ранний запас до полной инициализации |
| После IP (t=9.905 s) | 92 387 | 39 828 | 51 200 | 7 872 760 | Wi-Fi/netif уже созданы |
| После AFE/активации (t=10.845 s) | 76 883 | 39 828 | 34 816 | 7 861 596 | временные audio/activation allocations |
| Инициализация audio/Weather (t=15.845 s) | 34 887 | 26 940 | 20 480 | 7 435 588 | глубокий ранний пик |
| После первого Weather и mDNS (t=20.845 s) | 49 935 | 21 300 | 25 600 | 7 481 004 | штатный baseline сервисов |
| Минимум текущего free (t=1 220.845 s) | 18 711 | 13 936 | 7 680 | 7 283 956 | второй пользовательский download + overlay |
| Поздний baseline после загрузок и речи | 33–53 KiB | 10 544 | 13–25 KiB | 7.25–7.31 MiB | восстановление после временных операций |
| Конец доступного снимка (t=4 085.845 s) | 34 939 | 10 544 | 19 456 | 7 287 416 | память не обнулилась, но остаётся рабочий запас |

Минимальные наблюдавшиеся значения по всему снимку:

- минимальный текущий Internal free: 18 711 байт;
- минимальное значение исторического min: 10 544 байта;
- минимальный Internal largest block: 7 680 байт;
- минимальный PSRAM free: примерно 7 254 504 байта;
- минимальный PSRAM largest block: примерно 7 077 888 байт.

Текущее состояние поэтому нельзя описывать как «осталось 10,5 KiB»: 10,5 KiB — high-water minimum, а не текущий free heap. Для оценки риска нужно одновременно смотреть текущий free, largest и historical min.

## Основные постоянные allocations

| Владелец | Оценка/размер | Capability и расположение | Lifetime | Вывод |
|---|---:|---|---|---|
| Резерв allocator | 96 KiB | INTERNAL, резерв ESP-IDF | весь runtime | политика allocator, а не утечка приложения |
| LVGL image cache | 2 MiB | PSRAM, включается при PSRAM от 8 MiB | постоянный | полезно для UI, занимает часть PSRAM заранее |
| LVGL display buffer | около 19,2 KiB для 480x320x20 RGB565 | DMA/internal, buff_spiram=0 | постоянный | перенос в PSRAM без проверки DMA опасен |
| taskLVGL stack | 9 216 байт | исходник предписывает INTERNAL | постоянный | оставлять internal до отдельного A/B теста |
| audio_input stack | 6 144 байта | default heap, обычно internal из-за аудио | постоянный | realtime path |
| audio_output stack | 4 096 байт | default heap | постоянный | realtime path |
| opus_codec stack | 24 576 байт | явно PSRAM-capable | постоянный | уже разгружает INTERNAL |
| audio_afe stack | 4 096 байт | default heap | постоянный | location не логируется |
| wake-word encode stack | 24 576 байт | PSRAM; StaticTask_t остаётся internal | transient | создаётся для encode и удаляется |
| mDNS | около 5 164 байт по разнице before/after mdns | INTERNAL в текущем runtime | постоянный после первой инициализации | ожидаемая одноразовая стоимость |
| MusicBridge download buffer | 16 384 байта | PSRAM | один download | освобождается после закрытия файла |
| SD decoder input/output | 2 048 + 16 384 байта, с PSRAM fallback | предпочтительно PSRAM | на время playback | освобождаются при закрытии decoder |
| Music metadata/jobs | зависит от количества строк и cJSON/std::string | общий heap, обычно PSRAM-capable | часть persistent, часть transient | в текущем тесте нет признака линейного роста |

Wi-Fi, lwIP, MQTT, TLS, SD/FAT и LVGL создают дополнительные объекты внутри компонентов. Их точное распределение нельзя восстановить по одному агрегированному счётчику; нужны heap-capability snapshots и task/heap tracing.

## FreeRTOS tasks и стеки

Размеры ниже взяты из исходников и конфигурации. Для компонентов ESP-IDF, где проект не вызывает создание задачи напрямую, расположение указано как неизвестное до runtime registry.

| Task/объект | Core | Priority | Stack | Location | High-water mark в логе | Lifetime |
|---|---:|---:|---:|---|---:|---|
| taskLVGL | 1 | 1 | 9 216 B | INTERNAL по комментарию платы | нет | постоянная |
| audio_input | 0 | 8 | 6 144 B | default/internal-путь | нет | постоянная |
| audio_output | n/a | 4 | 4 096 B | default heap | нет | постоянная |
| opus_codec | n/a | 2 | 24 576 B | PSRAM-capable | нет | постоянная |
| audio_afe | n/a | 3 | 4 096 B | default heap | нет | постоянная |
| wake-word encode | n/a | n/a | 24 576 B | PSRAM stack + internal TCB | нет | transient |
| SdMusicPlayer | 1 | 5 | 4 096 B | default heap | 2 104–2 652 B остатка | пока жив плеер |
| InternetRadio | 1 | 3 | 6 144 B | PSRAM-capable create | нет в этом снимке | stream lifetime |
| weather_http | n/a | 1 | 8 192 B | default heap | 2 052–2 152 B остатка | transient |
| music_poll | n/a | 2 | 4 096 B | default heap | 512–532 B остатка | только pending jobs |
| udp_receive (managed component) | n/a | 1 | 3 072 B | component allocator | нет | транспорт |
| MQTT/Wi-Fi/lwIP/mDNS | component-defined | component-defined | component-defined | component-defined | нет | runtime/component |

Для MusicService high-water mark в исходнике переводится в байты через sizeof(StackType_t). Значения около 512 B означают, что задача не переполнилась в тесте, но запас невелик для редких ветвей ошибок, длинных имён файлов, cJSON и глубокой цепочки callback. Это риск, требующий отдельного измерения максимальной глубины, а не доказанное переполнение.

Дублирующихся music_poll задач в логе не видно: после завершения pending work появляется POLL_TASK_STOP reason=no_pending_work, а следующий job создаёт новый worker. Weather также удаляет transient task после обновления.

## Weather и HTTPS

Наблюдались три цикла:

| Фаза | Первый refresh | Последующий refresh | Поздний refresh |
|---|---:|---:|---:|
| before task create | 50 947 B | 40 771 B | 34 779 B |
| after task create | 42 287 B | 32 459 B | 26 403 B |
| before HTTP | 42 087 B | 32 143 B | 26 003 B |
| after HTTP body | 28 323 B, largest 11 264 | 19 651 B, largest 7 680 | 15 647 B, largest 7 680 |
| before JSON parse | 35 995 B | 27 503 B | 21 547 B |
| after JSON parse/extract | 32 343 B | 24 147 B | 18 191 B |
| after body release | 36 899 B | 28 415 B | 22 515 B |
| after update | 36 895 B | 28 095 B | 22 659 B |
| before task delete | 41 575 B | 32 019 B | 26 099 B |
| stack | configured 8 192 B, max used 6 048 B | configured 8 192 B, max used 6 040 B | configured 8 192 B, max used 6 140 B |

Task creation снимает примерно 8,3 KiB, что соответствует стеку и TCB. После after_run_before_delete значительная часть возвращается. Внутри одного цикла HTTP body и JSON дают временный провал largest до 7,5 KiB. Baseline между циклами снижается, но это нельзя приписать Weather без разделения на lazy initialization других сервисов, TLS allocator и состояние аудио/UI.

В source cleanup присутствуют закрытие HTTP-клиента, освобождение body и удаление Weather task. В текущем логе нет постепенного падения на одинаковую величину после каждого refresh. Доказательств Weather leak нет; есть реальный запас по INTERNAL RAM, который становится меньше при поздних refresh и требует контроля совместного сценария с Music download.

## MusicService и MusicBridge

Зафиксированы четыре успешные загрузки. Во всех случаях MusicBridge выделил фиксированный буфер 16 384 байта в PSRAM:

| Job | Размер файла | Before buffer | После buffer alloc | Минимум во время | После buffer free/close | Результат |
|---|---:|---:|---:|---:|---:|---|
| user_9026d3314371af8d | 7 329 315 B | 47 951 B | 35 579 B | 33 699 B | 45 651 / 47 951 B | success |
| user_56b05cffe14a0796 | 7 859 356 B | 33 447 B | 24 095 B | 18 711 B | 30 867 / 32 963 B | success |
| user_35e5bdb8c946693a | 4 594 213 B | 35 635 B | 23 059 B | 23 387 B | 33 303 / 35 667 B | success |
| generated f0… | 6 410 646 B | 39 775 B | 34 291 B | 30 567 B | 37 895 / 40 583 B | success |

Каждая транзакция использует .part файл на SD, проверяет content length и итоговый размер, закрывает HTTP/file, освобождает buffer, переименовывает .part в final и обновляет job metadata. После завершения появляются POLL_TASK_STOP и MUSIC_GATE RELEASE. Уменьшения baseline после каждой загрузки на фиксированную величину нет.

MUSIC_DOWNLOAD_QUIESCE_BEGIN/READY и gate сериализуют операции с плеером и overlay. Это снижает риск гонки, но не гарантирует, что TLS/Weather allocations никогда не пересекаются: такой overlap нужно измерить отдельным сценарием.

При MUSIC_MDNS_NOT_FOUND повторная попытка не оставляет download buffer: в этих ветках загрузка не стартует. .part файлы находятся на SD и не расходуют heap; их незавершённое состояние влияет на persistence, но не на INTERNAL RAM.

## mDNS и discovery

EnsureMdns защищён флагом mdns_initialized_ и вызывает mdns_init один раз. В логе:

- до mDNS: 52 855 B Internal;
- после mDNS: 47 691 B Internal;
- PSRAM меняется только примерно на 176 B.

Одноразовая стоимость mDNS около 5,2 KiB остаётся до перезагрузки. Повторяющиеся MUSIC_MDNS_NOT_FOUND происходят примерно каждые 60 секунд, но это не повторная инициализация mDNS. После обнаружения endpoint 192.168.31.153:8765 query result освобождается, затем выполняется HTTP reachability check.

Признаков накопления mDNS query result в текущем логе нет. Для окончательной проверки нужно дополнительно снимать largest/free до и после 20 циклов not-found/discovered.

## MQTT, TLS и аудио

В конфигурации включены динамические TLS buffers, входной лимит 16 KiB и выходной 4 KiB. MQTT, Wi-Fi, lwIP и UDP transport имеют собственные control blocks, очереди и стеки; они не принадлежат MusicService и не видны по отдельным строкам RAM.

В доступном логе найдено 15 AUDIO_SEQ_GAP. Наиболее заметные серии:

- около 938–943 s: gaps 3–6 пакетов;
- около 1306 s: несколько gaps 1–4 пакета;
- около 2736 s: gaps 1–4 пакета.

В момент первой серии Internal free примерно 38–45 KiB, PSRAM более 7,3 MiB. Очереди dec_q и play_q не выглядят переполненными: встречаются dec_q=0..20, play_q=0..2. Поэтому имеющихся данных недостаточно, чтобы связать gaps с heap pressure. Вероятнее Wi-Fi/UDP jitter, radio power-save transition, MQTT scheduling или краткое опустошение очереди. Нужны RSSI, counters транспорта, timestamps receive/play и queue depth в одной диагностической записи.

Аудиобуферы и DMA нельзя переносить в PSRAM только ради увеличения free: audio input/output, I2S DMA и realtime decode имеют требования к адресу, cache и задержке. В текущем коде PSRAM уже применяется для Opus stack, wake-word stack, radio PCM и MusicBridge buffer; DMA display/audio buffers остаются internal.

## LVGL и AnimeFace

Для плат с достаточной PSRAM lcd_display.cc резервирует 2 MiB под LVGL image cache. Для FNK0104S display buffer настроен как один DMA buffer, RGB565, ширина 480, 20 строк, double buffering выключен, buff_spiram=0. Оценка полезной пиксельной памяти — около 480 × 20 × 2 = 19 200 байт плюс DMA/дескрипторы.

Исходник защищает повторный SetupUI guard. В логе UI_MEM_AFTER_TAB_SWITCH встречаются:

- 33 519 B / largest 13 312 B после одного переключения;
- 37 607 B / largest 17 408 B;
- 40 163 B / largest 21 504 B;
- 41 195 B / largest 21 504 B;
- затем восстановление до 48 399 B / largest 25 600 B.

Такая картина совместима с временными audio/TLS allocations и очисткой очередей. Она не доказывает утечку UI-объектов. Проверка 20–50 циклов tab open/close с одинаковым idle baseline ещё нужна. AnimeFace использует постоянные LVGL descriptors/assets, но в этом тесте нет отдельного ANIME_FACE_MEM счётчика, поэтому его вклад нельзя разделить из агрегированных значений.

## Фрагментация и признаки утечки

Наблюдается реальное уменьшение крупнейшего свободного Internal блока:

- около 25 600 B на штатном baseline;
- 11 264–7 680 B во время Weather/download peaks;
- восстановление до 25 600 B после части операций;
- около 19 456 B в конце текущего снимка.

Это означает, что крупный непрерывный блок иногда дробится временными allocations. Но уменьшение largest само по себе не доказывает leak: оно может быть вызвано нормальным соседством живых маленьких объектов, TLS, LVGL и очередей. Для leak должны одновременно выполняться признаки: baseline после идентичного сценария монотонно падает, число живых объектов растёт, а освобождение операции не возвращает память.

В текущем логе:

- нет OOM/malloc failure;
- download buffers возвращаются в PSRAM;
- Weather task stack возвращается после удаления;
- Music poll task останавливается после отсутствия pending work;
- нет линейного падения после четырёх загрузок;
- нет повторной mDNS init.

Итог: подтверждены transient pressure и потенциальная фрагментация, но не подтверждена конкретная утечка. Поздний baseline около 35 KiB ниже раннего 50 KiB требует дальнейшего измерения владельцев, а не немедленной правки.

## Кандидаты на перенос в PSRAM

### A. Низкий риск, уже применено или почти применено

- MusicBridge download chunk 16 KiB — уже PSRAM; оставлять capability SPIRAM|8BIT.
- Opus codec stack — уже создаётся с PSRAM capabilities.
- Wake-word encode stack — PSRAM stack, internal только TCB.
- Radio PCM и крупные временные vectors — использовать существующий PsramAllocator.
- Большие non-DMA metadata buffers MusicService/Weather — кандидат при доказанном pressure, но сначала измерить lifetime.

### B. Только после A/B измерения

- Weather body/JSON buffers: можно проверить PSRAM, но JSON parser и TLS callback latency должны быть измерены.
- Долгоживущие MusicService vectors/строки: возможна экономия INTERNAL, но нужно проверить fragmentation и кодировку имён.
- LVGL image cache: уже в PSRAM; менять размер только после оценки UI cache hit rate.

### C. Не переносить автоматически

- I2S/DMA audio buffers;
- display DMA buffer;
- LVGL/task stacks, для которых source прямо требует internal/cache-safe memory;
- TCB/static control blocks;
- small hot-path allocations, где выигрыш меньше фрагментации и latency риска.

В устройстве остаётся более 7 MiB PSRAM, но это не делает любую allocation безопасной для внешней памяти. Capability, DMA, cache и realtime требования важнее одного числа свободных байт.

## Критические сочетания

| Сценарий | Что видно в логе | Риск |
|---|---|---|
| Weather + MQTT/TLS | поздний Weather body: 15 647 B free, largest 7 680 B, min 10 544 B | высокий transient pressure; нужен overlap-тест |
| Music download + LVGL overlay | второй download до 18 711 B free, largest 7 680 B; buffer в PSRAM | самый глубокий наблюдаемый пик |
| Music download + Weather | прямой overlap в снимке не подтверждён | проверить admission/gate и HTTP overlap |
| Radio + Weather | radio_active подавляет refresh в Weather Tick | логически защищён, проверить переходы |
| Audio playback + TLS | gaps при большом свободном heap | скорее transport/jitter, память не доказана |
| Repeated mDNS discovery | init одноразовый, query result освобождается | leak не подтверждён |
| Repeated screen open/close | SetupUI guard, значения частично восстанавливаются | leak не подтверждён |
| Repeated MusicBridge reconnect | not-found не создаёт download buffer | проверить 20 циклов discovery/reconnect |

## Что измерить следующим этапом

Это план диагностики, а не внесённые изменения.

1. В controlled diagnostic build добавить lifecycle snapshots для INTERNAL|8BIT, DMA и SPIRAM: до/после Weather task, HTTP open, body release, JSON parse, mDNS init, download buffer, overlay, tab switch и audio start/stop.
2. Периодически снимать heap_caps_get_info, heap_caps_get_largest_free_block и heap_caps_get_minimum_free_size по capability. heap_caps_dump запускать только в отдельном offline/diagnostic режиме.
3. Снять uxTaskGetSystemState вне realtime audio: имя, core, priority, stack high-water mark, lifetime и число экземпляров. Отдельно измерить deepest MusicDownload error/callback path.
4. Добавить счётчики lifecycle для Weather, music_poll, mDNS query result, HTTP client, overlay и UI screen objects. Цель — показать created == destroyed после каждого сценария.
5. Выполнить одинаковые сценарии: idle 30 минут; 10 Weather refresh; 20 mDNS failures; 5 user/generated downloads; 10 tab cycles; Radio+Weather; речь с MQTT/TLS. Сравнивать baseline после возврата в idle.
6. Для audio gaps записывать RSSI, reconnects, UDP receive/drop counters, sequence, decode queue, playback queue и power-save state. Не включать тяжёлый heap tracing постоянно в аудиопути.
7. Для MusicDownload проверить, возвращается ли largest block после не только success, но и HTTP timeout, короткого ответа, неверного content length и отмены.
8. Увеличение stack MusicDownload рассматривать только после измерения: текущий запас 512–532 B уже мал, но размер изменения следует выбрать по максимальной глубине и доступному INTERNAL резерву.

## Итоговый ответ на главный вопрос

Внутренняя RAM не «исчезает» одним подтверждённым владельцем. Она расходуется на постоянную стоимость AFE/audio, LVGL/DMA, Wi-Fi/MQTT/TLS, одноразовый mDNS и metadata, а во время Weather и MusicDownload дополнительно дробится временными allocations. После освобождения крупных временных ресурсов значительная часть возвращается; в доступном логе нет доказательства утечки.

Главная зона риска — сочетание низкого крупнейшего блока (7,5 KiB), исторического минимума 10,5 KiB и малого stack margin music_poll 512–532 B. PSRAM используется правильно и остаётся с большим запасом. Следующий безопасный шаг — точечная runtime-диагностика владельцев и overlap-сценариев, без переноса DMA/audio критичных буферов и без тяжёлого tracing в production.

Проверки, выполненные для этого отчёта: read-only анализ ветки/HEAD/status, sdkconfig, project description, partition table, исходников Weather/MusicBridge/MusicService/audio/SD/LVGL, полный доступный serial log и сводная обработка 817 RAM-снимков. Сборка, прошивка, reset, очистка и изменение Git-истории не выполнялись.
