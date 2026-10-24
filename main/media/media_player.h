#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <functional>
#include <mutex>

#include "internet_radio_player.h"

class MediaPlayer {
public:
    static MediaPlayer& GetInstance();

    void ScanSd();
    bool PrepareGeneratedPlaylist(std::string& err_msg);
    bool PrepareSdPlaylist(std::string& err_msg);
    void PlaySd(int index = -1);
    bool PlayGenerated(const std::string& query, std::string& err_msg);
    std::string ListSdTracks() const;
    int FindSdTrack(const std::string& query) const;
    std::string SearchSdTracks(const std::string& artist, const std::string& genre, int limit) const;
    bool PlayRadio(const RadioStationInfo& station, std::string& err_msg,
                   std::function<void()> on_startup_ready = {},
                   std::function<void()> on_startup_failed = {},
                   bool emit_failure_bip = true);
    bool PlayRadioByUuid(const std::string& station_uuid, const std::string& title = {},
                         std::function<void(const RadioStationInfo&)> on_startup_ready = {},
                         std::function<void()> on_startup_failed = {},
                         bool emit_failure_bip = true);
    bool PlayRadio(const RadioStationInfo& station);
    bool PlayRadio(const std::string& url, const std::string& title, std::string& err_msg);
    bool PlayRadio(const std::string& url, const std::string& title = {});
    void TogglePlayPause();
    void PauseForVoice();
    void PlayForVoice();
    // Pause the currently active local source for a generated-music download.
    // This has separate ownership from voice interruption and restores only
    // playback that was actually running when the transaction began.
    void PauseForGeneratedDownload();
    void ResumeAfterGeneratedDownload();
    void Next();
    void Prev();
    void Stop();

    bool IsPlaying() const;
    bool HasRadioPausedByUser() const;
    bool GetRadioStationPausedByUser(RadioStationInfo& station) const;
    bool GetCurrentRadioStationForAction(RadioStationInfo& station) const;
    bool GetRadioStationPausedForVoice(RadioStationInfo& station) const;
    bool IsPaused() const;
    std::string GetTitle() const;
    std::string GetStatus() const;

private:
    MediaPlayer() = default;

    enum class ControlCommandType {
        PlaySd,
        PlayRadio,
        PlayRadioByUuid,
        Stop,
        TogglePlayPause,
        PauseForVoice,
        PlayForVoice,
        PauseForGeneratedDownload,
        ResumeAfterGeneratedDownload,
        Next,
        Prev,
    };

    struct ControlCommand {
        ControlCommandType type;
        uint64_t sequence = 0;
        int sd_index = -1;
        RadioStationInfo station;
        std::function<void(const RadioStationInfo&)> on_station_resolved;
        std::function<void()> on_startup_ready;
        std::function<void()> on_startup_failed;
        bool emit_failure_bip = true;
        bool restore_voice_pause = false;
        bool restore_manual_pause = false;
        RadioStationInfo voice_station;
        RadioStationInfo manual_station;
    };

    static void ControlTaskFunction(void* arg);
    void EnsureControlTask();
    bool EnqueueControlCommand(ControlCommand&& command, bool replace_pending_source);
    bool IsLatestSourceCommand(uint64_t sequence) const;
    void ProcessControlCommand(ControlCommand&& command);
    bool StopVoiceInteractionForMediaSynchronously();

    void PlaySdNow(int index, uint64_t sequence);
    void PlayRadioNow(ControlCommand&& command);
    void PlayRadioByUuidNow(ControlCommand&& command);
    void StopNow();
    void TogglePlayPauseNow();
    void PauseForVoiceNow();
    void PlayForVoiceNow();
    void PauseForGeneratedDownloadNow();
    void ResumeAfterGeneratedDownloadNow();
    void NextNow();
    void PrevNow();

    std::atomic<bool> paused_for_voice_{false};
    std::atomic<bool> sd_paused_for_voice_{false};
    std::atomic<bool> paused_by_user_{false};
    bool generated_download_sd_paused_ = false;
    bool generated_download_radio_paused_ = false;
    RadioStationInfo radio_station_for_voice_;
    RadioStationInfo radio_station_for_manual_pause_;
    RadioStationInfo radio_station_for_generated_download_;
    mutable std::mutex voice_mutex_;

    mutable std::mutex control_mutex_;
    std::condition_variable control_cv_;
    std::deque<ControlCommand> control_queue_;
    TaskHandle_t control_task_handle_ = nullptr;
    std::atomic<uint64_t> control_sequence_{0};
    std::atomic<uint64_t> latest_source_sequence_{0};
};

#endif
