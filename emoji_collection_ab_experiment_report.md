# A/B эксперимент runtime `EmojiCollection` на FNK0104S

Дата: 20.09.2026. Плата FNK0104S, ESP32-S3, ESP-IDF 6.1.0, firmware 2.4.2, COM13.

Эксперимент проверял одну переменную: создание runtime стандартной `EmojiCollection` в `Assets::LvglStrategy::Apply()`. В control флаг `CONFIG_FNK_EMOJI_COLLECTION_AB_TEST=n`; в experiment — `y`. В experiment изменён только этот участок: `index.json`, assets partition, порядок остальных выделений, Weather, AFE, audio и UI не менялись. Реализация находится в [main/Kconfig.projbuild](/F:/FNK0104AI/aiXiaozhiFNK0104/main/Kconfig.projbuild:21) и [main/assets.cc](/F:/FNK0104AI/aiXiaozhiFNK0104/main/assets.cc:336).

## Сборки и прошивка

Обе сборки прошли успешно; app binary занял 12% свободного места partition:

| Arm | Флаг | `xiaozhi.bin` | SHA-256 |
|---|---|---:|---|
| Control | `n` | 3,628,848 bytes | `E04752C5877AA612657E47230019484896632B60289139A0049C83D28FC9B6E9` |
| Experiment | `y` | 3,626,320 bytes | `9231576A0E3D8A3C03E1E1F822887DB60126D04DE19D1BD79D150834F0A3DF96` |

Assets image в обоих build-каталогах идентичен: 1,955,326 bytes, SHA-256 `6A21B322E49370C0072301EC471960289802B368388EC1ABF10BCC2F93013722`. Это подтверждает, что PNG и `index.json` не удалялись и не менялись.

На устройство записывался только app по адресу `0x20000`. Esptool в каждой операции указал erase range `0x00020000..0x00395fff`; NVS, bootloader, partition table и assets partition не стирались и не записывались. После измерений восстановлен control app той же app-only операцией.

## Результаты внутри каждого boot

Значения `internal_free/internal_largest` и `dma_free/dma_largest` взяты из `DMA_TRACE`; для topology — из `HEAPMAP` с caps `INTERNAL|DMA`.

| Checkpoint | Control: internal free / largest | Control: DMA free / largest | Experiment: internal free / largest | Experiment: DMA free / largest |
|---|---:|---:|---:|---:|
| Assets enter | 105,143 / 63,488 | 97,371 / 63,488 | 117,343 / 73,728 | 109,571 / 73,728 |
| Before emoji setup | 98,203 / 57,344 | 90,431 / 57,344 | 110,039 / 69,632 | 102,267 / 69,632 |
| After emoji setup | 96,179 / 40,960 | 88,407 / 40,960 | 110,003 / 69,632 | 102,231 / 69,632 |
| Assets exit | 99,327 / 40,960 | 91,555 / 40,960 | 114,931 / 69,632 | 107,159 / 69,632 |
| AFE persistent ready | 75,459 / 31,744 | 67,687 / 31,744 | 77,139 / 32,768 | 69,367 / 32,768 |
| I2S RX after enable | 70,795 / 27,648 | 63,095 / 27,648 | 72,891 / 32,768 | 65,119 / 32,768 |
| Stable idle, final RAM sample | 72,823 / 27,648 | — | 74,687 / 32,768 | — |

The direct within-boot effect is clear:

- Control: emoji setup changes free by about `-2,024` bytes and largest block by `-16,384` bytes (`57,344 -> 40,960`). The setup creates `EmojiCollection`, 21 `LvglRawImage` wrappers and map entries; the log records `ASSETS_ALLOC owner=emoji_collection count=21`.
- Experiment: the same checkpoints differ by only `-36` bytes free and `0` bytes largest (`69,632 -> 69,632`). The log records `EMOJI_AB_TEST: skipping runtime standard EmojiCollection; assets index unchanged` and no `emoji_collection`/`emoji_image` allocations.

В control Weather HTTP completion был interleaved с хвостом Assets (`WEATHER_HTTP_RESULT` около 10.8 s, `ASSETS_AFTER_EMOJI_COLLECTION` сразу после него), поэтому разность free bytes между двумя поздними checkpoints не приписывается только emoji. Сам largest block уже последовательно падал внутри emoji loop (`57,344 -> 55,296 -> 53,248 -> 40,960`); в experiment аналогичный loop отсутствует и largest остаётся 69,632.

Cross-arm absolute values before Assets are not treated as a single deterministic baseline: allocator state and asynchronous network timing differ between cold boots. The stronger evidence is the paired before/after checkpoint in each boot and the identical assets image.

Heap maps show the topology consequence at the end of Assets:

| Heap map | Control | Experiment |
|---|---:|---:|
| `HEAPMAP_ASSETS_ENTER` free / largest / free blocks | 97,371 / 63,488 / 6 | 109,535 / 73,728 / 27 |
| `HEAPMAP_ASSETS_EXIT` free / largest / free blocks | 91,555 / 40,960 / 17 | 107,159 / 69,632 / 29 |

The experiment therefore retains roughly 15.6 KiB more free INTERNAL/DMA memory and 28 KiB more largest contiguous block at Assets exit in these captures. It still has more free blocks, so this is not a claim that every fragmentation metric improves; the decisive difference is removal of the persistent wrapper/map allocation sequence.

## Runtime validation

Both 90-second captures reached Wi-Fi, Weather HTTP 200, MQTT `PROTOCOL_CONNECTED`, `State: activating -> idle`, AFE initialization, and I2S input enable. The experiment remained in stable idle for the remainder of the capture with `internal_free` around 74.4–74.7 KiB and largest block 32,768 bytes. No panic, assert, watchdog, allocation failure or stack-canary marker appeared. This confirms that the FNK0104S RoboEyes/audio path boots and remains alive without runtime standard emoji wrappers in this test.

Logs:

- [Control capture](/F:/FNK0104AI/aiXiaozhiFNK0104/emoji_ab_control_boot.txt)
- [Experiment capture](/F:/FNK0104AI/aiXiaozhiFNK0104/emoji_ab_experiment_boot.txt)

## Conclusion

For this controlled FNK0104S A/B, standard runtime `EmojiCollection` is a direct cause of a persistent INTERNAL/DMA allocation and fragmentation step. Keeping the assets unchanged but skipping only the collection construction removes the approximately 16 KiB largest-block drop at the emoji checkpoint and preserves a materially larger block through Assets, AFE, audio enable and stable idle. The device completed the normal boot/network/audio lifecycle in the experiment arm. This result supports a later production decision about removing or making the collection conditional, but no production optimization was applied by this experiment.
