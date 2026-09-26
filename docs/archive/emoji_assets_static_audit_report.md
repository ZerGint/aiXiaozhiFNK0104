# Статический аудит стандартных emoji assets / `EmojiCollection` для FNK0104S

Дата: 20.09.2026. Проверен только исходный код и текущий `sdkconfig` проекта. Production-код не изменялся, сборка, прошивка и оптимизация не выполнялись.

## Конфигурация FNK0104S

Текущий `sdkconfig` выбирает `CONFIG_BOARD_TYPE_FREENOVE_FNK0104S=y`, `CONFIG_FLASH_DEFAULT_ASSETS=y` и не выбирает `CONFIG_USE_EMOTE_MESSAGE_STYLE` ([sdkconfig](/F:/FNK0104AI/aiXiaozhiFNK0104/sdkconfig:973), [sdkconfig](/F:/FNK0104AI/aiXiaozhiFNK0104/sdkconfig:1045), [sdkconfig](/F:/FNK0104AI/aiXiaozhiFNK0104/sdkconfig:1141)).

Для FNK0104S `main/CMakeLists.txt` задаёт `DEFAULT_EMOJI_COLLECTION noto-color-emoji_64` вместе с 20-pixel Noto и Material Symbols fonts ([main/CMakeLists.txt](/F:/FNK0104AI/aiXiaozhiFNK0104/main/CMakeLists.txt:853)). При сборке default assets этот параметр передаётся в `build_default_assets.py`, а скрипт добавляет список файлов в `index.json` как `emoji_collection` ([main/CMakeLists.txt](/F:/FNK0104AI/aiXiaozhiFNK0104/main/CMakeLists.txt:1272), [build_default_assets.py](/F:/FNK0104AI/aiXiaozhiFNK0104/scripts/build_default_assets.py:305), [build_default_assets.py](/F:/FNK0104AI/aiXiaozhiFNK0104/scripts/build_default_assets.py:728)). Поэтому сам факт наличия массива стандартных PNG в assets является ожидаемым результатом конфигурации.

При `CONFIG_USE_EMOTE_MESSAGE_STYLE=n` board создаёт `SpiLcdDisplay`, являющийся наследником `LcdDisplay`; `EmoteDisplay` выбирается только в альтернативной ветке ([freenove_fnk0104s_board.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/boards/freenove-fnk0104s/freenove_fnk0104s_board.cc:254), [lcd_display.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.h:193)).

## Lifecycle коллекции

1. `Assets::LvglStrategy::Apply()` читает `emoji_collection` из `index.json`. Для каждого объекта получает указатель на asset partition через `GetAssetData`, создаёт `new LvglRawImage(ptr, size)` и помещает wrapper в `EmojiCollection::AddEmoji()` ([main/assets.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/assets.cc:334), [main/assets.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/assets.cc:350), [main/assets.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/assets.cc:356)).
2. `EmojiCollection` хранит `std::map<std::string, LvglImage*>`; `AddEmoji()` вставляет map-node, а `GetEmojiImage()` выполняет единственный обычный lookup в проекте ([emoji_collection.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/emoji_collection.h:13), [emoji_collection.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/emoji_collection.cc:7)).
3. Одна `shared_ptr` записывается в light theme и dark theme, затем передаётся в `Display::SetEmojiCollection()` ([main/assets.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/assets.cc:364)). `LvglTheme` хранит её в поле `emoji_collection_` ([lvgl_theme.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/lvgl_theme.h:30), [lvgl_theme.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/lvgl_theme.h:55)).
4. Базовый `Display::SetEmojiCollection()` — пустой virtual no-op ([display.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/display.h:49)). Единственный override в проекте находится у Waveshare RGB matrix; у FNK0104S override нет. Значит вызов из `Assets` на FNK не создаёт дополнительного владельца коллекции в display object.
5. При уничтожении коллекции деструктор удаляет все `LvglImage*` wrappers и очищает map ([emoji_collection.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/emoji_collection.cc:21)). `LvglRawImage` хранит только `data`/`data_size` в `lv_img_dsc_t` и не освобождает mmap payload ([lvgl_image.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_display/lvgl_image.cc:10), [lvgl_image.h](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lvgl_image.h:12)). Следовательно, persistent heap cost этого пути — `EmojiCollection`, map nodes, строки и `LvglRawImage` objects; PNG bytes остаются в mapped asset partition.

## Все потребители

Статический поиск даёт только один вызов `EmojiCollection::GetEmojiImage()` в основном LVGL display: [lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2218). Второй потребитель — Waveshare RGB matrix, который к FNK0104S не относится ([rgb_matrix_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/boards/waveshare/esp32-s3-rgb-matrix/rgb_matrix_display.cc:341)).

`Application` передаёт серверное поле `emotion` в `Display::SetEmotion()` и также устанавливает состояния `neutral`, `listening`, `speaking` и другие через этот же интерфейс ([application.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/application.cc:817), [application.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/application.cc:1166)). Совпадение строк `thinking`, `loving`, `shocked`, `embarrassed` и т. п. само по себе не означает обращение к PNG.

## Что происходит на FNK0104S

`LcdDisplay::SetupUI()` создаёт `emoji_image_`, скрывает его, затем создаёт RoboEyes canvas и выделяет buffer 240×120×RGB565: сначала в PSRAM, затем fallback в INTERNAL ([lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:1131)). При успешном выделении buffer `LcdDisplay::SetEmotion()` входит в первую ветку, изменяет состояние `RoboEyes` и делает `return` ([lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2133), [lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2203)). До `GetEmojiImage()` выполнение не доходит.

Тот же RoboEyes path используется для runtime state update: `connecting` → `thinking`, `listening` → `listening`/`surprised`, `speaking` → `speaking`, `idle` → `neutral` ([lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2481)).

Единственный PNG fallback расположен после раннего `return`: если RoboEyes canvas/buffer не готовы, код проверяет `emoji_collection`, вызывает `GetEmojiImage()`, а затем может передать descriptor в GIF controller или `lv_image_set_src` ([lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2208), [lcd_display.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/display/lcd_display.cc:2218)). Поэтому статически можно утверждать:

- при штатно успешно созданном RoboEyes buffer стандартные PNG в FNK UI не lookup-ятся и не устанавливаются в LVGL image widget;
- collection и wrappers всё равно создаются заранее в `Assets::Apply()`;
- fallback PNG path остаётся защитной веткой при невозможности создать RoboEyes buffer или при использовании другого display implementation; это не штатный текущий путь FNK при успешной инициализации PSRAM/INTERNAL.

## Итог

Для текущей конфигурации FNK0104S стандартные `noto-color-emoji_64` assets являются **загружаемыми, но штатно не используемыми** PNG-изображениями: их wrappers и map entries создаются и живут в `EmojiCollection`, тогда как отображение эмоций выполняет RoboEyes. В исходниках нет другого FNK-потребителя, который бы вызвал `GetEmojiImage()`.

Это подтверждает, что наблюдавшаяся persistent INTERNAL/DMA нагрузка от emoji setup относится к ownership/metadata allocations (`shared_ptr`, map, строки, `LvglRawImage`), а не к декодированию или копированию PNG pixel data. Сам аудит не доказывает, что fallback никогда не сработает при редкой ошибке выделения RoboEyes buffer; для такого утверждения нужен runtime failure injection, который в рамках текущего запроса не выполнялся.


