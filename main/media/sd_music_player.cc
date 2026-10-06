#include "sd_music_player.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_manager.h"
#include "board.h"
#include "media/media_audio_output.h"
#include "storage_manager.h"
#include "music/generated_music_storage.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <decoder/impl/esp_mp3_dec.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>

#define TAG "SdMusicPlayer"

namespace {
constexpr char kGeneratedMusicIndex[] = "/sdcard/generated_music/library.dat";
constexpr char kGeneratedMusicIndexTmp[] = "/sdcard/generated_music/library.tmp";
void CheckMusicHeap(const char* checkpoint) {
    const bool ok = heap_caps_check_integrity_all(true);
    ESP_LOGW(TAG, "HEAP_CHECK %s: %s", checkpoint, ok ? "PASS" : "FAIL");
}

void LogMusicRuntime(const char* checkpoint) {
    CheckMusicHeap(checkpoint);
    ESP_LOGW(TAG, "MUSIC_CHECKPOINT %s task=%s internal_free=%u internal_largest=%u stack_hwm=%u",
             checkpoint, pcTaskGetName(nullptr),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
}  // namespace

bool SdMusicPlayer::DeleteTrack(int index, std::string& error) {
    error.clear();
    std::string path;
    PlaylistSource source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(playlist_.size())) { error = "track_not_found"; return false; }
        path = playlist_[index]; source = playlist_source_;
        if (is_playing_ && active_track_path_ == path) { error = "track_is_playing"; return false; }
    }
    if (unlink(path.c_str()) != 0) { error = "track_delete_failed"; return false; }
    if (source == PlaylistSource::Generated) {
        std::vector<GeneratedMusicRecord> records;
        if (!GeneratedMusicStorage::Load(kGeneratedMusicIndex, records, error)) return false;
        const std::string filename = path.substr(path.find_last_of('/') + 1);
        records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& r) { return r.filename == filename; }), records.end());
        if (!GeneratedMusicStorage::SaveAtomic(kGeneratedMusicIndex, kGeneratedMusicIndexTmp, records, error)) return false;
        ScanGeneratedPlaylist();
    } else ScanPlaylist();
    ESP_LOGI(TAG, "Track deleted: %s", path.c_str());
    return true;
}

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

SdMusicPlayer::SdMusicPlayer() {}

SdMusicPlayer::~SdMusicPlayer() { Stop(); }

void SdMusicPlayer::ScanDirectory(const char* directory, PlaylistSource source, bool include_wav) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool playback_active = is_playing_.load();
    const std::string active_path = active_track_path_;
    playlist_.clear();
    playlist_source_ = source;
    current_index_ = 0;
    selected_index_ = -1;
    // A tab refresh must not invalidate the shuffle sequence used by the
    // active playback snapshot. Rebuild it only when no playback is active.
    if (!playback_active) {
        shuffle_order_.clear();
        shuffle_position_ = -1;
    }

    auto files = StorageManager::GetInstance().ListDirectory(directory);
    std::sort(files.begin(), files.end());
    ESP_LOGI(TAG, "Scanning SD music directory %s (%d total entries)...", directory,
             static_cast<int>(files.size()));
    for (const auto& name : files) {
        std::string lower_name = name;
        for (auto& c : lower_name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool is_mp3 = lower_name.size() >= 4 &&
                            lower_name.compare(lower_name.size() - 4, 4, ".mp3") == 0;
        const bool is_wav = lower_name.size() >= 4 &&
                            lower_name.compare(lower_name.size() - 4, 4, ".wav") == 0;
        if (is_mp3 || (include_wav && is_wav)) {
            std::string full_path = std::string(directory) + "/" + name;
            playlist_.push_back(full_path);
            ESP_LOGI(TAG, "Track found: %s", full_path.c_str());
        }
    }
    if (playback_active && !active_path.empty()) {
        const auto active_it = std::find(playlist_.begin(), playlist_.end(), active_path);
        if (active_it != playlist_.end()) {
            current_index_ = static_cast<int>(active_it - playlist_.begin());
            selected_index_ = current_index_;
        }
        // Keep playback_playlist_ and playback_source_ unchanged. The tab
        // switch only changes the list shown by the browser.
    }
}

void SdMusicPlayer::ScanPlaylist() {
    const char* paths[] = {"/sdcard/mp3", "/sdcard/music", "/sdcard/Music", "/sdcard/MUSIC",
                           "/sdcard"};
    for (auto path : paths) {
        auto files = StorageManager::GetInstance().ListDirectory(path);
        if (files.empty())
            continue;
        ScanDirectory(path, PlaylistSource::Normal, true);
        if (!playlist_.empty())
            break;
    }
    ESP_LOGI(TAG, "ScanPlaylist complete. Total tracks found: %d", (int)playlist_.size());
}

void SdMusicPlayer::ScanGeneratedPlaylist() {
    ScanDirectory("/sdcard/generated_music", PlaylistSource::Generated, false);
    ESP_LOGI(TAG, "ScanGeneratedPlaylist complete. Total tracks found: %d",
             static_cast<int>(playlist_.size()));
}

int SdMusicPlayer::FindTrack(const std::string& query, bool& ambiguous) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ambiguous = false;
    if (query.empty() || playlist_source_ != PlaylistSource::Generated)
        return -1;

    std::string needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    int contains_match = -1;
    int contains_count = 0;
    for (size_t i = 0; i < playlist_.size(); ++i) {
        std::string title = playlist_[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos)
            title.erase(0, slash + 1);
        std::string lower_title = title;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_title == needle)
            return static_cast<int>(i);
        if (lower_title.find(needle) != std::string::npos) {
            contains_match = static_cast<int>(i);
            ++contains_count;
        }
    }
    if (contains_count == 1)
        return contains_match;
    ambiguous = contains_count > 1;
    return -1;
}
std::string SdMusicPlayer::GetCurrentTrackName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_playing_ && !active_track_path_.empty()) {
        const size_t last_slash = active_track_path_.find_last_of('/');
        return last_slash == std::string::npos ? active_track_path_
                                               : active_track_path_.substr(last_slash + 1);
    }
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

void SdMusicPlayer::SetSelectedTrackIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playlist_.empty()) {
        selected_index_ = -1;
        return;
    }
    if (index < 0)
        index = 0;
    if (index >= (int)playlist_.size())
        index = (int)playlist_.size() - 1;
    selected_index_ = index;
    if (shuffle_enabled_) {
        auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), index);
        if (it != shuffle_order_.end())
            shuffle_position_ = static_cast<int>(it - shuffle_order_.begin());
    }
}

void SdMusicPlayer::SetShuffleEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    shuffle_enabled_ = enabled;
    shuffle_order_.clear();
    shuffle_position_ = -1;
    if (!enabled || playlist_.empty())
        return;
    for (int i = 0; i < static_cast<int>(playlist_.size()); ++i)
        shuffle_order_.push_back(i);
    int current = selected_index_;
    if (current < 0 || current >= static_cast<int>(playlist_.size()))
        current = 0;
    std::mt19937 rng(std::random_device{}());
    std::shuffle(shuffle_order_.begin(), shuffle_order_.end(), rng);
    auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), current);
    std::iter_swap(shuffle_order_.begin(), it);
    selected_index_ = current;
    shuffle_position_ = 0;
}

int SdMusicPlayer::NavigateNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playlist_.empty())
        return -1;
    if (shuffle_enabled_ && shuffle_order_.size() == playlist_.size()) {
        shuffle_position_ = (shuffle_position_ + 1) % shuffle_order_.size();
        selected_index_ = shuffle_order_[shuffle_position_];
    } else {
        shuffle_enabled_ = false;
        selected_index_ = (selected_index_ < 0 ? 0 : (selected_index_ + 1) % playlist_.size());
    }
    return selected_index_;
}
int SdMusicPlayer::NavigatePrev() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playlist_.empty())
        return -1;
    if (shuffle_enabled_ && shuffle_order_.size() == playlist_.size()) {
        shuffle_position_ = (shuffle_position_ - 1 + shuffle_order_.size()) % shuffle_order_.size();
        selected_index_ = shuffle_order_[shuffle_position_];
    } else {
        shuffle_enabled_ = false;
        selected_index_ = (selected_index_ <= 0 ? playlist_.size() - 1 : selected_index_ - 1);
    }
    return selected_index_;
}

void SdMusicPlayer::Play(int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playlist_.empty()) {
            ESP_LOGW(TAG, "Cannot play: playlist is empty");
            return;
        }
        if (index < 0)
            index = 0;
        if (index >= (int)playlist_.size())
            index = 0;
        current_index_ = index;
        selected_index_ = index;
        playback_playlist_ = playlist_;
        playback_index_ = index;
        active_track_path_ = playlist_[index];
        playback_source_ = playlist_source_;
        stop_after_current_track_ = false;
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
        BaseType_t ret =
            xTaskCreatePinnedToCore(TaskFunction, "SdMusicPlayer", 4096, this, 5, &task_handle_, 1);
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
    int next_idx = NavigateNext();
    if (next_idx < 0)
        return;
    Play(next_idx);
}

void SdMusicPlayer::Prev() {
    int prev_idx = NavigatePrev();
    if (prev_idx < 0)
        return;
    Play(prev_idx);
}

void SdMusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_track_path_.clear();
        playback_playlist_.clear();
        playback_index_ = 0;
        stop_after_current_track_ = false;
    }
    if (is_playing_ || task_handle_ != nullptr) {
        stop_requested_ = true;
        is_paused_ = false;
        int timeout = 50;  // max 500ms timeout
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
    LogMusicRuntime("PLAYER_LOOP_ENTER");
    AudioManager::GetInstance().RequestAudioFocus(kAudioSourceMp3Player);
    LogMusicRuntime("AFTER_AUDIO_FOCUS");
    EnsureMp3DecoderRegistered();
    LogMusicRuntime("AFTER_DECODER_REGISTER");

    auto codec = Board::GetInstance().GetAudioCodec();
    uint32_t target_rate = codec ? codec->output_sample_rate() : 24000;
    if (target_rate == 0)
        target_rate = 24000;

    while (!stop_requested_) {
        std::string filepath;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_track_path_.empty()) {
                filepath = active_track_path_;
            } else if (playback_index_ >= 0 &&
                       playback_index_ < static_cast<int>(playback_playlist_.size())) {
                filepath = playback_playlist_[playback_index_];
            } else if (current_index_ >= 0 && current_index_ < (int)playlist_.size()) {
                filepath = playlist_[current_index_];
            } else {
                break;
            }
        }

        ESP_LOGI(TAG, "▶ Playing file: %s", filepath.c_str());

        LogMusicRuntime("BEFORE_FILE_OPEN");
        FILE* f = fopen(filepath.c_str(), "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open file: %s", filepath.c_str());
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
        LogMusicRuntime("AFTER_FILE_OPEN");

        std::string lower_path = filepath;
        for (auto& c : lower_path)
            c = tolower((unsigned char)c);
        bool is_mp3 = (lower_path.rfind(".mp3") != std::string::npos);

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.dec_type = is_mp3 ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 : ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        esp_audio_simple_dec_handle_t dec_handle = nullptr;

        esp_err_t err = esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
        skip_requested_ = false;

        if (err == ESP_AUDIO_ERR_OK && dec_handle != nullptr) {
            BeginMediaPcmStream();
            ESP_LOGI(TAG, "esp_audio_simple_dec opened successfully for %s", filepath.c_str());
            LogMusicRuntime("AFTER_DECODER_OPEN");
            size_t in_buf_size = 2048;
            size_t out_buf_size = 16384;
            uint8_t* in_buf_ptr =
                (uint8_t*)heap_caps_malloc(in_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!in_buf_ptr)
                in_buf_ptr = (uint8_t*)malloc(in_buf_size);
            uint8_t* out_pcm_buf_ptr =
                (uint8_t*)heap_caps_malloc(out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!out_pcm_buf_ptr)
                out_pcm_buf_ptr = (uint8_t*)malloc(out_buf_size);

            bool info_logged = false;
            bool pcm_checkpoint_logged = false;
            while (!stop_requested_ && !skip_requested_) {
                auto state = Application::GetInstance().GetDeviceState();
                if (is_paused_ || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                    ESP_LOGD(TAG, "Player waiting: is_paused=%d, state=%d", (int)is_paused_,
                             (int)state);
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
                        ESP_LOGW(TAG, "Decoder buffer too small, expanding from %d to %d bytes",
                                 (int)out_buf_size, (int)out_frame.needed_size);
                        size_t new_size =
                            out_frame.needed_size > 0 ? out_frame.needed_size : out_buf_size * 2;
                        uint8_t* new_ptr = (uint8_t*)heap_caps_realloc(
                            out_pcm_buf_ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (new_ptr) {
                            out_pcm_buf_ptr = new_ptr;
                            out_buf_size = new_size;
                        }
                        continue;
                    }

                    if (dec_res != ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(TAG,
                                 "esp_audio_simple_dec_process ret=%d, consumed=%d/%d, "
                                 "decoded_size=%d, needed_size=%d",
                                 (int)dec_res, (int)raw.consumed, (int)raw.len,
                                 (int)out_frame.decoded_size, (int)out_frame.needed_size);
                    }

                    if (out_frame.decoded_size > 0) {
                        esp_audio_simple_dec_info_t info = {};
                        if (esp_audio_simple_dec_get_info(dec_handle, &info) == ESP_AUDIO_ERR_OK &&
                            info.sample_rate > 0) {
                            if (!info_logged) {
                                ESP_LOGI(TAG, "Audio info: sample_rate=%u, channels=%u, bits=%u",
                                         (unsigned)info.sample_rate, (unsigned)info.channel,
                                         (unsigned)info.bits_per_sample);
                                info_logged = true;
                            }
                            if (!pcm_checkpoint_logged) {
                                LogMusicRuntime("BEFORE_FIRST_PCM");
                                pcm_checkpoint_logged = true;
                            }
                            size_t num_samples =
                                out_frame.decoded_size / (info.bits_per_sample / 8);
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

                if (is_eos)
                    break;
            }

            if (in_buf_ptr)
                free(in_buf_ptr);
            if (out_pcm_buf_ptr)
                free(out_pcm_buf_ptr);
            esp_audio_simple_dec_close(dec_handle);
        } else {
            ESP_LOGE(TAG, "esp_audio_simple_dec_open failed with ret=%d for %s", (int)err,
                     filepath.c_str());
            fclose(f);
            vTaskDelay(pdMS_TO_TICKS(500));
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playback_playlist_.empty()) {
                playback_index_ = (playback_index_ + 1) % playback_playlist_.size();
                active_track_path_ = playback_playlist_[playback_index_];
            }
            continue;
        }

        fclose(f);

        if (stop_requested_)
            break;

        if (!skip_requested_) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!playback_playlist_.empty()) {
                if (shuffle_enabled_ && shuffle_order_.size() == playback_playlist_.size()) {
                    if (shuffle_position_ + 1 >= static_cast<int>(shuffle_order_.size())) {
                        if (!repeat_enabled_) {
                            stop_requested_ = true;
                            continue;
                        }
                        shuffle_position_ = 0;
                    } else {
                        ++shuffle_position_;
                    }
                    playback_index_ = shuffle_order_[shuffle_position_];
                } else if (!shuffle_enabled_ && repeat_enabled_ &&
                           playback_index_ + 1 >= static_cast<int>(playback_playlist_.size())) {
                    playback_index_ = 0;
                } else if (!shuffle_enabled_ &&
                           playback_index_ + 1 >= static_cast<int>(playback_playlist_.size())) {
                    stop_requested_ = true;
                    continue;
                } else {
                    ++playback_index_;
                }
                active_track_path_ = playback_playlist_[playback_index_];
                const auto visible_it =
                    std::find(playlist_.begin(), playlist_.end(), active_track_path_);
                if (visible_it != playlist_.end()) {
                    current_index_ = static_cast<int>(visible_it - playlist_.begin());
                    selected_index_ = current_index_;
                }
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
