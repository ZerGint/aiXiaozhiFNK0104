#include "media_audio_output.h"

#include "application.h"
#include "audio_codec.h"

#include <decoder/impl/esp_mp3_dec.h>
#include <decoder/impl/esp_aac_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>
#include <esp_log.h>

#include <mutex>
#include <utility>
#include <vector>

namespace {
constexpr char TAG[] = "MediaAudioOutput";
}

void EnsureMp3DecoderRegistered() {
    static std::once_flag registration_once;
    std::call_once(registration_once, []() {
        const esp_audio_err_t mp3_result = esp_mp3_dec_register();
        if (mp3_result != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "MP3 decoder registration returned: %d",
                     static_cast<int>(mp3_result));
        }
        const esp_audio_err_t aac_result = esp_aac_dec_register();
        if (aac_result != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AAC decoder registration returned: %d",
                     static_cast<int>(aac_result));
        }
        esp_audio_simple_dec_register_default();
    });
}

void PushMediaPcm(AudioCodec* codec,
                  const int16_t* pcm_data,
                  size_t num_samples,
                  uint32_t channels,
                  uint32_t sample_rate,
                  uint32_t target_rate,
                  bool is_radio) {
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
            mono[i] = static_cast<int16_t>(
                (static_cast<int32_t>(pcm_data[i * 2]) +
                 static_cast<int32_t>(pcm_data[i * 2 + 1])) / 2);
        }
        samples = mono.data();
    }

    if (sample_rate == 0 || target_rate == 0 || sample_rate == target_rate) {
        const uint32_t duration_ms = target_rate == 0
            ? 0
            : static_cast<uint32_t>(count * 1000ULL / target_rate);
        Application::GetInstance().GetAudioService().PushPlaybackTask(
            std::vector<int16_t>(samples, samples + count), true, is_radio, duration_ms);
        return;
    }

    const size_t output_count = (count * target_rate) / sample_rate;
    if (output_count == 0) {
        return;
    }

    std::vector<int16_t> output(output_count);
    const double step = static_cast<double>(sample_rate) / target_rate;
    for (size_t i = 0; i < output_count; ++i) {
        const double position = i * step;
        const size_t index = static_cast<size_t>(position);
        const double fraction = position - index;
        if (index + 1 < count) {
            output[i] = static_cast<int16_t>(
                samples[index] + fraction * (samples[index + 1] - samples[index]));
        } else if (index < count) {
            output[i] = samples[index];
        }
    }

    const uint32_t duration_ms = static_cast<uint32_t>(output.size() * 1000ULL / target_rate);
    Application::GetInstance().GetAudioService().PushPlaybackTask(
        std::move(output), true, is_radio, duration_ms);
}
