#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <atomic>
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
    std::atomic<bool> paused_for_voice_{false};
    std::atomic<bool> sd_paused_for_voice_{false};
    std::atomic<bool> paused_by_user_{false};
    bool generated_download_sd_paused_ = false;
    bool generated_download_radio_paused_ = false;
    RadioStationInfo radio_station_for_voice_;
    RadioStationInfo radio_station_for_manual_pause_;
    RadioStationInfo radio_station_for_generated_download_;
    mutable std::mutex voice_mutex_;
};

#endif
