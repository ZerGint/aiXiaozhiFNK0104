#include "audio_service.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include "application.h"
#include "audio_manager.h"
#include "media/internet_radio_player.h"
#include "media/sd_music_player.h"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                     \
    (esp_opus_dec_cfg_t) {                                                                 \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,                \
        .frame_duration =                                                                  \
            (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false,                                                           \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

uint64_t AudioService::AssignAudioTaskId(AudioTask* task) {
    if (task == nullptr) {
        return 0;
    }
    uint64_t id = next_audio_task_id_.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) {
        id = next_audio_task_id_.fetch_add(1, std::memory_order_relaxed);
    }
    task->lifecycle_id = id;
    task->lifecycle_magic = AudioTask::kLifecycleMagic;
    return id;
}

bool AudioService::RegisterAudioTaskLocked(const AudioTask* task) {
    if (task == nullptr || task->lifecycle_id == 0) {
        return false;
    }
    for (const auto& entry : audio_task_registry_) {
        if (entry.pointer == task) {
            return true;
        }
    }
    for (auto& entry : audio_task_registry_) {
        if (entry.pointer == nullptr) {
            entry.pointer = task;
            entry.task_id = task->lifecycle_id;
            return true;
        }
    }
    ESP_LOGE(TAG, "AUDIO_TASK_REGISTRY_FULL task=%p task_id=%llu", task,
             static_cast<unsigned long long>(task->lifecycle_id));
    return false;
}

void AudioService::UnregisterAudioTaskLocked(const AudioTask* task) {
    if (task == nullptr) {
        return;
    }
    for (auto& entry : audio_task_registry_) {
        if (entry.pointer == task) {
            entry.pointer = nullptr;
            entry.task_id = 0;
            return;
        }
    }
}

bool AudioService::IsRegisteredAudioTaskLocked(const AudioTask* task) const {
    if (task == nullptr) {
        return false;
    }
    for (const auto& entry : audio_task_registry_) {
        if (entry.pointer == task) {
            return entry.task_id == task->lifecycle_id &&
                   task->lifecycle_magic == AudioTask::kLifecycleMagic;
        }
    }
    return false;
}

void AudioService::TraceAudioTask(const char* event, const AudioTask* task, size_t queue_size,
                                  bool object_valid) const {
    if (!event) {
        return;
    }
    const char* task_name = pcTaskGetName(nullptr);
    if (task_name == nullptr) {
        task_name = "?";
    }
    if (!object_valid || task == nullptr) {
        // This branch deliberately prints only the pointer value. It is used
        // after the queue invariant has failed, so dereferencing task here
        // could turn useful evidence into a second panic.
        ESP_LOGD(TAG,
                 "AUDIO_TASK_%s task_id=0 task=%p source=UNKNOWN generation=%lu task_name=%s "
                 "queue=%u valid=0",
                 event, task, 0UL, task_name, static_cast<unsigned>(queue_size));
        return;
    }
    const char* source = task->is_radio ? "RADIO" : task->is_music ? "SD" : "TTS";
    ESP_LOGD(TAG,
             "AUDIO_TASK_%s task_id=%llu task=%p source=%s generation=%lu task_name=%s queue=%u "
             "valid=1",
             event, static_cast<unsigned long long>(task->lifecycle_id), task, source,
             static_cast<unsigned long>(task->lifecycle_generation), task_name,
             static_cast<unsigned>(queue_size));
}

void AudioService::CheckHeapIntegrity(const char* checkpoint) {
    const bool ok = heap_caps_check_integrity_all(true);
    ESP_LOGW(TAG, "HEAP_CHECK %s: %s", checkpoint ? checkpoint : "unknown", ok ? "PASS" : "FAIL");
}

size_t AudioService::QuarantineCorruptPlaybackTasksLocked(const char* checkpoint) {
    size_t removed = 0;
    size_t index = 0;
    for (auto it = audio_playback_queue_.begin(); it != audio_playback_queue_.end();) {
        const AudioTask* task = it->get();
        const bool valid = IsRegisteredAudioTaskLocked(task);
        if (!valid) {
            ESP_LOGE(TAG,
                     "AUDIO_QUEUE_CORRUPTION checkpoint=%s queue=playback index=%u raw=%p "
                     "size=%u",
                     checkpoint ? checkpoint : "unknown", static_cast<unsigned>(index), task,
                     static_cast<unsigned>(audio_playback_queue_.size()));
            TraceAudioTask("DISCARD", task, audio_playback_queue_.size(), false);
            // Do not invoke delete on a pointer whose value or object header is
            // already corrupt. Releasing it makes the invariant violation
            // explicit while avoiding a second fault in unique_ptr::~unique_ptr.
            it->release();
            it = audio_playback_queue_.erase(it);
            ++removed;
        } else {
            ++it;
        }
        ++index;
    }
    if (removed > 0) {
        CheckHeapIntegrity(checkpoint ? checkpoint : "queue_corruption");
    }
    return removed;
}

size_t AudioService::QuarantineCorruptEncodeTasksLocked(const char* checkpoint) {
    size_t removed = 0;
    size_t index = 0;
    for (auto it = audio_encode_queue_.begin(); it != audio_encode_queue_.end();) {
        const AudioTask* task = it->get();
        if (!IsRegisteredAudioTaskLocked(task)) {
            ESP_LOGE(TAG,
                     "AUDIO_QUEUE_CORRUPTION checkpoint=%s queue=encode index=%u raw=%p "
                     "size=%u",
                     checkpoint ? checkpoint : "unknown", static_cast<unsigned>(index), task,
                     static_cast<unsigned>(audio_encode_queue_.size()));
            TraceAudioTask("DISCARD", task, audio_encode_queue_.size(), false);
            it->release();
            it = audio_encode_queue_.erase(it);
            ++removed;
        } else {
            ++it;
        }
        ++index;
    }
    if (removed > 0) {
        CheckHeapIntegrity(checkpoint ? checkpoint : "encode_queue_corruption");
    }
    return removed;
}

void AudioService::DiscardPlaybackTasksLocked(bool radio_only, bool music_only,
                                              const char* reason) {
    QuarantineCorruptPlaybackTasksLocked(reason);
    for (auto it = audio_playback_queue_.begin(); it != audio_playback_queue_.end();) {
        const AudioTask* task = it->get();
        const bool matches = (!radio_only && !music_only) ||
                             (radio_only && task->is_radio) ||
                             (music_only && task->is_music);
        if (!matches) {
            ++it;
            continue;
        }
        TraceAudioTask("DISCARD", task, audio_playback_queue_.size());
        TraceAudioTask("DESTROY", task, audio_playback_queue_.size());
        UnregisterAudioTaskLocked(task);
        it = audio_playback_queue_.erase(it);
    }
}

void AudioService::DiscardAllEncodeTasksLocked(const char* reason) {
    (void)reason;
    for (auto it = audio_encode_queue_.begin(); it != audio_encode_queue_.end();) {
        const AudioTask* task = it->get();
        if (task != nullptr && IsRegisteredAudioTaskLocked(task)) {
            TraceAudioTask("DISCARD", task, audio_encode_queue_.size());
            TraceAudioTask("DESTROY", task, audio_encode_queue_.size());
            UnregisterAudioTaskLocked(task);
            it = audio_encode_queue_.erase(it);
        } else {
            ESP_LOGE(TAG, "AUDIO_QUEUE_CORRUPTION queue=encode raw=%p size=%u", task,
                     static_cast<unsigned>(audio_encode_queue_.size()));
            it->release();
            it = audio_encode_queue_.erase(it);
        }
    }
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_.store(false);
    ai_speech_level_.store(0, std::memory_order_relaxed);
    ai_speech_level_updated_ms_.store(0, std::memory_order_release);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                           AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                                           AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    ESP_LOGI(TAG,
             "AUDIO_JITTER_CONFIG playback_capacity=%u frames prebuffer=%u frames max_wait=%u ms "
             "decoded_pcm=PSRAM",
             static_cast<unsigned>(MAX_PLAYBACK_TASKS_IN_QUEUE),
             static_cast<unsigned>(SPEECH_PREBUFFER_TASKS),
             static_cast<unsigned>(SPEECH_PREBUFFER_MAX_MS));

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreateWithCaps(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->OpusCodecTask();
            vTaskDeleteWithCaps(NULL);
        },
        "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void AudioService::Stop() {
    CheckHeapIntegrity("audio_stop_begin");
    esp_timer_stop(audio_power_timer_);
    service_stopped_.store(true);
    ai_speech_level_.store(0, std::memory_order_relaxed);
    ai_speech_level_updated_ms_.store(0, std::memory_order_release);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                         AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        ++media_generation_;
        DiscardAllEncodeTasksLocked("audio_stop");
        audio_decode_queue_.clear();
        DiscardPlaybackTasksLocked(false, false, "audio_stop");
        speech_prebuffering_ = false;
        speech_prebuffer_started_us_ = 0;
        radio_buffered_duration_ms_ = 0;
        radio_prebuffer_ready_generation_ = 0;
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
    CheckHeapIntegrity("audio_stop_end");
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            const int input_channels = codec_->input_channels();
            if (input_channels <= 0 || input_channels > 2) {
                ESP_LOGE(TAG, "Invalid codec input channel count: %d", input_channels);
                return false;
            }
            uint32_t in_sample_num = data.size() / input_channels;
            uint32_t output_samples = 0;
            const esp_ae_err_t size_result = esp_ae_rate_cvt_get_max_out_sample_num(
                input_resampler_, in_sample_num, &output_samples);
            if (size_result != ESP_AE_ERR_OK || output_samples == 0 ||
                output_samples > MAX_AUDIO_TASK_PCM_SAMPLES) {
                ESP_LOGE(TAG, "Invalid input resampler capacity: ret=%d samples=%lu",
                         static_cast<int>(size_result), static_cast<unsigned long>(output_samples));
                return false;
            }
            auto resampled = std::vector<int16_t>(output_samples * input_channels);
            uint32_t actual_output = output_samples;
            const esp_ae_err_t process_result = esp_ae_rate_cvt_process(
                input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                (esp_ae_sample_t)resampled.data(), &actual_output);
            if (process_result != ESP_AE_ERR_OK || actual_output > output_samples) {
                ESP_LOGE(TAG, "Invalid input resampler output: ret=%d samples=%lu capacity=%lu",
                         static_cast<int>(process_result), static_cast<unsigned long>(actual_output),
                         static_cast<unsigned long>(output_samples));
                return false;
            }
            resampled.resize(actual_output * input_channels);
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
                                                  AS_EVENT_WAKE_WORD_RUNNING |
                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >=
                AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;  // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    bool radio_started = false;
    bool speech_starved = false;
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        bool woke = true;
        if (speech_diagnostics_active_.load(std::memory_order_acquire)) {
            woke = audio_queue_cv_.wait_for(
                lock, std::chrono::milliseconds(100),
                [this]() { return !audio_playback_queue_.empty() || service_stopped_.load(); });
        } else {
            audio_queue_cv_.wait(
                lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_.load(); });
        }
        if (service_stopped_.load()) {
            break;
        }
        QuarantineCorruptPlaybackTasksLocked("output_loop");
        if (!woke && audio_playback_queue_.empty() && !decode_in_flight_ &&
            Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
            if (!speech_starved) {
                ++playback_underrun_count_;
                speech_starved = true;
            }
        }
        if (!audio_playback_queue_.empty()) {
            speech_starved = false;
        }

        const auto device_state = Application::GetInstance().GetDeviceState();
        if (speech_prebuffering_ && device_state == kDeviceStateSpeaking &&
            !audio_playback_queue_.empty()) {
            const int64_t elapsed_ms =
                (esp_timer_get_time() - speech_prebuffer_started_us_) / 1000;
            if (audio_playback_queue_.size() < SPEECH_PREBUFFER_TASKS &&
                elapsed_ms < SPEECH_PREBUFFER_MAX_MS) {
                audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return service_stopped_.load() || !speech_prebuffering_ ||
                           audio_playback_queue_.size() >= SPEECH_PREBUFFER_TASKS;
                });
                continue;
            }
            speech_prebuffering_ = false;
            speech_prebuffer_started_us_ = 0;
        } else if (speech_prebuffering_) {
            speech_prebuffering_ = false;
            speech_prebuffer_started_us_ = 0;
        }
        if (IsVoiceAudioState(device_state)) {
            DiscardPlaybackTasksLocked(false, true, "voice_state_cleanup");
            radio_buffered_duration_ms_ = 0;
            radio_prebuffer_ready_generation_ = 0;
            if (audio_playback_queue_.empty()) {
                radio_started = false;
                audio_queue_cv_.notify_all();
                continue;
            }
        }
        if (audio_playback_queue_.empty()) {
            radio_started = false;
            audio_queue_cv_.notify_all();
            continue;
        }

        bool has_radio = false;
        for (const auto& queued_task : audio_playback_queue_) {
            has_radio = has_radio || queued_task->is_radio;
        }
        if (!has_radio && !output_in_flight_) {
            radio_started = false;
            last_radio_write_end_us_ = 0;
            last_radio_duration_ms_ = 0;
        }

        if (!radio_started && RADIO_PREBUFFER_MS > 0 && !audio_playback_queue_.empty() &&
            audio_playback_queue_.front()->is_radio) {
            uint32_t radio_duration_ms = 0;
            for (const auto& queued_task : audio_playback_queue_) {
                if (queued_task->is_radio) {
                    radio_duration_ms += queued_task->duration_ms;
                }
            }
            if (has_radio && radio_duration_ms < RADIO_PREBUFFER_MS) {
                if (radio_started) {
                    InternetRadioPlayer::GetInstance().RecordUnderrun();
                }
                static int64_t last_prebuffer_log_us = 0;
                uint64_t now_prebuffer_us = esp_timer_get_time();
                if (now_prebuffer_us - last_prebuffer_log_us >= 1000000) {
                    last_prebuffer_log_us = now_prebuffer_us;
                    ESP_LOGI(
                        TAG,
                        "[RADIO_PREBUFFER] buffering... current=%u ms target=%u ms (queue=%zu)",
                        (unsigned)radio_duration_ms, (unsigned)RADIO_PREBUFFER_MS,
                        audio_playback_queue_.size());
                }
                audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(50));
                continue;
            }
            if (has_radio) {
                radio_started = true;
                last_radio_write_end_us_ = 0;
            }
        }
        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        const size_t queue_size_after_dequeue = audio_playback_queue_.size();
        if (!task || !IsRegisteredAudioTaskLocked(task.get())) {
            const AudioTask* corrupt_task = task.get();
            ESP_LOGE(TAG,
                     "AUDIO_QUEUE_CORRUPTION checkpoint=output_dequeue queue=playback raw=%p",
                     corrupt_task);
            TraceAudioTask("DISCARD", corrupt_task, queue_size_after_dequeue, false);
            task.release();
            output_in_flight_ = false;
            media_output_in_flight_ = false;
            audio_queue_cv_.notify_all();
            continue;
        }
        TraceAudioTask("DEQUEUE", task.get(), queue_size_after_dequeue);
        speech_starved = false;
        if (task->is_radio) {
            radio_buffered_duration_ms_ = radio_buffered_duration_ms_ > task->duration_ms
                                              ? radio_buffered_duration_ms_ - task->duration_ms
                                              : 0;
        }
        output_in_flight_ = true;
        media_output_in_flight_ = task->is_music;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (task->is_music &&
            (IsVoiceAudioState(device_state) ||
             (AudioManager::GetInstance().GetCurrentAudioSource() != kAudioSourceMp3Player &&
              AudioManager::GetInstance().GetCurrentAudioSource() != kAudioSourceInternetRadio))) {
            const bool task_is_radio = task->is_radio;
            lock.lock();
            TraceAudioTask("DISCARD", task.get(), audio_playback_queue_.size());
            TraceAudioTask("DESTROY", task.get(), audio_playback_queue_.size());
            UnregisterAudioTaskLocked(task.get());
            output_in_flight_ = false;
            media_output_in_flight_ = false;
            if (task_is_radio) {
                radio_started = false;
            }
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            task.reset();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            continue;
        }

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        TraceAudioTask("OUTPUT_BEGIN", task.get(), queue_size_after_dequeue);
        if (task->stream_start) {
            ESP_LOGI(TAG,
                     "AUDIO_TASK_OUTPUT_BEGIN source=%s task=%p pcm=%p samples=%u",
                     task->is_radio ? "radio" : "sd", task.get(), task->GetPcmData(),
                     static_cast<unsigned>(task->GetPcmSize()));
        }

        if (task->playback_id != 0 && callbacks_.on_playback_progress) {
            callbacks_.on_playback_progress(task->playback_id, task->media_position_ms);
        }

        codec_->SetOutputSource(task->output_source, task->stream_start);

        if (task->output_source == AudioOutputSource::kAiSpeech) {
            uint64_t sum_abs = 0;
            const int16_t* pcm = task->GetPcmData();
            const size_t sample_count = task->GetPcmSize();
            for (size_t i = 0; i < sample_count; ++i) {
                const int32_t sample = pcm[i];
                sum_abs += static_cast<uint32_t>(sample < 0 ? -sample : sample);
            }
            const uint32_t mean_abs = sample_count > 0
                                           ? static_cast<uint32_t>(sum_abs / sample_count)
                                           : 0;
            const uint32_t level = std::min<uint32_t>(
                100, (mean_abs * 160U + 16383U) / 32768U);
            ai_speech_level_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
            ai_speech_level_updated_ms_.store(
                static_cast<uint32_t>(esp_timer_get_time() / 1000), std::memory_order_release);
        } else {
            ai_speech_level_.store(0, std::memory_order_relaxed);
        }

        if (task->is_radio) {
            uint64_t now_us = esp_timer_get_time();
            uint32_t now_ms = static_cast<uint32_t>(now_us / 1000);
            uint32_t wait_in_q_ms =
                now_ms >= task->created_at_ms ? (now_ms - task->created_at_ms) : 0;
            uint32_t gap_ms =
                last_radio_write_end_us_ > 0
                    ? static_cast<uint32_t>((now_us - last_radio_write_end_us_) / 1000)
                    : 0;

            if (gap_ms > last_radio_duration_ms_ + 15 && last_radio_write_end_us_ > 0) {
                size_t playback_queue_size = 0;
                {
                    std::lock_guard<std::mutex> queue_lock(audio_queue_mutex_);
                    playback_queue_size = audio_playback_queue_.size();
                }
                ESP_LOGW(TAG,
                         "[RADIO_OUTPUT_GAP] gap=%u ms expected_duration=%u ms queue=%zu buffer=%u "
                         "ms wait_in_q=%u ms",
                         (unsigned)gap_ms, (unsigned)last_radio_duration_ms_,
                         playback_queue_size, (unsigned)radio_buffered_duration_ms_,
                         (unsigned)wait_in_q_ms);
            }

            uint64_t write_start_us = esp_timer_get_time();
            codec_->OutputData(task->GetPcmData(), task->GetPcmSize());
            uint64_t write_end_us = esp_timer_get_time();

            uint32_t write_dur_ms = static_cast<uint32_t>((write_end_us - write_start_us) / 1000);
            last_radio_write_end_us_ = write_end_us;
            last_radio_duration_ms_ = task->duration_ms;

            static int64_t last_output_log_us = 0;
            if (now_us - last_output_log_us >= 500000) {
                last_output_log_us = now_us;
                size_t playback_queue_size = 0;
                {
                    std::lock_guard<std::mutex> queue_lock(audio_queue_mutex_);
                    playback_queue_size = audio_playback_queue_.size();
                }
                ESP_LOGI(
                    TAG,
                    "[RADIO_OUTPUT] queue=%zu buffer=%u ms pcm=%u ms write=%u ms wait_in_q=%u ms",
                    playback_queue_size, (unsigned)radio_buffered_duration_ms_,
                    (unsigned)task->duration_ms, (unsigned)write_dur_ms, (unsigned)wait_in_q_ms);
            }
        } else {
            codec_->OutputData(task->GetPcmData(), task->GetPcmSize());
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        const bool task_is_radio = task->is_radio;
        const bool task_stream_start = task->stream_start;
        const void* task_ptr = task.get();
        const void* task_pcm_ptr = task->GetPcmData();
        const size_t task_pcm_size = task->GetPcmSize();
#if CONFIG_USE_SERVER_AEC
        const uint32_t task_timestamp = task->timestamp;
#endif
        bool notify_drained = false;
        lock.lock();
        TraceAudioTask("OUTPUT_END", task.get(), audio_playback_queue_.size());
        TraceAudioTask("DESTROY", task.get(), audio_playback_queue_.size());
        UnregisterAudioTaskLocked(task.get());
        output_in_flight_ = false;
        media_output_in_flight_ = false;
        if (task_is_radio) {
            bool has_radio = false;
            for (const auto& queued_task : audio_playback_queue_) {
                has_radio = has_radio || queued_task->is_radio;
            }
            if (!has_radio) {
                radio_started = false;
            }
        }
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp while the queue mutex still protects the history. */
        if (task_timestamp > 0) {
            timestamp_queue_.push_back(task_timestamp);
        }
#endif
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();
        task.reset();

        if (task_stream_start) {
            ESP_LOGI(TAG,
                     "AUDIO_TASK_DESTROY source=%s task=%p pcm=%p samples=%u",
                     task_is_radio ? "radio" : "sd", task_ptr, task_pcm_ptr,
                     static_cast<unsigned>(task_pcm_size));
        }
        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_encode_queue_.empty() ||
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

        QuarantineCorruptEncodeTasksLocked("codec_loop");
        QuarantineCorruptPlaybackTasksLocked("codec_loop");

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->pcm_in_psram = true;
            task->lifecycle_generation = generation;
            task->timestamp = packet->timestamp;
            task->playback_id = packet->playback_id;
            task->media_position_ms = packet->media_position_ms;
            task->output_source =
                packet->is_system_sound ? AudioOutputSource::kSystem : AudioOutputSource::kAiSpeech;
            AssignAudioTaskId(task.get());

            {
                std::lock_guard<std::mutex> registry_lock(audio_queue_mutex_);
                if (!RegisterAudioTaskLocked(task.get())) {
                    ESP_LOGE(TAG, "AUDIO_TASK_DISCARD task_id=%llu reason=registry_full",
                             static_cast<unsigned long long>(task->lifecycle_id));
                    TraceAudioTask("DESTROY", task.get(), 0);
                    decode_in_flight_ = false;
                    audio_queue_cv_.notify_all();
                    continue;
                }
                TraceAudioTask("CREATE", task.get(), 0);
            }

            const bool decode_configured =
                SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (decode_configured && opus_decoder_ != nullptr) {
                task->playback_pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t*)(task->playback_pcm.data()),
                    .len = (uint32_t)(task->playback_pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0 &&
                    out_frame.decoded_size <= out_frame.len &&
                    (out_frame.decoded_size % sizeof(int16_t)) == 0) {
                    task->playback_pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    bool pcm_valid = true;
                    if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                        output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        const auto size_ret = esp_ae_rate_cvt_get_max_out_sample_num(
                            output_resampler_, task->playback_pcm.size(), &target_size);
                        if (size_ret != ESP_AE_ERR_OK || target_size == 0 ||
                            target_size > MAX_AUDIO_TASK_PCM_SAMPLES) {
                            ESP_LOGE(TAG,
                                     "AUDIO_DECODE_INVALID_RESAMPLER_SIZE task_id=%llu ret=%d "
                                     "target=%lu",
                                     static_cast<unsigned long long>(task->lifecycle_id),
                                     static_cast<int>(size_ret),
                                     static_cast<unsigned long>(target_size));
                            pcm_valid = false;
                        } else {
                            PsramVector resampled(target_size);
                            uint32_t actual_output = target_size;
                            const auto process_ret = esp_ae_rate_cvt_process(
                                output_resampler_, (esp_ae_sample_t)task->playback_pcm.data(),
                                task->playback_pcm.size(), (esp_ae_sample_t)resampled.data(),
                                &actual_output);
                            if (process_ret != ESP_AE_ERR_OK || actual_output > target_size) {
                                ESP_LOGE(TAG,
                                         "AUDIO_DECODE_INVALID_RESAMPLER_OUTPUT task_id=%llu "
                                         "ret=%d actual=%lu capacity=%lu",
                                         static_cast<unsigned long long>(task->lifecycle_id),
                                         static_cast<int>(process_ret),
                                         static_cast<unsigned long>(actual_output),
                                         static_cast<unsigned long>(target_size));
                                pcm_valid = false;
                            } else {
                                resampled.resize(actual_output);
                                task->playback_pcm = std::move(resampled);
                            }
                        }
                    } else if (decoder_sample_rate_ != codec_->output_sample_rate()) {
                        ESP_LOGE(TAG, "AUDIO_DECODE_MISSING_RESAMPLER rate=%d target=%d",
                                 decoder_sample_rate_, codec_->output_sample_rate());
                        pcm_valid = false;
                    }
                    if (pcm_valid && task->playback_pcm.size() <= MAX_AUDIO_TASK_PCM_SAMPLES) {
                        decoded = true;
                    }
                } else {
                    ESP_LOGE(TAG,
                             "AUDIO_DECODE_INVALID_OUTPUT task_id=%llu ret=%d decoded=%lu "
                             "capacity=%lu",
                             static_cast<unsigned long long>(task->lifecycle_id), static_cast<int>(ret),
                             static_cast<unsigned long>(out_frame.decoded_size),
                             static_cast<unsigned long>(out_frame.len));
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured or packet parameters are invalid");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                if (task->pcm_in_psram && task->output_source == AudioOutputSource::kAiSpeech &&
                    audio_playback_queue_.empty() && !output_in_flight_ &&
                    speech_diagnostics_active_.load(std::memory_order_acquire)) {
                    speech_prebuffering_ = true;
                    speech_prebuffer_started_us_ = esp_timer_get_time();
                }
                TraceAudioTask("ENQUEUE", task.get(), audio_playback_queue_.size() + 1);
                audio_playback_queue_.push_back(std::move(task));
            } else if (task) {
                TraceAudioTask("DISCARD", task.get(), audio_playback_queue_.size());
                TraceAudioTask("DESTROY", task.get(), audio_playback_queue_.size());
                UnregisterAudioTaskLocked(task.get());
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            if (!task || !IsRegisteredAudioTaskLocked(task.get())) {
                const AudioTask* corrupt_task = task.get();
                ESP_LOGE(TAG,
                         "AUDIO_QUEUE_CORRUPTION checkpoint=codec_dequeue queue=encode raw=%p",
                         corrupt_task);
                TraceAudioTask("DISCARD", corrupt_task, audio_encode_queue_.size(), false);
                task.release();
                audio_queue_cv_.notify_all();
                lock.unlock();
                continue;
            }
            TraceAudioTask("DEQUEUE", task.get(), audio_encode_queue_.size());
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* Never let a full send queue stall encoding: stale realtime
                             * audio is useless to the server, so drop the oldest packet. */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG,
                         "Failed to encode audio: encoder not configured or invalid frame size "
                         "(got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
            if (task) {
                TraceAudioTask("OUTPUT_END", task.get(), audio_encode_queue_.size());
                TraceAudioTask("DESTROY", task.get(), audio_encode_queue_.size());
                UnregisterAudioTaskLocked(task.get());
                task.reset();
            }
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

bool AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    const bool valid_rate = sample_rate >= 8000 && sample_rate <= 48000;
    const bool valid_duration = frame_duration == 5 || frame_duration == 10 ||
                                frame_duration == 20 || frame_duration == 40 ||
                                frame_duration == 60 || frame_duration == 80 ||
                                frame_duration == 100 || frame_duration == 120;
    if (!valid_rate || !valid_duration) {
        ESP_LOGE(TAG, "AUDIO_DECODE_INVALID_CONFIG rate=%d duration=%d", sample_rate,
                 frame_duration);
        return false;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return opus_decoder_ != nullptr && decoder_frame_size_ > 0 &&
               decoder_frame_size_ <= MAX_AUDIO_TASK_PCM_SAMPLES;
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (ret != ESP_AUDIO_ERR_OK || opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_close(opus_decoder_);
            opus_decoder_ = nullptr;
        }
        return false;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_,
                 codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
            esp_opus_dec_close(opus_decoder_);
            opus_decoder_ = nullptr;
            return false;
        }
    } else if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
    if (decoder_frame_size_ <= 0 || decoder_frame_size_ > MAX_AUDIO_TASK_PCM_SAMPLES) {
        ESP_LOGE(TAG, "AUDIO_DECODE_INVALID_FRAME_SIZE samples=%d", decoder_frame_size_);
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
        return false;
    }
    return true;
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    AssignAudioTaskId(task.get());

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        QuarantineCorruptEncodeTasksLocked("encode_enqueue");

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
                task->timestamp = timestamp_queue_.front();
            } else {
                ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp",
                         timestamp_queue_.size());
            }
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            auto dropped = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            if (dropped) {
                if (IsRegisteredAudioTaskLocked(dropped.get())) {
                    TraceAudioTask("DISCARD", dropped.get(), audio_encode_queue_.size());
                    UnregisterAudioTaskLocked(dropped.get());
                } else {
                    ESP_LOGE(TAG,
                             "AUDIO_QUEUE_CORRUPTION checkpoint=encode_drop raw=%p size=%u",
                             dropped.get(), static_cast<unsigned>(audio_encode_queue_.size()));
                    dropped.release();
                }
            }
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        if (!RegisterAudioTaskLocked(task.get())) {
            ESP_LOGE(TAG, "AUDIO_TASK_DISCARD task_id=%llu reason=registry_full",
                     static_cast<unsigned long long>(task->lifecycle_id));
            TraceAudioTask("DESTROY", task.get(), audio_encode_queue_.size());
            return;
        }
        TraceAudioTask("CREATE", task.get(), audio_encode_queue_.size());
        TraceAudioTask("ENQUEUE", task.get(), audio_encode_queue_.size() + 1);
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

uint32_t AudioService::GetPlaybackGeneration() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return playback_generation_;
}

uint32_t AudioService::GetMediaGeneration() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return media_generation_;
}

void AudioService::DiscardMediaPlayback(const char* reason) {
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        // Reject PCM that a media producer is converting right now. Speech,
        // the wake popup and other system sounds keep their own generation.
        ++media_generation_;
        DiscardPlaybackTasksLocked(false, true, reason);
        radio_buffered_duration_ms_ = 0;
        radio_prebuffer_ready_generation_ = 0;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::WaitForMediaDrained(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    return audio_queue_cv_.wait_for(lock, timeout, [this]() {
        if (service_stopped_.load()) {
            return true;
        }
        if (media_output_in_flight_) {
            return false;
        }
        for (const auto& task : audio_playback_queue_) {
            if (task->is_music) {
                return false;
            }
        }
        return true;
    });
}

bool AudioService::PushPlaybackTask(std::vector<int16_t>&& pcm, bool is_music, bool is_radio,
                                    uint32_t duration_ms, bool stream_start,
                                    uint32_t expected_generation) {
    if (pcm.empty() || pcm.size() > MAX_AUDIO_TASK_PCM_SAMPLES || codec_ == nullptr) {
        if (pcm.size() > MAX_AUDIO_TASK_PCM_SAMPLES) {
            ESP_LOGW(TAG, "PushPlaybackTask dropped oversized PCM samples=%u",
                     static_cast<unsigned>(pcm.size()));
        }
        return false;
    }

    if (service_stopped_.load()) {
        ESP_LOGW(TAG, "PushPlaybackTask dropped PCM because AudioService is stopped!");
        return false;
    }

    if (is_music) {
        const auto device_state = Application::GetInstance().GetDeviceState();
        if (IsVoiceAudioState(device_state)) {
            return false;
        }
    }

    while (!service_stopped_.load()) {
        if (is_music) {
            const auto device_state = Application::GetInstance().GetDeviceState();
            if (IsVoiceAudioState(device_state)) {
                return false;
            }
        }
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        if (expected_generation != 0 && expected_generation != media_generation_) {
            ESP_LOGW(TAG,
                     "AUDIO_MEDIA_STALE_DROP expected_generation=%lu current_generation=%lu "
                     "source=%s",
                     static_cast<unsigned long>(expected_generation),
                     static_cast<unsigned long>(media_generation_),
                     is_radio ? "radio" : "sd");
            return false;
        }
        const size_t max_queue_size = is_radio ? 90 : 4;
        const bool queue_has_capacity = audio_playback_queue_.size() < max_queue_size;
        const bool radio_has_capacity = !is_radio || RADIO_PREBUFFER_MS == 0 ||
                                        radio_buffered_duration_ms_ < RADIO_PREBUFFER_MS;
        if (queue_has_capacity && radio_has_capacity) {
            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->is_music = is_music;
            task->is_radio = is_radio;
            task->output_source = is_music ? AudioOutputSource::kMedia : AudioOutputSource::kSystem;
            task->stream_start = stream_start;
            task->lifecycle_generation = playback_generation_;
            task->duration_ms = duration_ms;
            task->created_at_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            AssignAudioTaskId(task.get());
            if (is_radio) {
                radio_buffered_duration_ms_ += duration_ms;
                task->radio_pcm.assign(pcm.begin(), pcm.end());
                const size_t q_size = audio_playback_queue_.size() + 1;
                if (radio_buffered_duration_ms_ == duration_ms || q_size == 32) {
                    const void* ptr = task->radio_pcm.data();
                    const bool in_psram = esp_ptr_external_ram(ptr);
                    ESP_LOGI(TAG, "[RADIO_PCM] ptr=%p size=%u bytes PSRAM=%s (queue=%zu)", ptr,
                             static_cast<unsigned>(task->radio_pcm.size() * sizeof(int16_t)),
                             in_psram ? "YES" : "NO", q_size);
                }
            } else {
                task->pcm = std::move(pcm);
            }
            if (!RegisterAudioTaskLocked(task.get())) {
                ESP_LOGE(TAG, "AUDIO_TASK_DISCARD task_id=%llu reason=registry_full",
                         static_cast<unsigned long long>(task->lifecycle_id));
                TraceAudioTask("DESTROY", task.get(), audio_playback_queue_.size());
                return false;
            }
            TraceAudioTask("CREATE", task.get(), audio_playback_queue_.size());
            TraceAudioTask("ENQUEUE", task.get(), audio_playback_queue_.size() + 1);
            if (stream_start) {
                ESP_LOGI(TAG,
                         "AUDIO_TASK_ENQUEUE source=%s generation=%lu task=%p pcm=%p samples=%u "
                         "queue=%u",
                         is_radio ? "radio" : "sd",
                         static_cast<unsigned long>(playback_generation_), task.get(),
                         task->GetPcmData(), static_cast<unsigned>(task->GetPcmSize()),
                         static_cast<unsigned>(audio_playback_queue_.size() + 1));
            }
            audio_playback_queue_.push_back(std::move(task));
            if (is_radio && RADIO_PREBUFFER_MS > 0 &&
                radio_buffered_duration_ms_ >= RADIO_PREBUFFER_MS &&
                radio_prebuffer_ready_generation_ == 0) {
                radio_prebuffer_ready_generation_ = media_generation_;
                ESP_LOGI(TAG, "[RADIO_PREBUFFER_READY] generation=%lu buffer=%u ms",
                         static_cast<unsigned long>(media_generation_),
                         static_cast<unsigned>(radio_buffered_duration_ms_));
            }
            audio_queue_cv_.notify_all();
            return true;
        }
        lock.unlock();
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return false;
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t generation = playback_generation_;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this, generation]() {
                return service_stopped_.load() || generation != playback_generation_ ||
                       audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            ++decode_queue_drop_count_;
            return false;
        }
    }
    if (service_stopped_.load() || generation != playback_generation_) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

void AudioService::BeginSpeechDiagnostics() {
    playback_underrun_count_.store(0, std::memory_order_relaxed);
    decode_queue_drop_count_.store(0, std::memory_order_relaxed);
    playback_min_queue_depth_.store(MAX_PLAYBACK_TASKS_IN_QUEUE, std::memory_order_relaxed);
    last_speech_diagnostic_us_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        speech_prebuffering_ = false;
        speech_prebuffer_started_us_ = 0;
    }
    speech_diagnostics_active_.store(true, std::memory_order_release);
    LogSpeechDiagnostics("tts_start");
}

void AudioService::EndSpeechDiagnostics() {
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        speech_prebuffering_ = false;
        speech_prebuffer_started_us_ = 0;
        audio_queue_cv_.notify_all();
    }
    if (speech_diagnostics_active_.exchange(false, std::memory_order_acq_rel)) {
        LogSpeechDiagnostics("tts_stop");
    }
}

void AudioService::MaybeLogSpeechDiagnostics() {
    if (!speech_diagnostics_active_.load(std::memory_order_acquire)) return;
    const int64_t now_us = esp_timer_get_time();
    const int64_t previous = last_speech_diagnostic_us_.load(std::memory_order_relaxed);
    if (previous != 0 && now_us - previous < 1000000) return;
    last_speech_diagnostic_us_.store(now_us, std::memory_order_relaxed);
    LogSpeechDiagnostics("tts_active");
}

void AudioService::LogSpeechDiagnostics(const char* reason) {
    size_t decode_q = 0;
    size_t playback_q = 0;
    bool decode_in_flight = false;
    bool output_in_flight = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        decode_q = audio_decode_queue_.size();
        playback_q = audio_playback_queue_.size();
        decode_in_flight = decode_in_flight_;
        output_in_flight = output_in_flight_;
    }
    size_t minimum = playback_min_queue_depth_.load(std::memory_order_relaxed);
    if (playback_q < minimum) {
        playback_min_queue_depth_.store(playback_q, std::memory_order_relaxed);
        minimum = playback_q;
    }
    ESP_LOGI(TAG,
             "AUDIO_DIAG reason=%s dec_q=%u play_q=%u dec_inflight=%d out_inflight=%d "
             "underruns=%u decode_drops=%u play_min=%u internal_free=%u internal_largest=%u "
             "internal_min=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u",
             reason, static_cast<unsigned>(decode_q), static_cast<unsigned>(playback_q),
             decode_in_flight ? 1 : 0, output_in_flight ? 1 : 0,
             static_cast<unsigned>(playback_underrun_count_.load(std::memory_order_relaxed)),
             static_cast<unsigned>(decode_queue_drop_count_.load(std::memory_order_relaxed)),
             static_cast<unsigned>(minimum),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!InitializeAudioEngine()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
        auto* lite_engine = static_cast<LiteAudioEngine*>(audio_engine_.get());
        if (!lite_engine->RestoreWakeWordResources()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#endif
        if (!audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::ReleaseWakeWordResources() {
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
    if (!audio_engine_initialized_) {
        return;
    }
    if (xEventGroupGetBits(event_group_) &
        (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
        ESP_LOGW(TAG, "Cannot release WakeNet while the audio engine is active");
        return;
    }
    static_cast<LiteAudioEngine*>(audio_engine_.get())->ReleaseWakeWordResources();
#endif
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
        ResetDecoder();
        audio_input_need_warmup_ = true;
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket(
        [this](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->is_system_sound = true;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            PushPacketToDecodeQueue(std::move(packet), true);
        });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ResetDecoder() {
    CheckHeapIntegrity("reset_decoder_begin");
    ai_speech_level_.store(0, std::memory_order_relaxed);
    ai_speech_level_updated_ms_.store(0, std::memory_order_release);
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        ++media_generation_;
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        DiscardPlaybackTasksLocked(false, false, "reset_decoder");
        speech_prebuffering_ = false;
        speech_prebuffer_started_us_ = 0;
        radio_buffered_duration_ms_ = 0;
        radio_prebuffer_ready_generation_ = 0;
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
    CheckHeapIntegrity("reset_decoder_end");
}

uint8_t AudioService::GetAiSpeechLevel() const {
    const uint32_t updated_ms = ai_speech_level_updated_ms_.load(std::memory_order_acquire);
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (updated_ms == 0 || now_ms - updated_ms > 220) {
        return 0;
    }
    return ai_speech_level_.load(std::memory_order_relaxed);
}

bool AudioService::WaitForPlaybackDrained(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    return playback_drain_cv_.wait_for(lock, timeout, [this]() {
        return IsPlaybackDrainedLocked();
    });
}

bool AudioService::IsPlaybackDrainedLocked() const {
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() && !decode_in_flight_ &&
           !output_in_flight_;
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked()) {
        return false;
    }
    playback_drain_cv_.notify_all();
    if (playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        // ADC continuous start/stop must run in the same task. Wake the audio
        // input task instead of closing the codec from the esp_timer task.
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        if (AudioManager::GetInstance().GetCurrentAudioSource() == kAudioSourceMp3Player ||
            AudioManager::GetInstance().GetCurrentAudioSource() == kAudioSourceInternetRadio ||
            SdMusicPlayer::GetInstance().IsPlaying()) {
            last_output_time_ = now;
        } else if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}

uint32_t AudioService::GetRadioBufferedMs() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return radio_buffered_duration_ms_;
}

bool AudioService::IsRadioPrebufferReady(uint32_t expected_generation) {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (RADIO_PREBUFFER_MS == 0) {
        return expected_generation == 0 || expected_generation == media_generation_;
    }
    return radio_prebuffer_ready_generation_ != 0 &&
           (expected_generation == 0 ||
            radio_prebuffer_ready_generation_ == expected_generation);
}

void AudioService::DiscardRadioPrebuffer() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    DiscardPlaybackTasksLocked(true, false, "radio_prebuffer_discard");
    radio_buffered_duration_ms_ = 0;
    radio_prebuffer_ready_generation_ = 0;
    audio_queue_cv_.notify_all();
}

size_t AudioService::GetRadioQueueSize() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    QuarantineCorruptPlaybackTasksLocked("radio_queue_size");
    size_t count = 0;
    for (const auto& task : audio_playback_queue_) {
        if (task->is_radio) {
            count++;
        }
    }
    return count;
}
