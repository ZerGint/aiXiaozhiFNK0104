#include "audio_manager.h"
#include <esp_log.h>

#define TAG "AudioManager"

bool AudioManager::RequestAudioFocus(AudioSource source) {
    if (current_source_ == source) return true;

    ESP_LOGI(TAG, "Audio focus requested by source %d (current %d)", static_cast<int>(source), static_cast<int>(current_source_));

    // Priority: XiaoZhi TTS > MP3/Radio/Emulator
    if (source == kAudioSourceXiaoZhiTts && current_source_ != kAudioSourceNone) {
        paused_source_ = current_source_;
        ESP_LOGI(TAG, "Pausing current audio source %d for TTS", static_cast<int>(paused_source_));
    }

    current_source_ = source;
    return true;
}

void AudioManager::ReleaseAudioFocus(AudioSource source) {
    if (current_source_ == source) {
        ESP_LOGI(TAG, "Releasing audio focus from source %d", static_cast<int>(source));
        current_source_ = paused_source_;
        paused_source_ = kAudioSourceNone;
    }
}

void AudioManager::SetMasterVolume(uint8_t volume) {
    volume_ = (volume > 100) ? 100 : volume;
    ESP_LOGI(TAG, "Master volume set to %d", volume_);
}

void AudioManager::Mute(bool mute) {
    is_muted_ = mute;
    ESP_LOGI(TAG, "Audio muted: %s", mute ? "true" : "false");
}
