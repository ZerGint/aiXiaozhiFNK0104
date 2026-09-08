# План устранения проблем памяти радиотракта

Дата: 2026-09-08.

## Цель и статус

Устранить потери ресурсов при OOM, защитить каталог и избранное от повреждения,
затем уменьшить INTERNAL/DMA peaks без регрессий радио и AI.

Пользователь разрешил сохранить план и приступить к реализации. Безопасные
изменения этапов 1–3 выполняются последовательно. Архитектурные решения этапа 4
и изменения функционального поведения требуют отдельного согласования.
Fail-closed сохранение из этапа 1 — явно предложенная защита ошибочного сценария:
вместо ложного успеха при OOM возвращается ошибка, успешное поведение сохраняется.

На старте реализации ветка `astra`. При первой проверке обнаружено изменение
`log_5min_stage7_diag2.txt`; при повторной проверке оно уже отсутствовало.
Агент этот файл не изменял. Продолжение возможно с чистым рабочим деревом.

Аудит описывал предыдущий снимок исходников. Перед каждым fix повторно проверить
текущий код; диагностические точки и другие детали могли измениться.

## Неизменяемые условия

- Bluetooth/BLE/NimBLE и SEND_WAKE_WORD_DATA остаются выключенными.
- Radio prebuffer: 1800 мс. Wi-Fi при радио: PERFORMANCE / WIFI_PS_NONE.
- Не менять I2S/DMA, TLS settings, decoder architecture, AudioService,
  MCP/AI lifecycle, формат каталога и избранного без отдельного разрешения.
- Не менять malloc threshold/reserve наугад и не переносить buffers в PSRAM
  без проверки текущего размещения и capability requirements.
- В аудите существующая сборка использовала ESP-IDF 6.1.0, хотя AGENTS.md
  рекомендует 6.0.2. Проверить окружение; миграцию SDK не смешивать с fixes.
- Сборка не заменяет физическую проверку устройства.

## Workflow commits

ONE LOGICAL FIX = ONE COMMIT.

Перед каждым изменением:

1. Проверить git status и текущую ветку. Дерево должно быть чистым.
2. Сформулировать одну конкретную проблему.
3. Прочитать текущую реализацию и применимые AGENTS.md.

После изменения:

1. Проверить минимальность git diff и отсутствие generated/случайных файлов.
2. Форматировать только затронутые C/C++ файлы по .clang-format.
3. Выполнить git diff --check и подходящие tests/build FNK0104S.
4. Проверить отсутствие нежелательных изменений sdkconfig.
5. Только после успешных проверок создать отдельный commit.
6. Показать hash, subject и отчёт; git status должен быть clean.

Не коммитить незавершённые fixes, неуспешные проверки и изменения другой задачи.
Не squash, не force-push, не rebase. Работать только в текущей отдельной ветке.
Зависимые части одного исправления объединять, независимые оптимизации разделять.
Для нетривиального commit добавить body: проблема, изменение, безопасность.

Формат отчёта после каждого commit:

```text
COMMIT: <hash> <subject>
PROBLEM:
CHANGE:
WHY SAFE:
MEMORY EFFECT: leak / peak / fragmentation / allocations / none
FILES:
BUILD: PASS/FAIL
git diff --check: PASS/FAIL
FUNCTIONAL BEHAVIOR CHANGED: YES/NO
PHYSICAL TEST REQUIRED:
GIT STATUS: clean / not clean
```

Документ с планом оформить отдельным documentation commit после разрешения
начального dirty-tree состояния. Firmware build для документа не требуется;
проверить содержимое и git diff --check.

## Этап 1. Защита ресурсов и данных при OOM

### 1.1 Загрузка файлов RadioStorage

- Subject: `fix(radio-storage): make JSON file loading exception-safe`
- RAII для FILE* и cJSON DOM; закрыть пути исключений при создании строк/vector.
- Эффект: отсутствие потери файла/DOM при bad_alloc, обычная загрузка прежняя.
- Проверки: успешная загрузка, отсутствующий/повреждённый файл, моделирование
  allocation failures и проверка освобождения ресурсов.

### 1.2 Безопасное сохранение RadioStorage

- Subject: `fix(radio-storage): preserve saved stations on JSON allocation failure`
- Проверять создание array, station objects, полей и результат сериализации.
- RAII для промежуточных объектов; при OOM остановить save до записи.
- Не подставлять [] вместо неудачной сериализации; не сохранять частичный JSON.
- Эффект: устранение OOM-утечек и защиты каталога/избранного от перезаписи.
- Проверки: отказы на разных allocations; исходный файл остаётся неизменным.
- Ownership и проверки создания одной JSON-операции — одна логическая единица,
  если раздельное внесение не обеспечивает корректности.

### 1.3 RadioBrowser

- Subject: `fix(radio-browser): make JSON response ownership exception-safe`
- RAII для DOM, print buffers и HTTP handle в search/UUID lookup/ответах.
- Проверки: HTTP error, malformed JSON, пустой ответ, allocation failures.
- Успешные ответы, порядок и фильтрация остаются прежними.

### 1.4 MediaPlayer

- Subject: `fix(media-player): make JSON listing ownership exception-safe`
- Защитить JSON-ответы списков от аналогичных исключений.
- Проверки: сравнение успешных ответов и освобождение ресурсов при ошибках.

## Этап 2. Простые оптимизации без изменения архитектуры

### 2.1 Убрать дублирование сериализованного каталога

- Subject: `perf(radio-storage): remove duplicate serialized JSON copy`
- Записывать принадлежащий RAII-объекту cJSON print buffer напрямую.
- Эффект: минус одна полная копия JSON; формат файла сохраняется.

### 2.2 Поиск без временных lowercase strings

- Subject: `perf(radio-search): avoid temporary lowercase strings`
- Allocation-free ContainsInsensitive; сохранить текущую байтовую семантику
  регистра, не вводить Unicode-нормализацию.
- Проверки: пустые строки, начало/конец/отсутствие совпадения, регистр ASCII,
  UTF-8 и байты с установленным старшим битом; unsigned char для cctype.
- Эффект: меньше частых малых allocations, не обещание устранения всех AES OOM.

### 2.3 Reserve bounded results

- Subject: `perf(radio-search): reserve bounded station results`
- Reserve только для vectors с известным небольшим limit.
- Не резервировать весь каталог с произвольным запасом.
- Эффект: устранение роста backing storage ограниченных выдач.

### 2.4 Move metadata UUID lookup

- Subject: `perf(radio-browser): move resolved station metadata`
- Убрать доказанную ненужную копию, когда источник больше не используется.
- LOW VALUE: небольшая экономия, не самостоятельное решение SRAM pressure.

После этапа — аппаратная контрольная точка; не переходить к сложным изменениям
только потому, что они перечислены в плане.

## Этап 3. Диагностика текущей прошивки

- Subject: `chore(radio): clarify heap diagnostics and allocation lifetimes`
- Сначала найти существующие точки в текущем коде.
- Уточнить единицы stack HWM; точки после фактического освобождения объектов.
- Логировать размер/capacity и heap location основных buffers.
- Использовать сопоставимые capability masks; ограничить частоту логирования.
- Отдельно согласовать временную диагностику failed allocations: size, caps,
  alignment, callsite/task. Callback без allocations и тяжёлого логирования.

Аппаратная матрица:

| Сценарий | Измерение/проверка |
| --- | --- |
| HTTP/HTTPS, MP3/AAC/AAC+ | INTERNAL/DMA free/min/largest, TLS/decoder errors |
| Local search limit=1/10 | Пик загрузки каталога |
| Online search | Body/DOM/filter peak |
| UUID lookup/favorite play | Разрешение и обновление станции |
| Save во время радио | SRAM pressure, непрерывность звука |
| Повторные reconnect | Восстановление heap после cleanup |
| Voice interruption/resume | Взаимодействие радио и AI |

Фиксировать firmware SHA, IDF revision/config, размер каталога и ответы.
Для leak-проверки повторять операцию без роста каталога и дождаться очистки
ресурсов, включая освобождение завершённых задач idle task.

## Этап 4. Крупные пики: отдельное согласование перед реализацией

### 4.1 Последовательная запись существующего JSON array

- Subject: `perf(radio-storage): serialize station files incrementally`
- Писать по одной станции вместо полного DOM/serialized buffer.
- Сохранить JSON array и все поля; vector пока может оставаться целиком.
- Проверить escapes, пустой каталог, длинные строки и ошибки SD.

### 4.2 Последовательный локальный поиск

- Subject: `perf(radio-storage): search catalog without loading all stations`
- Читать станции последовательно, хранить только N результатов.
- Сохранить порядок, codec handling, фильтры и limit.
- Явно сохранить прежнее поведение при повреждении конца файла: не возвращать
  частичный успех, если прежняя реализация отклоняла весь каталог.

### 4.3 Последовательный разбор RadioBrowser

- Subject: `perf(radio-browser): parse station responses incrementally`
- Устранить полный DOM ответа; предварительно выбрать проверенный parser.
- Проверить границы HTTP chunks, JSON escapes, UTF-8, неизвестные поля,
  порядок, фильтрацию и повреждение конца ответа.
- Не делать parser простым поиском фигурных скобок.
- Высокий ожидаемый эффект: старые логи показали особенно сильный DMA-провал
  на parse/filter. Связь с текущей прошивкой подтвердить измерениями.

### 4.4 Ограничения входных данных

- Subject: `fix(radio-browser): bound response memory usage`
- Выбрать body/field limits по реальным ответам и согласовать реакцию.
- Изменяет поведение для слишком больших данных: отдельное разрешение.
- Не усекать названия и URL молча.

### 4.5 Последовательное обновление каталога

- Subject: `perf(radio-storage): update catalog incrementally`
- После проверки streaming reader/writer убрать полный load при update.
- Сохранить UUID/fallback matching, порядок и правила непустых полей.
- Не смешивать с миграцией на NDJSON.

## Этап 5. Отдельные проблемы надёжности

- Удаление старого файла перед rename: изучить FAT/VFS guarantees и согласовать
  восстановление при сбое замены или питания.
- Проверка fflush/fclose: возвращать ошибку записи, определить судьбу tmp.
- Условная AES leak в локальном SDK: подтвердить достижимость; выбрать
  поддерживаемое исправление зависимости. Не редактировать vendor output.
- Возможное перекрытие radio tasks при долгом Stop: воспроизвести отдельно;
  исправление lifecycle требует отдельного согласования.

## Отложить до доказанной необходимости

- Перенос больших buffers в PSRAM: многие уже предпочитают external heap.
- Reuse PCM/decoder buffers: только при значимом измеренном allocation churn.
- Уменьшение стеков: только по worst-case HWM с запасом.
- Изменение allocator threshold/reserve.
- NDJSON, UUID index, binary DB, SQLite, PMR/custom allocators.
- Сокращение playback metadata: ожидаемый эффект мал.

## Критерии завершения

- Нет утечек на проверенных error paths.
- OOM не приводит к записи пустого/неполного каталога вместо корректного.
- После повторных одинаковых операций heap восстанавливается.
- Есть измеренный DMA reserve относительно реальных AES allocations.
- Нет регрессий радио, поиска, избранного, voice interruption/resume.
- Host/build проверки успешны; оставшиеся физические проверки перечислены.

## Журнал реализации

- 2026-09-08: план сохранён. Начальное постороннее изменение лога отсутствует
  при повторной проверке; файл агентом не изменялся. Реализация начинается
  с exception-safe загрузки RadioStorage.
- Documentation commit: `f159081 docs: record radio memory remediation plan`.
- Этап 1.1 подготовлен в `main/media/radio_storage.cc`: unique_ptr для FILE*
  и cJSON DOM. Canonical build FNK0104S на IDF 6.1.0: PASS; merge-bin: PASS;
  свободно 21% app partition. Проверка затронутых участков clang-format 23.1.0
  и git diff --check: PASS. Физические и fault-injection проверки не выполнены.
- Первая попытка commit этапа 1.1 остановлена: host suite имел 1 failure и 5 errors.
  Failure: `VersionTests.test_chip_defaults_only_contain_target_overrides` —
  CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA повторён в base и esp32c3 defaults
  уже в HEAD. Errors: пять тестов удаляют TemporaryDirectory до возврата cwd,
  что вызывает Windows WinError 32 и последующий RecursionError.
  После отдельного разрешения пользователя обе причины воспроизведены на
  чистом `f159081` без RAII-fix и исправлены отдельными commits:
  `727d6f0` — наследование одинаковых mbedTLS defaults;
  `3991d9c` — восстановление cwd до удаления TemporaryDirectory.
  Canonical build после каждого blocker: PASS. Все 67 host tests: PASS.
  Generated sdkconfig, sdkconfig.h и sdkconfig.json после удаления defaults
  побайтово идентичны исходным; merged defaults всех пяти targets идентичны.
  Все 192 assertion-вызова в Windows-тестах сохранены.
- RAII возвращён из stash `28d59adaf1791e84d5a7119890404ec5e2c92c56`.
  Diff radio_storage.cc побайтово совпадает с сохранённым diff; SHA256:
  `3bbafcb2821ea6a27f36a02cd6c25c1667978cb9b0ba0ddc6506cc1021f24935`.
  Повторные host tests: 67/67 PASS; canonical build и merge-bin: PASS;
  clang-format затронутых участков и git diff --check: PASS.
  Исходный stash сохранён как резервная копия.
  Этапы 1.2 и далее ещё не начаты.
- Standard IDF export не смог найти инструменты в .espressif/tools; сборка
  выполнена с существующими PlatformIO tools через IDF_PATH/ESP_IDF_VERSION,
  IDF_PYTHON_ENV_PATH и PATH, без миграции SDK. Для форматирования установлен
  clang-format 23.1.0 в существующий Python venv (18 не поддерживает ExceptShortType).
