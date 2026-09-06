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
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> skip_requested_{false};

    TaskHandle_t task_handle_ = nullptr;
    mutable std::mutex mutex_;
};

#endif // SD_MUSIC_PLAYER_H
