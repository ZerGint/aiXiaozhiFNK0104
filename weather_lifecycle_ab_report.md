# Controlled Weather lifecycle A/B на FNK0104S

Дата: 21.09.2026. Плата FNK0104S, ESP32-S3, ESP-IDF 6.1.0, firmware 2.4.2, COM13.

Эксперимент сравнивает production baseline `6255d42` и изолированную сборку с одной изменённой политикой Weather admission. Assets image, NVS, OTA, AFE, audio и UI не менялись. На устройство записывался только app по адресу `0x20000`; esptool стирал только диапазон `0x00020000..0x00393fff`.

## Политика

Control запускает первый Weather refresh после `SetNetworkConnected(true)` во время `activating`. Experiment сохраняет запрос pending и допускает HTTP/TLS только когда одновременно выполнены:

- сеть подключена и время синхронизировано;
- Application находится в `idle`;
- wake-word/audio processor успешно готовы;
- Internet Radio не активен;
- другой Weather request не выполняется.

Повторные и ручные запросы при unsafe state coalesce-ятся в один pending request и запускаются на ближайшем safe Tick. Проверка admission повторяется в начале Weather task перед созданием HTTP клиента. Ошибка создания task также оставляет запрос pending.

## Runtime captures

| Событие | Control | Experiment |
|---|---:|---:|
| `AFTER_WIFI` | 8.588 s | 8.618 s |
| `WEATHER_HTTP_START` | 8.848 s | 14.038 s |
| `PROTOCOL_CONNECTED` | 13.548 s | 13.658 s |
| `State: activating -> idle` | 13.558 s | 13.668 s |
| AFE initialized | 13.708 s | 13.828 s |
| audio input enabled | 13.728 s | 13.848 s |
| Weather HTTP 200 | 10.378 s | 16.348 s |
| `WEATHER_UPDATE_OK` | 10.828 s | 16.788 s |

В control HTTP/TLS полностью пересекается с activation/protocol и выполняется до AFE. В experiment HTTP начинается примерно через 190 ms после включения input, уже после `idle`, AFE и protocol. Обе сессии получили HTTP 200, корректно распарсили forecast и дошли до стабильного idle без panic/assert/watchdog/stack-canary маркеров.

## Memory observations

Control: перед Weather `WEATHER_MEM` largest block равен 59,392 B; после HTTP/cleanup — 45,056 B. После AFE и audio stable idle largest составлял 28,672 B.

Experiment: до отложенного Weather после AFE/input largest block равен 24,576 B; после HTTP/cleanup остаётся 24,576 B. Stable idle удерживает около 31–32 KiB в обычных RAM samples. Эти абсолютные значения зависят от cold-boot allocator topology; A/B вывод основан прежде всего на порядке событий, а не на сравнении разных стартовых heap snapshots.

## Artifacts

- [control capture](weather_lifecycle_control_2min.txt), SHA-256 `4747D14C6DE063A6C257ECCFF50D276ABC9866C37D1366639895833E3A7D431B`;
- [experiment capture](weather_lifecycle_experiment_final_2min.txt), SHA-256 `6DBB4E13638E474198DC4E453406FD337A76732648BB8A991B075BE110EE0D16`;
- [final production smoke capture](weather_lifecycle_final_smoke.txt), SHA-256 `70232E0324CA2F2CB6D8409137EF42DDC95815CA895B1E48BB78A88EEDD4634C`;
- final experiment `build/xiaozhi.bin`, 3,619,760 B, SHA-256 `476FF011EAAC03A17B7F27D15D7D3A97EA1DD0E7D20DA7D811FB8424E573BF1E`;
- identical `build/generated_assets.bin`, 1,955,326 B, SHA-256 `6A21B322E49370C0072301EC471960289802B368388EC1ABF10BCC2F93013722`.

Вывод: изменение изолирует первый Weather HTTP/TLS от критического boot/AFE окна и сохраняет pending semantics для unsafe состояний. AFE/ESP-SR allocations и их fragmentation не изменялись.
