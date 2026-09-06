#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <atomic>
#include <string>

class MediaPlayer {
public:
    static MediaPlayer& GetInstance();

    void ScanSd();
    void PlaySd(int index = -1);
    std::string ListSdTracks() const;
    int FindSdTrack(const std::string& query) const;
    std::string SearchSdTracks(const std::string& artist, const std::string& genre, int limit) const;
    void PlayRadio(const std::string& url, const std::string& title = {});
    void TogglePlayPause();
    void PauseForVoice();
    void PlayForVoice();
    void Next();
    void Prev();
    void Stop();

    bool IsPlaying() const;
    bool IsPaused() const;
    std::string GetTitle() const;
    std::string GetStatus() const;

private:
    MediaPlayer() = default;
    std::atomic<bool> paused_for_voice_{false};
    std::string radio_url_for_voice_;
    std::string radio_title_for_voice_;
};

#endif
