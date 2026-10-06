#ifndef SD_MUSIC_PLAYER_H
#define SD_MUSIC_PLAYER_H

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class SdMusicPlayer {
public:
    enum class PlaylistSource { Normal, Generated };
    static SdMusicPlayer& GetInstance() {
        static SdMusicPlayer instance;
        return instance;
    }

    void ScanPlaylist();
    void ScanGeneratedPlaylist();
    const std::vector<std::string>& GetPlaylist() const { return playlist_; }
    PlaylistSource GetPlaylistSource() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return playlist_source_;
    }
    bool IsGeneratedPlaylist() const { return GetPlaylistSource() == PlaylistSource::Generated; }
    int FindTrack(const std::string& query, bool& ambiguous) const;
    int GetCurrentTrackIndex() const { return current_index_; }
    void SetSelectedTrackIndex(int index);
    int GetSelectedTrackIndex() const { return selected_index_; }
    void SetShuffleEnabled(bool enabled);
    bool IsShuffleEnabled() const { return shuffle_enabled_; }
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

    std::vector<std::string> playlist_;
    PlaylistSource playlist_source_ = PlaylistSource::Normal;
    int current_index_ = 0;
    int selected_index_ = -1;
    bool shuffle_enabled_ = false;
    bool repeat_enabled_ = false;
    std::vector<int> shuffle_order_;
    int shuffle_position_ = -1;
    // The visible playlist can change when the user switches tabs. Keep the
    // playlist that owns the active playback separate so the current source
    // continues until the user explicitly starts another track.
    std::vector<std::string> playback_playlist_;
    int playback_index_ = 0;
    std::string active_track_path_;
    PlaylistSource playback_source_ = PlaylistSource::Normal;
    bool stop_after_current_track_ = false;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> skip_requested_{false};

    TaskHandle_t task_handle_ = nullptr;
    mutable std::mutex mutex_;
};

#endif // SD_MUSIC_PLAYER_H
