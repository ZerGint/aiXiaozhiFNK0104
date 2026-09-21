# FNK0104S Home Assistant Settings Portal

Дата: 21.09.2026. Базовая production-ветка: `cc0c260`. Изменения выполнены в отдельном worktree `aiXiaozhiFNK0104_settings_audit`; основной dirty worktree не затрагивался.

## Поведение

Портал выключен после загрузки и не хранит состояние включения в NVS. Он запускается только кнопкой с шестерёнкой в верхней панели FNK0104S и останавливается повторным нажатием. При потере Wi-Fi обработчик сетевого события останавливает сервер. Кнопка меняет цвет для состояний OFF/ON, а при запуске показывает локальный URL по текущему IP.

Существующий `WifiManager` и provisioning-путь не менялись. Конфигурация Home Assistant остаётся в `Settings("ha")`; добавленный mutex защищает URL/token при параллельном обращении UI, HTTP и MCP. Другие платы не получают сервер, кнопку или новые зависимости.

## HTTP API

- `GET /` — статическая responsive HTML-страница без CDN и внешних зависимостей;
- `GET /api/ha` — URL и только boolean `token_configured`;
- `POST /api/ha/test` — проверка переданных URL/token через существующий Home Assistant HTTP/TLS клиент, без сохранения;
- `POST /api/ha/save` — сохранение URL и token в существующий NVS namespace; пустой token сохраняет прежний.

Токен не возвращается страницей, JSON API или логами. Тела запросов ограничены, URL/token валидируются по типу и длине.

## Проверка

Финальная production-сборка завершилась успешно: `build/xiaozhi.bin`, 3,629,408 байт (`0x376160`), 12% свободно в app-разделе. SHA-256: `F3AE0E8639776BF47F39E486C26BD61FA762C6AEB5F748C7FB4F5282502B819E`.

Host-тесты ранее дали 112 PASS и один известный pre-existing failure `test_configure_build_uses_all_cmake_values_in_one_run` из-за дополнительного `-DIDF_CMAKE_CHECK_WARN_ONLY=ON` в текущем репозитории. `git diff --check` не обнаружил whitespace errors; `clang-format` недоступен в окружении.

Финальный app-only flash выполнен на COM13 по адресу `0x20000`; esptool стирал только диапазон `0x00020000..0x00396fff` и подтвердил hash. Bootloader, partition table, assets и NVS не записывались. После reset устройство получило IP `192.168.31.99`, дошло до `PROTOCOL_CONNECTED` и `State: activating -> idle`, Weather завершился HTTP 200. В этой контрольной загрузке без нажатия кнопки маркеров `HASettings` нет, что подтверждает отсутствие auto-start. В предыдущем runtime capture с ручным нажатием зафиксированы `HASettings before_start/after_start` и вызов `before_test/after_test`; panic/assert/watchdog не наблюдались.

Финальный boot log: `serial_settings_portal_final_boot2.txt`.
