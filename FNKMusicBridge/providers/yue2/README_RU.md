# Провайдер WanGP YuE2

`Yue2Provider.exe` — внешний адаптер для локального WanGP с backend YuE2.
Основной `FNKMusicBridge.exe` запускает его через JSON-протокол и получает
готовый WAV-файл.

Настройки подключения находятся в `config.json` этой папки. Их можно менять
без пересборки основного моста:

- `wan_gp_url` — адрес работающего WanGP;
- `generation_timeout_sec` — таймаут генерации;
- `poll_interval_sec` — интервал проверки состояния;
- `audio_duration` — максимальная длительность результата.

Не переименовывайте `provider.json` и `Yue2Provider.exe`.

FNK_TO_WANGP_REFERENCE.jsonc — справочник HTTP-команд адаптера к WanGP YuE2. Файл предназначен для просмотра и диагностики и не изменяет работу EXE.
