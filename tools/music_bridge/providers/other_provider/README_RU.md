# Папка для другого провайдера

Сюда помещается отдельный адаптер провайдера. Основной мост не содержит
логики конкретного API и запускает адаптер как отдельный процесс.

Минимальная структура:

```text
other_provider/
  provider.json
  config.json
  OtherProvider.exe
```

Пример `provider.json`:

```json
{
  "id": "other_provider",
  "name": "Другой провайдер",
  "version": "1.0",
  "adapter": "OtherProvider.exe",
  "config": "config.json",
  "capabilities": {
    "lyrics": true,
    "style": true,
    "duration": true
  }
}
```

Адаптер получает одну строку JSON через `stdin`:

```json
{
  "protocol": 1,
  "job_id": "...",
  "title": "...",
  "style": "...",
  "lyrics": "...",
  "duration_seconds": 320,
  "timeout_seconds": 1800,
  "output_path": "C:/.../generated/job.wav",
  "config_path": "C:/.../providers/other_provider/config.json"
}
```

В `stdout` нужно выдавать JSON-события построчно:

```json
{"event":"progress","status":"generating","message":"Создание"}
{"event":"result","external_id":"id-провайдера","audio_path":"C:/.../job.wav"}
```

При ошибке:

```json
{"event":"error","message":"Описание ошибки"}
```

Для проверки доступности провайдера мост запускает EXE с тем же протоколом и
полем `operation`:

```json
{"protocol":1,"operation":"health","config_path":"C:/.../config.json"}
```

Доступный провайдер должен вернуть событие `result` с полем `healthy: true`.
Если сервис провайдера недоступен, верните `error` и завершите процесс с
ненулевым кодом.

Адаптер должен создать WAV по `output_path`. После этого мост сам проверит
файл, конвертирует его в MP3 встроенным ffmpeg, добавит метаданные и положит
результат в библиотеку.

Папки без корректного `provider.json` или без указанного EXE не попадают в
список доступных провайдеров.
