# XiaoZhi Emote & Expression System (FNK0104S Integration)

## 1. Обзор архитектуры

Система визуальных выражений лица AI персонажа в XiaoZhi базируется на штатных компонентах:
- **`esp_emote_expression`**: Официальный компонент Espressif (`espressif2022/esp_emote_expression ^1.0.2`), обеспечивающий рендеринг анимированных表情 (emotes) и обработку `.eaf` файлов анимаций.
- **`emote::EmoteDisplay`** (`main/display/emote_display.h / .cc`): Класс-обёртка, реализующий интерфейс `Display` и транслирующий события устройства в команды изменения выражения лица.

---

## 2. Состояния и события (Visual States)

При инициализации прошивки на плате **FNK0104S** создает экземпляр `emote::EmoteDisplay`, связывая его с контроллером экрана ST7796.

Визуальные состояния устройства маппятся на внутренние события `EmoteDisplay`:

| Состояние XiaoZhi (`DeviceState`) | Событие Emote (`emote_event_t`) | Анимация / Выражение |
| :--- | :--- | :--- |
| `kDeviceStateIdle` | `EMOTE_MGR_EVT_IDLE` | Анимация ожидания / спокойные глаза |
| `kDeviceStateListening` | `EMOTE_MGR_EVT_LISTEN` | Анимация прослушивания пользователя |
| `kDeviceStateSpeaking` | `EMOTE_MGR_EVT_SPEAK` | Анимация говорения (TTS) |
| `kDeviceStateConnecting` / `THINKING` | `EMOTE_MGR_EVT_SYS` | Режим обработки / размышления |
| `kDeviceStateFatalError` | `EMOTE_MGR_EVT_SET` | Состояние ошибки |

---

## 3. Ресурсы и Партиции (Expression Assets)

- Ресурсы анимаций хранятся в формате `.eaf` в специализированной партиции или файловой системе `esp_mmap_assets`.
- Конфигурационный флаг `CONFIG_USE_EMOTE_MESSAGE_STYLE=y` активирует использование `EmoteDisplay` вместо стандартного текста LVGL.
- При получении текстовых сообщений или системных уведомлений `EmoteDisplay` динамически накладывает субтитры поверх анимированного лица без необходимости вызывать собственный движок отрисовки.

---

## 4. Подключение на FNK0104S

В файле `main/boards/freenove-fnk0104s/freenove_fnk0104s_board.cc`:
```cpp
#if CONFIG_USE_EMOTE_MESSAGE_STYLE
    display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
    display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, ...);
#endif
```

При этом `EmoteDisplay` захватывает панель `panel` и `panel_io`, осуществляя отрисовку со скоростью 30 FPS и используя два буфера кадра для отсутствия мерцания (double buffer).
