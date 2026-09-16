#ifndef INTERNET_RADIO_PLAYER_H
#define INTERNET_RADIO_PLAYER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

struct RadioStationInfo {
    std::string stationuuid;
    std::string name;
    std::string url_resolved;
    std::string codec;
    uint32_t bitrate{0};
    std::string country;
    std::string countrycode;
    std::string state;
    std::string language;
    std::string tags;
};

// Streams an MP3/AAC station directly into AudioService. The player deliberately
// owns no playlist; RadioBrowser supplies station URLs to MediaPlayer.
class InternetRadioPlayer {
public:
    static InternetRadioPlayer& GetInstance();

    bool Play(const RadioStationInfo& station, std::string& err_msg,
              std::function<void()> on_startup_ready = {},
              std::function<void()> on_startup_failed = {},
              bool emit_failure_bip = true);
    bool Play(const RadioStationInfo& station);
    bool Play(const std::string& url, const std::string& title, std::string& err_msg);
    bool Play(const std::string& url, const std::string& title = {});
    void TogglePlayPause();
    void Stop();
    bool IsPlaying() const { return playing_ && !paused_; }
    bool IsPaused() const { return playing_ && paused_; }
    bool IsActive() const { return playing_ || task_handle_.load() != nullptr; }
    std::string GetTitle() const;
    std::string GetUrl() const;
    RadioStationInfo GetCurrentStation() const;
    void RecordUnderrun() { underrun_count_++; }
    uint32_t GetReconnectCount() const { return reconnect_count_.load(); }
    uint32_t GetDecoderErrorCount() const { return decoder_error_count_.load(); }
    uint32_t GetUnderrunCount() const { return underrun_count_.load(); }

private:
    struct AttemptContext {
        InternetRadioPlayer* player;
        uint64_t generation;
        std::function<void()> on_startup_ready;
        std::function<void()> on_startup_failed;
        bool emit_failure_bip;
        bool startup_ready_notified = false;
    };
    InternetRadioPlayer();
    ~InternetRadioPlayer();
    InternetRadioPlayer(const InternetRadioPlayer&) = delete;
    InternetRadioPlayer& operator=(const InternetRadioPlayer&) = delete;

    static void TaskFunction(void* arg);
    void StreamLoop(AttemptContext* attempt);

    RadioStationInfo current_station_;
    std::string url_;
    std::string title_;
    mutable std::mutex mutex_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reconnect_requested_{false};
    std::atomic<uint32_t> reconnect_count_{0};
    std::atomic<uint32_t> decoder_error_count_{0};
    std::atomic<uint32_t> underrun_count_{0};
    std::atomic<TaskHandle_t> task_handle_{nullptr};
    mutable std::mutex lifecycle_mutex_;
    EventGroupHandle_t startup_event_group_ = nullptr;
    std::atomic<bool> initial_ready_{false};
    std::atomic<bool> cleanup_complete_{true};
    std::atomic<uint64_t> active_generation_{0};
    std::atomic<uint64_t> completed_generation_{0};
    std::string startup_err_msg_;
    std::function<void()> on_startup_ready_;
    std::function<void()> on_startup_failed_;
    bool emit_failure_bip_ = true;
    bool startup_ready_notified_ = false;
    std::atomic<uint64_t> generation_{0};
};

#endif
