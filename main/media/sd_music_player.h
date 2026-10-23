#ifndef SD_MUSIC_PLAYER_H
#define SD_MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Playlist containers can grow while the UI rescans the SD card. Keep their
// backing arrays out of scarce internal RAM. The allocator falls back to the
// regular heap on targets without PSRAM so the player remains portable.
template <typename T> class MusicPsramAllocator {
public:
    using value_type = T;

    MusicPsramAllocator() noexcept = default;
    template <typename U> MusicPsramAllocator(const MusicPsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t count) {
        if (count > (static_cast<std::size_t>(-1) / sizeof(T)))
            throw std::bad_alloc();
        void* ptr = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr == nullptr)
            ptr = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_8BIT);
        if (ptr == nullptr)
            throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t) noexcept { heap_caps_free(ptr); }

    template <typename U> struct rebind { using other = MusicPsramAllocator<U>; };
};

template <typename T, typename U>
bool operator==(const MusicPsramAllocator<T>&, const MusicPsramAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const MusicPsramAllocator<T>&, const MusicPsramAllocator<U>&) noexcept {
    return false;
}

class SdMusicPlayer {
public:
    using TrackList = std::vector<std::string, MusicPsramAllocator<std::string>>;
    using ShuffleOrder = std::vector<int, MusicPsramAllocator<int>>;
    enum class PlaylistSource { Normal, Generated };
    static SdMusicPlayer& GetInstance() {
        static SdMusicPlayer instance;
        return instance;
    }

    void ScanPlaylist();
    void ScanGeneratedPlaylist();
    void EnsurePlaylist(PlaylistSource source);
    void MarkPlaylistDirty(PlaylistSource source);
    // Returns an independent snapshot. Callers must never retain a reference
    // to the internal playlist while it may be rescanned by another task.
    TrackList GetPlaylistSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return playlist_;
    }
    PlaylistSource GetPlaylistSource() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return playlist_source_;
    }
    bool IsGeneratedPlaylist() const { return GetPlaylistSource() == PlaylistSource::Generated; }
    int FindTrack(const std::string& query, bool& ambiguous) const;
    int GetCurrentTrackIndex() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_index_;
    }
    void SetSelectedTrackIndex(int index);
    int GetSelectedTrackIndex() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return selected_index_;
    }
    void SetShuffleEnabled(bool enabled);
    bool IsShuffleEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shuffle_enabled_;
    }
    void SetRepeatEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        repeat_enabled_ = enabled;
    }
    void ToggleRepeat() {
        std::lock_guard<std::mutex> lock(mutex_);
        repeat_enabled_ = !repeat_enabled_;
    }
    bool IsRepeatEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return repeat_enabled_;
    }
    int NavigateNext();
    int NavigatePrev();
    std::string GetCurrentTrackName() const;

    bool IsPlaying() const { return is_playing_ && !is_paused_; }
    bool IsPaused() const { return is_playing_ && is_paused_; }
    bool IsGeneratedPlayback() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return is_playing_ && playback_source_ == PlaylistSource::Generated;
    }

    void Play(int index);
    void TogglePlayPause();
    // Suspend PCM production for a voice interaction without closing the
    // current file or resetting the decoder position. The call waits until
    // the player task reaches a safe point between decode/output operations.
    bool PauseForVoice(std::chrono::milliseconds timeout = std::chrono::milliseconds(750));
    // Manual pause uses the same safe PCM gate, but marks the player paused so
    // the normal Play action is required to resume it.
    bool PauseForUser(std::chrono::milliseconds timeout = std::chrono::milliseconds(750));
    void ResumeFromVoice();
    void ResumeFromUser();
    void Next();
    void Prev();
    void Stop();
    bool DeleteTrack(int index, std::string& error);

private:
    SdMusicPlayer();
    ~SdMusicPlayer();

    void PlayerLoop();
    static void TaskFunction(void* param);
    void ScanDirectory(const char* directory, PlaylistSource source, bool include_wav);
    void ActivatePlaylistLocked(const TrackList& tracks, PlaylistSource source);

    TrackList playlist_;
    TrackList normal_playlist_cache_;
    TrackList generated_playlist_cache_;
    PlaylistSource playlist_source_ = PlaylistSource::Normal;
    bool normal_playlist_loaded_ = false;
    bool generated_playlist_loaded_ = false;
    bool normal_playlist_dirty_ = true;
    bool generated_playlist_dirty_ = true;
    int current_index_ = 0;
    int selected_index_ = -1;
    bool shuffle_enabled_ = false;
    bool repeat_enabled_ = false;
    ShuffleOrder shuffle_order_;
    int shuffle_position_ = -1;
    // The visible playlist can change when the user switches tabs. Keep the
    // playlist that owns the active playback separate so the current source
    // continues until the user explicitly starts another track.
    TrackList playback_playlist_;
    int playback_index_ = 0;
    std::string active_track_path_;
    PlaylistSource playback_source_ = PlaylistSource::Normal;
    bool stop_after_current_track_ = false;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_paused_{false};
    std::atomic<bool> voice_pause_requested_{false};
    bool voice_pause_acknowledged_ = false;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> skip_requested_{false};

    TaskHandle_t task_handle_ = nullptr;
    // The task waits until Play has published task_handle_ before entering
    // PlayerLoop. This closes the xTaskCreate output-parameter race.
    std::atomic<bool> task_start_pending_{false};
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    mutable std::mutex voice_pause_mutex_;
    std::condition_variable voice_pause_cv_;
    mutable std::mutex mutex_;
};

#endif // SD_MUSIC_PLAYER_H
