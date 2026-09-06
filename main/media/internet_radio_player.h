#ifndef INTERNET_RADIO_PLAYER_H
#define INTERNET_RADIO_PLAYER_H

#include <atomic>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Streams an MP3 station directly into AudioService.  The player deliberately
// owns no playlist; RadioBrowser supplies station URLs to MediaPlayer.
class InternetRadioPlayer {
public:
    static InternetRadioPlayer& GetInstance();

    void Play(const std::string& url, const std::string& title = {});
    void TogglePlayPause();
    void Stop();
    bool IsPlaying() const { return playing_ && !paused_; }
    bool IsPaused() const { return playing_ && paused_; }
    bool IsActive() const { return playing_ || task_handle_ != nullptr; }
    std::string GetTitle() const;
    std::string GetUrl() const;

private:
    InternetRadioPlayer() = default;
    ~InternetRadioPlayer();
    InternetRadioPlayer(const InternetRadioPlayer&) = delete;
    InternetRadioPlayer& operator=(const InternetRadioPlayer&) = delete;

    static void TaskFunction(void* arg);
    void StreamLoop();

    std::string url_;
    std::string title_;
    mutable std::mutex mutex_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reconnect_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
};

#endif
