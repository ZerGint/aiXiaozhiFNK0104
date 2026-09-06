#include "sd_music_player.h"
#include "media/media_audio_output.h"
#include "storage_manager.h"
#include "audio_manager.h"
#include "board.h"
#include "audio_codec.h"
#include "application.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>
#include <cstdio>
#include <cstring>

#define TAG "SdMusicPlayer"

struct ChunkHeader {
    char id[4];
    uint32_t size;
};

struct FmtChunk {
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

SdMusicPlayer::SdMusicPlayer() {
}

SdMusicPlayer::~SdMusicPlayer() {
    Stop();
}

void SdMusicPlayer::ScanPlaylist() {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_.clear();

    const char* paths[] = {"/sdcard/music", "/sdcard/Music", "/sdcard/MUSIC", "/sdcard"};

    for (auto p : paths) {
        auto files = StorageManager::GetInstance().ListDirectory(p);
        if (!files.empty()) {
            ESP_LOGI(TAG, "Scanning directory %s (%d total entries)...", p, (int)files.size());
            for (const auto& name : files) {
                std::string lower_name = name;
                for (auto& c : lower_name) c = tolower((unsigned char)c);
                if (lower_name.rfind(".wav") != std::string::npos ||
                    lower_name.rfind(".mp3") != std::string::npos) {
                    std::string full_path = std::string(p) + "/" + name;
                    playlist_.push_back(full_path);
                    ESP_LOGI(TAG, " 🎵 Track found: %s", full_path.c_str());
                }
            }
            if (!playlist_.empty()) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "ScanPlaylist complete. Total tracks found: %d", (int)playlist_.size());
}

std::string SdMusicPlayer::GetCurrentTrackName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_index_ >= 0 && current_index_ < (int)playlist_.size()) {
        std::string full = playlist_[current_index_];
        size_t last_slash = full.find_last_of('/');
        if (last_slash != std::string::npos) {
            return full.substr(last_slash + 1);
        }
        return full;
    }
    return "Нет трека";
}

void SdMusicPlayer::TaskFunction(void* param) {
    auto player = static_cast<SdMusicPlayer*>(param);
    player->PlayerLoop();
    player->is_playing_ = false;
    player->task_handle_ = nullptr;
    vTaskDelete(NULL);
}

void SdMusicPlayer::Play(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            ESP_LOGW(TAG, "Cannot play: playlist is empty");
            return;
        }
        if (index < 0) index = 0;
        if (index >= (int)playlist_.size()) index = 0;
        current_index_ = index;
    }

    if (is_playing_) {
        skip_requested_ = true;
        is_paused_ = false;
        return;
    }

    stop_requested_ = false;
    skip_requested_ = false;
    is_paused_ = false;
    is_playing_ = true;

    if (task_handle_ == nullptr) {
        BaseType_t ret = xTaskCreatePinnedToCore(TaskFunction, "SdMusicPlayer", 4096, this, 5, &task_handle_, 1);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed with error: %d", (int)ret);
            is_playing_ = false;
            task_handle_ = nullptr;
        }
    }
}

void SdMusicPlayer::TogglePlayPause() {
    if (!is_playing_) {
        Play(current_index_);
    } else {
        is_paused_ = !is_paused_;
        ESP_LOGI(TAG, "Playback %s", is_paused_ ? "PAUSED" : "RESUMED");
    }
}

void SdMusicPlayer::Next() {
    int next_idx = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) return;
        next_idx = (current_index_ + 1) % playlist_.size();
    }
    Play(next_idx);
}

void SdMusicPlayer::Prev() {
    int prev_idx = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) return;
        prev_idx = (current_index_ - 1 + (int)playlist_.size()) % playlist_.size();
    }
    Play(prev_idx);
}

void SdMusicPlayer::Stop() {
    if (is_playing_ || task_handle_ != nullptr) {
        stop_requested_ = true;
        is_paused_ = false;
        int timeout = 50; // max 500ms timeout
        while (is_playing_ && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout--;
        }
        if (is_playing_) {
            ESP_LOGW(TAG, "Stop timeout reached, forcing task cleanup");
            if (task_handle_ != nullptr) {
                vTaskDelete(task_handle_);
                task_handle_ = nullptr;
            }
            is_playing_ = false;
        }
        AudioManager::GetInstance().ReleaseAudioFocus(kAudioSourceMp3Player);
    }
}

void SdMusicPlayer::PlayerLoop() {
    AudioManager::GetInstance().RequestAudioFocus(kAudioSourceMp3Player);
    EnsureMp3DecoderRegistered();

    auto codec = Board::GetInstance().GetAudioCodec();
    uint32_t target_rate = codec ? codec->output_sample_rate() : 24000;
    if (target_rate == 0) target_rate = 24000;

    while (!stop_requested_) {
        std::string filepath;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (current_index_ < 0 || current_index_ >= (int)playlist_.size()) {
                break;
            }
            filepath = playlist_[current_index_];
        }

        ESP_LOGI(TAG, "▶ Playing file: %s", filepath.c_str());

        FILE* f = fopen(filepath.c_str(), "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open file: %s", filepath.c_str());
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }

        std::string lower_path = filepath;
        for (auto& c : lower_path) c = tolower((unsigned char)c);
        bool is_mp3 = (lower_path.rfind(".mp3") != std::string::npos);

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.dec_type = is_mp3 ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 : ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        esp_audio_simple_dec_handle_t dec_handle = nullptr;

        esp_err_t err = esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
        skip_requested_ = false;

        if (err == ESP_AUDIO_ERR_OK && dec_handle != nullptr) {
            ESP_LOGI(TAG, "esp_audio_simple_dec opened successfully for %s", filepath.c_str());
            size_t in_buf_size = 2048;
            size_t out_buf_size = 16384;
            uint8_t* in_buf_ptr = (uint8_t*)heap_caps_malloc(in_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!in_buf_ptr) in_buf_ptr = (uint8_t*)malloc(in_buf_size);
            uint8_t* out_pcm_buf_ptr = (uint8_t*)heap_caps_malloc(out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!out_pcm_buf_ptr) out_pcm_buf_ptr = (uint8_t*)malloc(out_buf_size);

            bool info_logged = false;
            while (!stop_requested_ && !skip_requested_) {
                auto state = Application::GetInstance().GetDeviceState();
                if (is_paused_ || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                    ESP_LOGD(TAG, "Player waiting: is_paused=%d, state=%d", (int)is_paused_, (int)state);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }

                vTaskDelay(pdMS_TO_TICKS(1));

                size_t bytes_read = fread(in_buf_ptr, 1, in_buf_size, f);
                bool is_eos = (bytes_read < in_buf_size);

                esp_audio_simple_dec_raw_t raw = {};
                raw.buffer = in_buf_ptr;
                raw.len = bytes_read;
                raw.eos = is_eos;
                raw.consumed = 0;

                while (raw.len > 0) {
                    esp_audio_simple_dec_out_t out_frame = {};
                    out_frame.buffer = out_pcm_buf_ptr;
                    out_frame.len = out_buf_size;

                    esp_err_t dec_res = esp_audio_simple_dec_process(dec_handle, &raw, &out_frame);
                    
                    if (dec_res == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        ESP_LOGW(TAG, "Decoder buffer too small, expanding from %d to %d bytes", (int)out_buf_size, (int)out_frame.needed_size);
                        size_t new_size = out_frame.needed_size > 0 ? out_frame.needed_size : out_buf_size * 2;
                        uint8_t* new_ptr = (uint8_t*)heap_caps_realloc(out_pcm_buf_ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (new_ptr) {
                            out_pcm_buf_ptr = new_ptr;
                            out_buf_size = new_size;
                        }
                        continue;
                    }

                    if (dec_res != ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(TAG, "esp_audio_simple_dec_process ret=%d, consumed=%d/%d, decoded_size=%d, needed_size=%d",
                                 (int)dec_res, (int)raw.consumed, (int)raw.len, (int)out_frame.decoded_size, (int)out_frame.needed_size);
                    }

                    if (out_frame.decoded_size > 0) {
                        esp_audio_simple_dec_info_t info = {};
                        if (esp_audio_simple_dec_get_info(dec_handle, &info) == ESP_AUDIO_ERR_OK && info.sample_rate > 0) {
                            if (!info_logged) {
                                ESP_LOGI(TAG, "Audio info: sample_rate=%u, channels=%u, bits=%u",
                                         (unsigned)info.sample_rate, (unsigned)info.channel,
                                         (unsigned)info.bits_per_sample);
                                info_logged = true;
                            }
                            size_t num_samples = out_frame.decoded_size / (info.bits_per_sample / 8);
                            PushMediaPcm(codec, reinterpret_cast<const int16_t*>(out_pcm_buf_ptr),
                                         num_samples, info.channel, info.sample_rate, target_rate,
                                         false);
                        }
                    }

                    if (dec_res != ESP_AUDIO_ERR_OK || raw.consumed == 0) {
                        break;
                    }
                    raw.buffer += raw.consumed;
                    raw.len -= raw.consumed;
                    raw.consumed = 0;
                }

                if (is_eos) break;
            }

            if (in_buf_ptr) free(in_buf_ptr);
            if (out_pcm_buf_ptr) free(out_pcm_buf_ptr);
            esp_audio_simple_dec_close(dec_handle);
        } else {
            ESP_LOGE(TAG, "esp_audio_simple_dec_open failed with ret=%d for %s", (int)err, filepath.c_str());
            fclose(f);
            vTaskDelay(pdMS_TO_TICKS(500));
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playlist_.empty()) {
                current_index_ = (current_index_ + 1) % playlist_.size();
            }
            continue;
        }

        fclose(f);

        if (stop_requested_) break;

        if (!skip_requested_) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playlist_.empty()) {
                current_index_ = (current_index_ + 1) % playlist_.size();
            } else {
                break;
            }
        }
    }

    is_playing_ = false;
    is_paused_ = false;
    AudioManager::GetInstance().ReleaseAudioFocus(kAudioSourceMp3Player);
    ESP_LOGI(TAG, "Player task stopped.");
}
