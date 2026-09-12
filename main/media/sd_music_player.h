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
    static SdMusicPlayer& GetInstance() {
        static SdMusicPlayer instance;
        return instance;
    }

    void ScanPlaylist();
    const std::vector<std::string>& GetPlaylist() const { return playlist_; }
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

    void Play(int index);
    void TogglePlayPause();
    void Next();
    void Prev();
    void Stop();

private:
    SdMusicPlayer();
    ~SdMusicPlayer();

    void PlayerLoop();
    static void TaskFunction(void* param);

    std::vector<std::string> playlist_;
    int current_index_ = 0;
    int selected_index_ = -1;
    bool shuffle_enabled_ = false;
    bool repeat_enabled_ = false;
    std::vector<int> shuffle_order_;
    int shuffle_position_ = -1;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> skip_requested_{false};

    TaskHandle_t task_handle_ = nullptr;
    mutable std::mutex mutex_;
};

#endif // SD_MUSIC_PLAYER_H
