# Отчёт: генерация и скачивание трека YuE2 в локальном WanGP

Дата проверки: 30 сентября 2026 года.

## Найденная схема

Локальный WanGP запущен из:

```text
F:\games setup\StabilityMatrix-win-x64\Data\Packages\Wan2GP
```

Python-окружение:

```text
F:\games setup\StabilityMatrix-win-x64\Data\Packages\Wan2GP\venv\Scripts\python.exe
```

Gradio слушает только локальные адреса `127.0.0.1:7860` и `::1:7860`. Браузер для генерации не нужен: запросы можно отправлять из Python.

Основной Gradio endpoint:

```text
/ask_ai_with_ui_settings
```

Он принимает пользовательский текст и запускает Deepy. Внутренний инструмент Deepy находится в `shared/deepy/engine.py`, функция `gen_song()` (около строки 3584). Она ставит задачу в очередь WanGP и ждёт появления аудиофайла в галерее.

YuE2 backend находится в `models/TTS/yue2/yue2_handler.py`. Он создаёт WAV с частотой 48 кГц и двумя каналами.

## Важная особенность HybridService

При обычном режиме значение `default_song` передаётся через Gradio callback. В текущем запуске включён `HybridService`. В его ветке `shared/deepy/gradio_ui.py` вызывается `hybrid.submit(...)`, а остальные UI-параметры callback не применяются. Поэтому перед отправкой запроса нужно отдельно установить вариант генератора через Deepy settings API:

```text
GET  /deepy/deepy_api/settings
POST /deepy/deepy_api/settings
```

Тело POST:

```json
{
  "song_variant": "YuE2",
  "audio_duration": 10,
  "use_template_properties": true,
  "seed": -1
}
```

После POST нужно проверить, что ответ содержит:

```json
{
  "values": {
    "song_variant": "YuE2"
  }
}
```

Без этого шага запрос может попасть в сохранённый вариант ACE-Step, даже если в аргументах `/ask_ai_with_ui_settings` указано `default_song="YuE2"`.

## Запуск генерации

Вызов Gradio передаётся через `gradio_client.Client.submit()`. Успешное завершение Gradio Job означает только принятие запроса и возврат UI-update объектов. Это ещё не означает, что WAV готов.

Для одного запроса нужно передавать `separate_requests_with_empty_line=False`, иначе текст с пустыми строками разбивается на несколько Deepy-сообщений.

```python
from gradio_client import Client

client = Client("http://127.0.0.1:7860/")

request = (
    "Generate one song with the YuE2 backend. "
    "Lyrics: [Verse] Test song from local API. "
    "[Chorus] This is only a test. "
    "Music style: soft synth pop, female vocal, atmospheric. "
    "Use a maximum duration of 10 seconds and return the generated audio."
)

args = [
    [],                              # output_value
    -1,                             # last_choice_value
    "[]",                           # audio_files_paths_value
    -1,                             # audio_file_selected_value
    request,                         # ask_request
    "bridge_yue2_20260930_01",      # client_submission_id
    True,                            # auto_cancel_queue_tasks
    False,                           # separate_requests_with_empty_line
    True,                            # use_template_properties
    "fast",                         # model_speed
    "smaller",                      # model_size
    720, 1280, 81, 10, -1,           # height, width, frames, duration, seed
    "LTX-2 2.5 Distilled",
    "LTX-2.5 Distilled With Sound",
    "Krea 2 Turbo (8 Steps)",
    "Flux Klein 9B",
    "YuE2",                         # default_song (informational in HybridService)
    "MiniMax H3 Ref2VA Pruned Turbo Lightx2v 8 Steps",
    "Qwen3 1.7B",
    "Index TTS 2",
]

job = client.submit(*args, api_name="/ask_ai_with_ui_settings")
job.result()  # только подтверждает принятие запроса
```

Перед этим вызовом вариант YuE2 устанавливается обычным HTTP-запросом:

```python
import requests

base = "http://127.0.0.1:7860"
settings = requests.post(
    base + "/deepy/deepy_api/settings",
    json={
        "song_variant": "YuE2",
        "audio_duration": 10,
        "use_template_properties": True,
        "seed": -1,
    },
    timeout=30,
).json()

assert settings["values"]["song_variant"] == "YuE2"
```

## Ожидание готовности

Состояние Deepy доступно через:

```text
GET /deepy/deepy_api/state
```

События очереди доступны через:

```text
GET /deepy/deepy_api/events/poll?after=<cursor>
```

Алгоритм ожидания:

1. Получить `cursor` из `/deepy/deepy_api/state` до запуска или сохранить текущий cursor после запуска.
2. Периодически запрашивать `/deepy/deepy_api/events/poll?after=cursor`.
3. Показывать пользователю последние `progress.description`, например `Denoising | YuE2 acoustic synthesis`.
4. Считать генерацию успешной, когда в `state.gallery` появляется новый объект:

   ```json
   {
     "kind": "audio",
     "summary": {"model_type": "yue2"},
     "url": "deepy_api/media/file_<id>/file?...",
     "size": 1920834
   }
   ```

5. Дополнительно проверить `busy == false` и отсутствие события ошибки. Объект должен быть именно `kind == "audio"` и `summary.model_type == "yue2"`.

Пример ожидания:

```python
import time
import requests

def wait_for_yue2_audio(base, previous_ids, timeout=1800):
    deadline = time.time() + timeout
    while time.time() < deadline:
        state = requests.get(base + "/deepy/deepy_api/state", timeout=30).json()

        for item in state.get("gallery", []):
            if (
                item.get("kind") == "audio"
                and item.get("summary", {}).get("model_type") == "yue2"
                and item.get("id") not in previous_ids
            ):
                return item

        if not state.get("busy"):
            raise RuntimeError("WanGP завершил очередь, но новый YuE2 audio-файл не найден")

        time.sleep(2)

    raise TimeoutError("YuE2 generation timeout")
```

Нельзя использовать только `Job.done()` или `Job.result()` как признак готового файла: при HybridService они завершаются раньше внутренней генерации.

## Скачивание готового файла

`state.gallery[*].url` — относительный URL Deepy. Для скачивания нужно добавить префикс `/deepy/`:

```python
from urllib.parse import urljoin

def download_gallery_audio(base, item, destination):
    relative = item["url"].lstrip("/")
    url = urljoin(base + "/", "deepy/" + relative)
    with requests.get(url, stream=True, timeout=60) as response:
        response.raise_for_status()
        with open(destination, "wb") as output:
            for chunk in response.iter_content(chunk_size=1024 * 1024):
                if chunk:
                    output.write(chunk)
    return destination
```

Пример:

```python
item = wait_for_yue2_audio(base, previous_ids)
download_gallery_audio(
    base,
    item,
    r"C:\Users\zergi\Downloads\yue2_result.wav",
)
```

Также WanGP сохраняет результат в каталоге:

```text
F:\games setup\StabilityMatrix-win-x64\Data\Images\Img2Vid
```

В ходе проверки был создан файл:

```text
2026-09-30-22h37m29s_seed392304498_[Verse] Test song from local API. [Chorus] This is.wav
```

Его параметры: 48 кГц, stereo, 16-bit, 9.9987 секунды, 1 920 834 байта.

## Низкоуровневый вариант

Для отдельного Deepy-клиента существует:

```text
POST /deepy/deepy_api/messages
```

Тело:

```json
{
  "text": "Generate a song with YuE2 ...",
  "submission_id": "unique-request-id",
  "steering": false
}
```

Этот вариант также требует предварительного `POST /deepy/deepy_api/settings` и того же ожидания через `/deepy/deepy_api/state` или `/deepy/deepy_api/events/poll`.

## Итоговая рекомендация для интеграции

Использовать связку:

```text
POST /deepy/deepy_api/settings      -> song_variant = YuE2
POST /ask_ai_with_ui_settings       -> отправка запроса через Gradio
GET  /deepy/deepy_api/state         -> проверка busy/gallery
GET  /deepy/deepy_api/events/poll   -> прогресс и ошибки
GET  /deepy/deepy_api/media/.../file -> скачивание готового WAV
```

Проверка готовности должна основываться на новом `audio` в галерее с `summary.model_type == "yue2"`, а не на завершении Gradio Job.

Изменений в исходниках прошивки и исходниках WanGP для этого аудита не выполнялось.
