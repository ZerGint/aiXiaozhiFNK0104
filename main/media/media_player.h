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
    void PlaySd(int index = -1);
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
    std::atomic<bool> paused_by_user_{false};
    RadioStationInfo radio_station_for_voice_;
    RadioStationInfo radio_station_for_manual_pause_;
    mutable std::mutex voice_mutex_;
};

#endif
