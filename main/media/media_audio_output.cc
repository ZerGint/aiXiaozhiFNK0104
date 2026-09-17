#include "media_audio_output.h"

#include "application.h"
#include "audio_codec.h"

#include <esp_ae_rate_cvt.h>
#include <esp_log.h>
#include <decoder/impl/esp_aac_dec.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace {
constexpr char TAG[] = "MediaAudioOutput";
std::atomic<bool> stream_start_pending{false};

class MediaRateConverter {
public:
    ~MediaRateConverter() { Close(); }

    void ResetStream() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ != nullptr)
            esp_ae_rate_cvt_reset(handle_);
    }

    bool Process(const int16_t* input, size_t input_samples, uint32_t source_rate,
                 uint32_t target_rate, std::vector<int16_t>& output) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_rate == target_rate) {
            Close();
            output.assign(input, input + input_samples);
            return true;
        }
        if (handle_ == nullptr || source_rate_ != source_rate || target_rate_ != target_rate) {
            Close();
            esp_ae_rate_cvt_cfg_t config = {
                .src_rate = source_rate,
                .dest_rate = target_rate,
                .channel = 1,
                .bits_per_sample = 16,
                .complexity = 3,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
            };
            const esp_ae_err_t result = esp_ae_rate_cvt_open(&config, &handle_);
            if (result != ESP_AE_ERR_OK || handle_ == nullptr) {
                ESP_LOGE(TAG, "Unable to create media rate converter %lu -> %lu: %d",
                         static_cast<unsigned long>(source_rate),
                         static_cast<unsigned long>(target_rate), static_cast<int>(result));
                handle_ = nullptr;
                return false;
            }
            source_rate_ = source_rate;
            target_rate_ = target_rate;
            ESP_LOGI(TAG, "Stateful media resampler: %lu -> %lu Hz, mono, quality=3, memory mode",
                     static_cast<unsigned long>(source_rate),
                     static_cast<unsigned long>(target_rate));
        }
        uint32_t output_capacity = 0;
        const esp_ae_err_t size_result = esp_ae_rate_cvt_get_max_out_sample_num(
            handle_, static_cast<uint32_t>(input_samples), &output_capacity);
        if (size_result != ESP_AE_ERR_OK || output_capacity == 0)
            return false;
        output.resize(output_capacity);
        uint32_t output_samples = output_capacity;
        const esp_ae_err_t process_result = esp_ae_rate_cvt_process(
            handle_, const_cast<int16_t*>(input), static_cast<uint32_t>(input_samples),
            output.data(), &output_samples);
        if (process_result != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "Media rate conversion failed: %d", static_cast<int>(process_result));
            output.clear();
            return false;
        }
        output.resize(output_samples);
        return true;
    }

private:
    void Close() {
        if (handle_ != nullptr)
            esp_ae_rate_cvt_close(handle_);
        handle_ = nullptr;
        source_rate_ = target_rate_ = 0;
    }
    std::mutex mutex_;
    esp_ae_rate_cvt_handle_t handle_ = nullptr;
    uint32_t source_rate_ = 0;
    uint32_t target_rate_ = 0;
};

MediaRateConverter& GetMediaRateConverter() {
    static MediaRateConverter converter;
    return converter;
}
}  // namespace

void BeginMediaPcmStream() {
    GetMediaRateConverter().ResetStream();
    stream_start_pending = true;
}

void EnsureMp3DecoderRegistered() {
    static std::once_flag registration_once;
    std::call_once(registration_once, []() {
        const esp_audio_err_t mp3_result = esp_mp3_dec_register();
        if (mp3_result != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "MP3 decoder registration returned: %d", static_cast<int>(mp3_result));
        }
        const esp_audio_err_t aac_result = esp_aac_dec_register();
        if (aac_result != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AAC decoder registration returned: %d", static_cast<int>(aac_result));
        }
        esp_audio_simple_dec_register_default();
    });
}

void PushMediaPcm(AudioCodec* codec, const int16_t* pcm_data, size_t num_samples, uint32_t channels,
                  uint32_t sample_rate, uint32_t target_rate, bool is_radio) {
    if (!codec || !pcm_data || num_samples == 0) {
        return;
    }

    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    std::vector<int16_t> mono;
    const int16_t* samples = pcm_data;
    size_t count = num_samples;
    if (channels == 2) {
        count = num_samples / 2;
        mono.resize(count);
        for (size_t i = 0; i < count; ++i) {
            mono[i] = static_cast<int16_t>((static_cast<int32_t>(pcm_data[i * 2]) +
                                            static_cast<int32_t>(pcm_data[i * 2 + 1])) /
                                           2);
        }
        samples = mono.data();
    }

    if (sample_rate == 0 || target_rate == 0) {
        ESP_LOGE(TAG, "Invalid media sample rate: %lu -> %lu",
                 static_cast<unsigned long>(sample_rate), static_cast<unsigned long>(target_rate));
        return;
    }

    std::vector<int16_t> output;
    if (!GetMediaRateConverter().Process(samples, count, sample_rate, target_rate, output) ||
        output.empty()) {
        return;
    }

    const uint32_t duration_ms = static_cast<uint32_t>(output.size() * 1000ULL / target_rate);
    Application::GetInstance().GetAudioService().PushPlaybackTask(
        std::move(output), true, is_radio, duration_ms, stream_start_pending.exchange(false));
}
