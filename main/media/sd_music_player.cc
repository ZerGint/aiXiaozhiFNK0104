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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>

#define TAG "SdMusicPlayer"

namespace {
constexpr char kGeneratedMusicIndex[] = "/sdcard/generated_music/library.dat";
constexpr char kGeneratedMusicIndexTmp[] = "/sdcard/generated_music/library.tmp";
constexpr size_t kMaxDecodedFrameBytes = 256 * 1024;
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
    MarkPlaylistDirty(source);
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
    auto files = StorageManager::GetInstance().ListDirectory(directory);
    std::sort(files.begin(), files.end());
    ESP_LOGI(TAG, "Scanning SD music directory %s (%d total entries)...", directory,
             static_cast<int>(files.size()));
    TrackList tracks;
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
            tracks.push_back(full_path);
            ESP_LOGI(TAG, "Track found: %s", full_path.c_str());
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (source == PlaylistSource::Generated) {
        generated_playlist_cache_ = tracks;
        generated_playlist_loaded_ = true;
        generated_playlist_dirty_ = false;
    } else {
        normal_playlist_cache_ = tracks;
        normal_playlist_loaded_ = true;
        normal_playlist_dirty_ = false;
    }
    ActivatePlaylistLocked(tracks, source);
}

void SdMusicPlayer::ActivatePlaylistLocked(const TrackList& tracks, PlaylistSource source) {
    const bool playback_active = is_playing_.load();
    const std::string active_path = active_track_path_;
    playlist_ = tracks;
    playlist_source_ = source;
    current_index_ = 0;
    selected_index_ = -1;
    // A tab refresh must not invalidate the shuffle sequence used by the
    // active playback snapshot. Rebuild it only when no playback is active.
    if (!playback_active) {
        shuffle_order_.clear();
        shuffle_position_ = -1;
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

void SdMusicPlayer::EnsurePlaylist(PlaylistSource source) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool loaded = source == PlaylistSource::Generated ? generated_playlist_loaded_
                                                                 : normal_playlist_loaded_;
        const bool dirty = source == PlaylistSource::Generated ? generated_playlist_dirty_
                                                                : normal_playlist_dirty_;
        if (loaded && !dirty) {
            if (playlist_source_ != source) {
                const auto& cache = source == PlaylistSource::Generated
                                        ? generated_playlist_cache_
                                        : normal_playlist_cache_;
                ActivatePlaylistLocked(cache, source);
            }
            return;
        }
    }
    if (source == PlaylistSource::Generated)
        ScanGeneratedPlaylist();
    else
        ScanPlaylist();
}

void SdMusicPlayer::MarkPlaylistDirty(PlaylistSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == PlaylistSource::Generated)
        generated_playlist_dirty_ = true;
    else
        normal_playlist_dirty_ = true;
}

void SdMusicPlayer::ScanPlaylist() {
    const char* paths[] = {"/sdcard/mp3", "/sdcard/music", "/sdcard/Music", "/sdcard/MUSIC",
                           "/sdcard"};
    for (auto path : paths) {
        if (strcmp(path, "/sdcard") != 0 &&
            !StorageManager::GetInstance().EnsureDirectory(path)) {
            continue;
        }
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
    if (!StorageManager::GetInstance().EnsureDirectory("/sdcard/generated_music")) {
        ESP_LOGW(TAG, "ScanGeneratedPlaylist skipped: generated directory unavailable");
        return;
    }
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
    const size_t query_slash = needle.find_last_of("/\\");
    if (query_slash != std::string::npos)
        needle.erase(0, query_slash + 1);
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
    while (player->task_start_pending_.load(std::memory_order_acquire)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    player->PlayerLoop();
    player->is_playing_ = false;
    {
        std::lock_guard<std::mutex> lock(player->lifecycle_mutex_);
        if (player->task_handle_ == xTaskGetCurrentTaskHandle()) {
            player->task_handle_ = nullptr;
        }
    }
    player->lifecycle_cv_.notify_all();
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

    // An explicit track selection cancels either kind of pause. Voice resume
    // uses ResumeFromVoice directly and therefore keeps its ownership rules.
    voice_pause_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(voice_pause_mutex_);
        voice_pause_acknowledged_ = false;
    }
    voice_pause_cv_.notify_all();

    if (is_playing_) {
        skip_requested_ = true;
        is_paused_ = false;
        return;
    }

    stop_requested_ = false;
    skip_requested_ = false;
    is_paused_ = false;

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (task_handle_ == nullptr) {
        is_playing_ = true;
        task_start_pending_.store(true, std::memory_order_release);
        TaskHandle_t new_task_handle = nullptr;
        BaseType_t ret = xTaskCreatePinnedToCore(TaskFunction, "SdMusicPlayer", 4096, this, 5,
                                                 &new_task_handle, 1);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed with error: %d", (int)ret);
            task_start_pending_.store(false, std::memory_order_release);
            is_playing_ = false;
            task_handle_ = nullptr;
        } else {
            task_handle_ = new_task_handle;
            task_start_pending_.store(false, std::memory_order_release);
            xTaskNotifyGive(new_task_handle);
        }
    } else {
        ESP_LOGW(TAG, "Playback task is still stopping; new track was not started");
    }
}

void SdMusicPlayer::TogglePlayPause() {
    if (!is_playing_) {
        Play(current_index_);
    } else if (is_paused_) {
        ResumeFromUser();
    } else {
        PauseForUser();
    }
}

bool SdMusicPlayer::PauseForVoice(std::chrono::milliseconds timeout) {
    if (!is_playing_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(voice_pause_mutex_);
        if (!voice_pause_requested_.load(std::memory_order_acquire)) {
            voice_pause_acknowledged_ = false;
            voice_pause_requested_.store(true, std::memory_order_release);
        } else if (voice_pause_acknowledged_) {
            return true;
        }
    }
    voice_pause_cv_.notify_all();

    std::unique_lock<std::mutex> lock(voice_pause_mutex_);
    const bool acknowledged = voice_pause_cv_.wait_for(lock, timeout, [this]() {
        return voice_pause_acknowledged_ || !is_playing_.load(std::memory_order_acquire);
    });
    if (!acknowledged) {
        // Keep the gate asserted on timeout. The player task will acknowledge
        // it as soon as its current decode/output call returns, and ResumeFromVoice
        // will release it after the voice interaction has fully ended.
        ESP_LOGW(TAG, "VOICE_PAUSE_ACK_TIMEOUT; keeping SD PCM gate closed");
    } else if (voice_pause_acknowledged_) {
        ESP_LOGI(TAG, "VOICE_PAUSE_ACK");
    }
    return true;
}

bool SdMusicPlayer::PauseForUser(std::chrono::milliseconds timeout) {
    if (!PauseForVoice(timeout)) {
        return false;
    }

    is_paused_.store(true, std::memory_order_release);
    // Discard PCM already queued for the media source. The file and decoder
    // remain open, so the next Play action continues from the saved position.
    // Speech and system sounds stay queued.
    Application::GetInstance().GetAudioService().DiscardMediaPlayback("sd_user_pause");
    ESP_LOGI(TAG, "Playback PAUSED");
    return true;
}

void SdMusicPlayer::ResumeFromVoice() {
    if (!voice_pause_requested_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(voice_pause_mutex_);
        voice_pause_acknowledged_ = false;
    }
    voice_pause_cv_.notify_all();
    ESP_LOGI(TAG, "VOICE_RESUME_GATE_OPEN");
}

void SdMusicPlayer::ResumeFromUser() {
    is_paused_.store(false, std::memory_order_release);
    ResumeFromVoice();
    ESP_LOGI(TAG, "Playback RESUMED");
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
    Application::GetInstance().GetAudioService().CheckHeapIntegrity("stop_sd_begin");
    voice_pause_requested_.store(false, std::memory_order_release);
    voice_pause_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_track_path_.clear();
        playback_playlist_.clear();
        playback_index_ = 0;
        stop_after_current_track_ = false;
    }
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        task = task_handle_;
    }
    if (is_playing_ || task != nullptr) {
        stop_requested_ = true;
        is_paused_ = false;
        ESP_LOGI(TAG, "AUDIO_SOURCE_STOP_REQUEST source=sd");
        Application::GetInstance().GetAudioService().DiscardMediaPlayback("sd_stop");

        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        const bool stopped = lifecycle_cv_.wait_for(lock, std::chrono::seconds(3), [this]() {
            return task_handle_ == nullptr && !is_playing_.load();
        });
        if (!stopped) {
            ESP_LOGE(TAG, "Stop timeout: playback task remains active; task was not deleted");
        }

        lock.unlock();
        if (!Application::GetInstance().GetAudioService().WaitForMediaDrained(
                std::chrono::seconds(3))) {
            ESP_LOGE(TAG, "Stop timeout: media playback task remains in flight");
        }
        ESP_LOGI(TAG, "AUDIO_SOURCE_STOP_COMPLETE source=sd drained=%d",
                 Application::GetInstance().GetAudioService().IsPlaybackIdle() ? 1 : 0);
    }
    Application::GetInstance().GetAudioService().CheckHeapIntegrity("stop_sd_end");
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
        if (voice_pause_requested_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(voice_pause_mutex_);
                voice_pause_acknowledged_ = true;
            }
            voice_pause_cv_.notify_all();
            while (voice_pause_requested_.load(std::memory_order_acquire) &&
                   !stop_requested_.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

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
            if (stop_requested_ || skip_requested_) {
                esp_audio_simple_dec_close(dec_handle);
                fclose(f);
                break;
            }
            uint32_t stream_generation = BeginMediaPcmStream();
            if (stop_requested_ || skip_requested_) {
                esp_audio_simple_dec_close(dec_handle);
                fclose(f);
                break;
            }
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

            if (!in_buf_ptr || !out_pcm_buf_ptr) {
                ESP_LOGE(TAG, "Audio buffer allocation failed: input=%p output=%p",
                         in_buf_ptr, out_pcm_buf_ptr);
                if (in_buf_ptr)
                    free(in_buf_ptr);
                if (out_pcm_buf_ptr)
                    free(out_pcm_buf_ptr);
                esp_audio_simple_dec_close(dec_handle);
                fclose(f);
                stop_requested_ = true;
                break;
            }

            bool info_logged = false;
            bool pcm_checkpoint_logged = false;
            while (!stop_requested_ && !skip_requested_) {
                if (voice_pause_requested_.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard<std::mutex> lock(voice_pause_mutex_);
                        voice_pause_acknowledged_ = true;
                    }
                    voice_pause_cv_.notify_all();
                    while (voice_pause_requested_.load(std::memory_order_acquire) &&
                           !stop_requested_.load(std::memory_order_acquire)) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    continue;
                }

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

                while (raw.len > 0 && !stop_requested_ && !skip_requested_) {
                    esp_audio_simple_dec_out_t out_frame = {};
                    out_frame.buffer = out_pcm_buf_ptr;
                    out_frame.len = out_buf_size;

                    esp_err_t dec_res = esp_audio_simple_dec_process(dec_handle, &raw, &out_frame);

                    if (dec_res == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                        ESP_LOGW(TAG, "Decoder buffer too small, expanding from %d to %d bytes",
                                 (int)out_buf_size, (int)out_frame.needed_size);
                        size_t new_size =
                            out_frame.needed_size > 0 ? out_frame.needed_size : out_buf_size * 2;
                        if (new_size <= out_buf_size || new_size > kMaxDecodedFrameBytes) {
                            ESP_LOGE(TAG, "Decoder requested invalid output size=%u current=%u",
                                     static_cast<unsigned>(new_size),
                                     static_cast<unsigned>(out_buf_size));
                            stop_requested_ = true;
                            break;
                        }
                        uint8_t* new_ptr = (uint8_t*)heap_caps_realloc(
                            out_pcm_buf_ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (new_ptr) {
                            out_pcm_buf_ptr = new_ptr;
                            out_buf_size = new_size;
                        } else {
                            ESP_LOGE(TAG, "Decoder output buffer expansion failed: size=%u",
                                     static_cast<unsigned>(new_size));
                            stop_requested_ = true;
                            break;
                        }
                        continue;
                    }

                    if (raw.consumed > raw.len) {
                        ESP_LOGE(TAG, "Decoder returned invalid consumed=%u input=%u",
                                 static_cast<unsigned>(raw.consumed),
                                 static_cast<unsigned>(raw.len));
                        stop_requested_ = true;
                        break;
                    }

                    if (dec_res != ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(TAG,
                                 "esp_audio_simple_dec_process ret=%d, consumed=%d/%d, "
                                 "decoded_size=%d, needed_size=%d",
                                 (int)dec_res, (int)raw.consumed, (int)raw.len,
                                 (int)out_frame.decoded_size, (int)out_frame.needed_size);
                    }

                    if (out_frame.decoded_size > 0 &&
                        (out_frame.decoded_size & 1U) == 0 &&
                        out_frame.decoded_size <= out_buf_size) {
                        // A voice pause or decoder reset discards queued media
                        // and advances the media generation. The SD decoder
                        // remains open across that reset, so bind this live
                        // stream to the new generation instead of dropping
                        // every frame after the voice interaction.
                        auto& audio_service = Application::GetInstance().GetAudioService();
                        if (audio_service.GetMediaGeneration() != stream_generation) {
                            stream_generation = BeginMediaPcmStream();
                            if (stop_requested_ || skip_requested_)
                                break;
                        }
                        esp_audio_simple_dec_info_t info = {};
                        if (esp_audio_simple_dec_get_info(dec_handle, &info) == ESP_AUDIO_ERR_OK &&
                            info.sample_rate > 0 && info.bits_per_sample == 16 &&
                            (info.channel == 1 || info.channel == 2)) {
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
                                         false, stream_generation);
                        }
                    } else if (out_frame.decoded_size > 0) {
                        ESP_LOGW(TAG, "Dropping invalid decoded PCM size=%d buffer=%u",
                                 static_cast<int>(out_frame.decoded_size),
                                 static_cast<unsigned>(out_buf_size));
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
    voice_pause_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(voice_pause_mutex_);
        voice_pause_acknowledged_ = false;
    }
    voice_pause_cv_.notify_all();
    AudioManager::GetInstance().ReleaseAudioFocus(kAudioSourceMp3Player);
    ESP_LOGI(TAG, "Player task stopped.");
}
