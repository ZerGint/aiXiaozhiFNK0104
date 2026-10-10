#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <nvs.h>
#include <cstring>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { static_cast<AudioCodec*>(arg)->HandleOutputVolumeSaveTimer(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "volume_save",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &volume_save_timer_));
}

AudioCodec::~AudioCodec() {
    if (volume_save_timer_ != nullptr) {
        esp_timer_stop(volume_save_timer_);
        esp_timer_delete(volume_save_timer_);
    }
}

void AudioCodec::OutputData(std::vector<int16_t>& data) { Write(data.data(), data.size()); }

void AudioCodec::OutputData(const int16_t* data, size_t samples) {
    if (data && samples > 0) {
        Write(data, static_cast<int>(samples));
    }
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ < 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)",
                 output_volume_);
        output_volume_ = 10;
    }
    persisted_output_volume_ = output_volume_;

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    ScheduleOutputVolumeSave();
}

void AudioCodec::ScheduleOutputVolumeSave() {
    if (volume_save_timer_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(volume_save_mutex_);
    last_volume_change_us_ = esp_timer_get_time();
    esp_timer_stop(volume_save_timer_);
    const esp_err_t err = esp_timer_start_once(volume_save_timer_, kVolumeSaveDebounceUs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule output volume save: %s", esp_err_to_name(err));
    }
}

void AudioCodec::HandleOutputVolumeSaveTimer() {
    std::lock_guard<std::mutex> lock(volume_save_mutex_);
    const int64_t elapsed_us = esp_timer_get_time() - last_volume_change_us_;
    if (elapsed_us < static_cast<int64_t>(kVolumeSaveDebounceUs)) {
        const esp_err_t err = esp_timer_start_once(
            volume_save_timer_, kVolumeSaveDebounceUs - static_cast<uint64_t>(elapsed_us));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reschedule output volume save: %s", esp_err_to_name(err));
        }
        return;
    }
    SaveOutputVolumeLocked();
}

void AudioCodec::PersistOutputVolume() {
    std::lock_guard<std::mutex> lock(volume_save_mutex_);
    if (volume_save_timer_ != nullptr) {
        esp_timer_stop(volume_save_timer_);
    }
    SaveOutputVolumeLocked();
}

bool AudioCodec::SaveOutputVolumeLocked() {
    const int volume = output_volume_;
    if (volume == persisted_output_volume_) {
        return true;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("audio", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, "output_volume", volume);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist output volume %d: %s", volume, esp_err_to_name(err));
        return false;
    }

    persisted_output_volume_ = volume;
    ESP_LOGI(TAG, "Persisted output volume %d", volume);
    return true;
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
