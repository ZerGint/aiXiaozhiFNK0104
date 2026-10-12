# FNK0104S — MCP limit audit and fix

Дата исследования: 2026-10-12
Проект: `F:/FNK0104AI/aiXiaozhiFNK0104`

## Вывод

Сообщение `The number of tools has reached the limit 32` отсутствует в прошивке,
ESP-IDF и исходниках этого репозитория. В локальном MCP-коде нет ограничения по
количеству инструментов. Единственное локальное ограничение — тело одной страницы
`tools/list` размером 8000 байт с поддержкой `nextCursor` в
`main/mcp_server.cc:GetToolsList()`.

Сбой возник на стороне XiaoZhi/MCP-сервера или его адаптера к LLM: после добавления
четырёх Weather-инструментов устройство публиковало 33 публичных инструмента, тогда
как сервер принимал максимум 32. Это не ограничение MCP-протокола и не лимит
`std::vector` прошивки.

## Регистрация до исправления

Для FNK0104S порядок регистрации был таким: Home Assistant, Radio/SD, Weather,
Music, затем common-инструменты и user-only-инструменты из `Application::Initialize()`.
`McpServer::AddTool()` проверяет только дубликаты имён; при превышении 32 он не
отказывал. Поэтому на устройстве проходили все 42 вызова регистрации:

- 33 публичных инструмента, передаваемых модели через `tools/list`;
- 9 user-only-инструментов, скрытых от модели без `withUserTools=true`.

При жёстком серверном пределе 32 последним публичным элементом в этом порядке был
`self.screen.set_theme`. Это вывод из порядка регистрации; точный серверный отказ
подтверждается только логом XiaoZhi. В имеющихся локальных логах есть строки `Add
tool`, но нет текста ошибки сервера.

| Группа | Инструмент | Назначение |
|---|---|---|
| common | `self.get_device_status` | Состояние устройства |
| common | `self.audio_speaker.set_volume` | Громкость |
| common | `self.screen.set_brightness` | Яркость |
| HA | `homeassistant.call_service` | Вызов сервиса Home Assistant |
| HA | `homeassistant.get_states` | Чтение состояний Home Assistant |
| Radio | `radio.add_favorite` | Добавить радиостанцию в избранное |
| Radio | `radio.get_current` | Текущая радиостанция |
| Radio | `radio.list_favorites` | Список избранных станций |
| Radio | `radio.play_favorite` | Воспроизвести избранную станцию |
| Radio | `radio.remove_favorite` | Удалить станцию из избранного |
| Radio | `radio.search_stations` | Поиск радиостанций |
| Radio | `radio.play_station` | Воспроизвести найденную станцию |
| Radio | `radio.stop` | Остановить радио |
| Media | `media.play_sd` | Воспроизвести файл с SD |
| Media | `media.list_sd` | Список файлов SD |
| Media | `media.search_sd` | Поиск файла на SD |
| Media | `media.pause` | Пауза/продолжение |
| Media | `media.next` | Следующий трек |
| Media | `media.previous` | Предыдущий трек |
| Media | `media.stop` | Остановить SD-плеер |
| Weather | `self.weather.search_city` | Поиск городов и кандидатов |
| Weather | `self.weather.get_city` | Прочитать сохранённый город |
| Weather | `self.weather.set_city` | Сохранить город по проверенному candidate_id |
| Weather | `self.weather.get_weather` | Разовый текущий прогноз |
| Music | `music.generate` | Запустить генерацию |
| Music | `music.generation_status` | Проверить генерацию |
| Music | `music.download_ready` | Скачать готовую генерацию |
| Music | `music.search_library` | Поиск сгенерированной библиотеки |
| Music | `music.download_library` | Скачать трек генерации |
| Music | `music.search_user_library` | Поиск пользовательской библиотеки |
| Music | `music.download_user_library` | Скачать пользовательский MP3 |
| Music | `music.play_generated` | Воспроизвести локальную генерацию |
| user-only | `homeassistant.set_config` | Настроить Home Assistant |
| user-only | `music.set_bridge_url` | Диагностическая настройка Music Bridge |
| user-only | `self.get_system_info` | Системная информация |
| user-only | `self.reboot` | Перезагрузка |
| user-only | `self.upgrade_firmware` | Обновление по URL |
| user-only | `self.screen.get_info` | Параметры экрана |
| user-only | `self.screen.snapshot` | Снимок экрана |
| user-only | `self.screen.preview_image` | Предпросмотр изображения |
| user-only | `self.assets.set_download_url` | URL загрузки assets |

## Исправление

Четыре Weather-инструмента объединены в один:

`self.weather(action, query, country, region, candidate_id, forecast_days)`

`action` принимает `search_city`, `get_city`, `set_city` или `get_weather`.
`action` обязательный; остальные параметры имеют безопасные значения по умолчанию,
поэтому схема совместима с текущим MCP-парсером XiaoZhi. Операции поиска и разового
прогноза по-прежнему идут через существующий Weather worker и callback. `set_city`
по-прежнему принимает только свежий проверенный `candidate_id`, пишет координаты и
метаданные в NVS и обновляет экран. `get_city` читает постоянное местоположение.
Неоднозначный поиск и одноразовый прогноз не изменены.

После объединения ожидаемый состав — 39 зарегистрированных инструментов:

- 29 публичных (`tools/list`), ниже серверного предела 32;
- 9 user-only.

Добавлены диагностические строки:

- `MCP_TOOL_SUMMARY total=... public=... user_only=...` после полной регистрации;
- `MCP_TOOLS_LIST page_tools=... bytes=... user_only=... next=...` для каждой страницы.

Они позволяют на устройстве отдельно проверить количество инструментов и фактический
размер MCP-описаний. Повторной регистрации в FNK-пути не обнаружено; дубликаты имён
отсекаются в `McpServer::AddTool()`.

## Проверка

- `python -m unittest scripts.tests.test_weather_service -v`: **11/11 OK**.
- `idf.py build` для FNK0104S: **успешно**.
- Размер приложения после исправления: `0x3DD1B0`; свободно в app-разделе
  `0xA2E50` (14%).
- Полный host-набор: **128 тестов, 2 старых несвязанных отказа** в
  `scripts/tests/test_build.py` (ожидается старое имя wake-word NIHAOXIAOZHI и
  устаревшая проверка аргументов CMake).

## Проверка на устройстве

Прошивка и коммит этим исследованием не выполнялись. После отдельного разрешения
нужно снять boot/runtime log и убедиться в строках `MCP_TOOL_SUMMARY total=38
public=29 user_only=9`, затем вызвать `tools/list` и проверить отсутствие ошибки
лимита. Для функционального smoke-теста выполнить четыре действия `self.weather`
по очереди: `search_city`, `get_city`, `set_city`, `get_weather`, включая сценарий
с несколькими кандидатами и повторной загрузкой после перезапуска.
