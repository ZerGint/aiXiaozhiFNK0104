#include "external_i2s_audio_codec.h"

#if CONFIG_FNK_EXTERNAL_I2S_SPEAKER

#include "config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {
constexpr char kTag[] = "FnkExternalI2s";
constexpr int kVolumeMaximum = 100;
constexpr int kAiMinimumVolume = 20;
constexpr int kAiVolumeOffset = 20;
constexpr uint32_t kAiGainRampSamples = 480;
constexpr int kExternalDmaDescriptors = 4;
constexpr int kExternalDmaFrames = 120;
constexpr uint32_t kDiagnosticIntervalUs = 1000000;

int AiLogicalVolume(int user_volume) {
    return user_volume == 0 ? 0 : std::max(kAiMinimumVolume, user_volume - kAiVolumeOffset);
}
}  // namespace

FnkExternalI2sAudioCodec::FnkExternalI2sAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port)
    : Es8311AudioCodec(i2c_master_handle, i2c_port, AUDIO_INPUT_SAMPLE_RATE,
                       AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
                       AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC,
                       AUDIO_CODEC_ES8311_ADDR, true, true) {
    duplex_ = false;
    output_sample_rate_ = EXTERNAL_I2S_OUTPUT_SAMPLE_RATE;

    ESP_ERROR_CHECK(gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(AUDIO_CODEC_PA_PIN, 1));

    i2s_chan_config_t channel_config = {
        .id = XIAOZHI_I2S_PORT(EXTERNAL_I2S_PORT),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = kExternalDmaDescriptors,
        .dma_frame_num = kExternalDmaFrames,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &external_tx_handle_, nullptr));

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXTERNAL_I2S_OUTPUT_SAMPLE_RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = GPIO_NUM_NC,
                .bclk = EXTERNAL_I2S_BCLK_PIN,
                .ws = EXTERNAL_I2S_WS_PIN,
                .dout = EXTERNAL_I2S_DATA_PIN,
                .din = GPIO_NUM_NC,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(external_tx_handle_, &standard_config));

    ESP_LOGI(kTag, "AUDIO_OUTPUT: EXTERNAL_I2S");
    ESP_LOGI(kTag, "I2S_PORT: %d", EXTERNAL_I2S_PORT);
    ESP_LOGI(kTag, "BCLK: GPIO%d", EXTERNAL_I2S_BCLK_PIN);
    ESP_LOGI(kTag, "WS: GPIO%d", EXTERNAL_I2S_WS_PIN);
    ESP_LOGI(kTag, "DATA: GPIO%d", EXTERNAL_I2S_DATA_PIN);
    ESP_LOGI(kTag, "RATE: %d", EXTERNAL_I2S_OUTPUT_SAMPLE_RATE);
    ESP_LOGI(kTag, "BITS: 16");
    ESP_LOGI(kTag, "CHANNELS: 2 (mono duplicated)");
}

FnkExternalI2sAudioCodec::~FnkExternalI2sAudioCodec() {
    if (external_tx_handle_ != nullptr) {
        if (output_enabled_) {
            i2s_channel_disable(external_tx_handle_);
        }
        i2s_del_channel(external_tx_handle_);
    }
}

void FnkExternalI2sAudioCodec::SetOutputVolume(int volume) {
    volume = std::clamp(volume, 0, kVolumeMaximum);
    {
        std::lock_guard<std::mutex> lock(data_if_mutex_);
        output_volume_ = volume;
        const int32_t user_gain = PerceptualGain(volume);
        if (volume == 0 || output_source_ == AudioOutputSource::kSystem) {
            SetImmediateGain(user_gain);
        } else if (output_source_ == AudioOutputSource::kAiSpeech) {
            fade_start_gain_ = current_gain_;
            target_gain_ = PerceptualGain(AiLogicalVolume(volume));
            fade_position_ = 0;
            fade_length_ = kAiGainRampSamples;
        } else if (output_source_ == AudioOutputSource::kMedia) {
            if (fade_length_ > fade_position_) {
                RetargetFade(user_gain);
            } else {
                SetImmediateGain(user_gain);
            }
        }
    }
    AudioCodec::SetOutputVolume(volume);
}

int32_t FnkExternalI2sAudioCodec::PerceptualGain(int volume) {
    struct Point {
        int volume;
        int gain;
    };
    static constexpr std::array<Point, 10> curve{{
        {0, 0},
        {5, 146},
        {10, 413},
        {20, 1163},
        {30, 2320},
        {40, 4125},
        {50, 6538},
        {60, 10362},
        {80, 16423},
        {100, 26214},
    }};
    volume = std::clamp(volume, 0, kVolumeMaximum);
    for (size_t i = 1; i < curve.size(); ++i) {
        if (volume <= curve[i].volume) {
            const auto& low = curve[i - 1];
            const auto& high = curve[i];
            return low.gain +
                   (high.gain - low.gain) * (volume - low.volume) / (high.volume - low.volume);
        }
    }
    return curve.back().gain;
}

void FnkExternalI2sAudioCodec::SetImmediateGain(int32_t gain) {
    current_gain_ = target_gain_ = fade_start_gain_ = gain;
    fade_position_ = fade_length_ = 0;
}

void FnkExternalI2sAudioCodec::RetargetFade(int32_t gain) {
    const uint32_t remaining = fade_length_ > fade_position_ ? fade_length_ - fade_position_ : 0;
    fade_start_gain_ = current_gain_;
    target_gain_ = gain;
    fade_position_ = 0;
    fade_length_ = remaining;
    if (remaining == 0)
        SetImmediateGain(gain);
}

int32_t FnkExternalI2sAudioCodec::NextGain() {
    if (fade_position_ < fade_length_) {
        ++fade_position_;
        current_gain_ = fade_start_gain_ +
                        static_cast<int32_t>(static_cast<int64_t>(target_gain_ - fade_start_gain_) *
                                             fade_position_ / fade_length_);
        if (fade_position_ == fade_length_)
            current_gain_ = target_gain_;
    }
    return current_gain_;
}

void FnkExternalI2sAudioCodec::SetOutputSource(AudioOutputSource source, bool stream_start) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    const bool source_changed = output_source_ != source;
    output_source_ = source;
    const int32_t user_gain = PerceptualGain(output_volume_);
    if (output_volume_ == 0) {
        SetImmediateGain(0);
    } else if (source == AudioOutputSource::kAiSpeech) {
        const int32_t ai_gain = PerceptualGain(AiLogicalVolume(output_volume_));
        if (source_changed || (fade_position_ >= fade_length_ && target_gain_ != ai_gain)) {
            SetImmediateGain(ai_gain);
        }
    } else if (source == AudioOutputSource::kMedia && stream_start) {
        current_gain_ = fade_start_gain_ = 0;
        target_gain_ = user_gain;
        fade_position_ = 0;
        fade_length_ = kFadeSamples;
        ESP_LOGI(kTag, "MEDIA_FADE_START user=%d target_gain=%ld samples=%lu", output_volume_,
                 static_cast<long>(target_gain_), static_cast<unsigned long>(fade_length_));
    } else if (source == AudioOutputSource::kMedia && fade_position_ < fade_length_) {
        if (target_gain_ != user_gain)
            RetargetFade(user_gain);
    } else {
        SetImmediateGain(user_gain);
    }
    if (source_changed && source == AudioOutputSource::kAiSpeech) {
        ESP_LOGI(kTag, "AI_VOLUME user=%d effective=%d gain=%ld", output_volume_,
                 AiLogicalVolume(output_volume_), static_cast<long>(current_gain_));
    }
}

void FnkExternalI2sAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }

    if (enable) {
        const esp_err_t err = i2s_channel_enable(external_tx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to enable external I2S output: %s", esp_err_to_name(err));
            return;
        }
        AudioCodec::EnableOutput(true);
        return;
    }

    AudioCodec::EnableOutput(false);
    const esp_err_t err = i2s_channel_disable(external_tx_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to disable external I2S output: %s", esp_err_to_name(err));
    }
}

int FnkExternalI2sAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_enabled_) {
        return samples;
    }

    std::array<int16_t, kConversionFrames * 2> stereo{};
    int offset = 0;
    while (offset < samples) {
        const int frame_count = std::min(samples - offset, static_cast<int>(kConversionFrames));
        for (int i = 0; i < frame_count; ++i) {
            const int32_t gain = NextGain();
            const int32_t scaled =
                static_cast<int32_t>(static_cast<int64_t>(data[offset + i]) * gain / kGainScale);
            const int16_t sample =
                static_cast<int16_t>(std::clamp<int32_t>(scaled, INT16_MIN, INT16_MAX));
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }

#if CONFIG_FNK_EXTERNAL_I2S_DIAGNOSTICS
        UpdateDiagnostics(data + offset, stereo.data(), frame_count, output_volume_);
#endif

        size_t bytes_written = 0;
        const size_t bytes_to_write = static_cast<size_t>(frame_count) * 2 * sizeof(int16_t);
        const esp_err_t err = i2s_channel_write(external_tx_handle_, stereo.data(), bytes_to_write,
                                                &bytes_written, portMAX_DELAY);
        if (err != ESP_OK || bytes_written != bytes_to_write) {
            ESP_LOGE(kTag, "External I2S write failed: %s (%u/%u bytes)", esp_err_to_name(err),
                     static_cast<unsigned>(bytes_written), static_cast<unsigned>(bytes_to_write));
            break;
        }
        offset += frame_count;
    }
    return offset;
}

#if CONFIG_FNK_EXTERNAL_I2S_DIAGNOSTICS
void FnkExternalI2sAudioCodec::UpdateDiagnostics(const int16_t* input, const int16_t* output,
                                                 int samples, int volume) {
    for (int i = 0; i < samples; ++i) {
        const int32_t value = input[i];
        const uint32_t absolute =
            value < 0 ? static_cast<uint32_t>(-value) : static_cast<uint32_t>(value);
        diagnostics_.input_min = std::min(diagnostics_.input_min, input[i]);
        diagnostics_.input_max = std::max(diagnostics_.input_max, input[i]);
        diagnostics_.input_peak = std::max(diagnostics_.input_peak, absolute);
        diagnostics_.input_square_sum += static_cast<uint64_t>(value * value);
        diagnostics_.sample_count++;
        diagnostics_.input_full_scale += input[i] == INT16_MIN || input[i] == INT16_MAX;
        diagnostics_.above_90_percent += absolute > 29490;
        diagnostics_.above_95_percent += absolute > 31128;
        diagnostics_.above_99_percent += absolute > 32439;

        const int32_t scaled = value * volume / kVolumeMaximum;
        diagnostics_.output_saturated += scaled < INT16_MIN || scaled > INT16_MAX;
        const int32_t output_value = output[i * 2];
        const uint32_t output_absolute = output_value < 0 ? static_cast<uint32_t>(-output_value)
                                                          : static_cast<uint32_t>(output_value);
        diagnostics_.output_peak = std::max(diagnostics_.output_peak, output_absolute);
    }

    const int64_t now_us = esp_timer_get_time();
    if (diagnostics_.sample_count == 0 ||
        now_us - diagnostics_.last_log_us < kDiagnosticIntervalUs) {
        return;
    }

    const double rms = std::sqrt(static_cast<double>(diagnostics_.input_square_sum) /
                                 static_cast<double>(diagnostics_.sample_count));
    ESP_LOGI(kTag,
             "PCM_DIAG samples=%llu volume=%d input[min=%d max=%d peak=%lu rms=%.1f "
             "full_scale=%lu >90%%=%lu >95%%=%lu >99%%=%lu] "
             "output[peak=%lu saturated=%lu]",
             static_cast<unsigned long long>(diagnostics_.sample_count), volume,
             diagnostics_.input_min, diagnostics_.input_max,
             static_cast<unsigned long>(diagnostics_.input_peak), rms,
             static_cast<unsigned long>(diagnostics_.input_full_scale),
             static_cast<unsigned long>(diagnostics_.above_90_percent),
             static_cast<unsigned long>(diagnostics_.above_95_percent),
             static_cast<unsigned long>(diagnostics_.above_99_percent),
             static_cast<unsigned long>(diagnostics_.output_peak),
             static_cast<unsigned long>(diagnostics_.output_saturated));
    diagnostics_ = {};
    diagnostics_.input_min = INT16_MAX;
    diagnostics_.input_max = INT16_MIN;
    diagnostics_.last_log_us = now_us;
}
#endif

#endif
