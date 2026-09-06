#include "media_player.h"

#include "application.h"
#include "sd_music_player.h"
#include "internet_radio_player.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <cJSON.h>

MediaPlayer& MediaPlayer::GetInstance() {
    static MediaPlayer instance;
    return instance;
}

void MediaPlayer::ScanSd() {
    SdMusicPlayer::GetInstance().ScanPlaylist();
}

void MediaPlayer::PlaySd(int index) {
    paused_for_voice_ = false;
    auto& application = Application::GetInstance();
    application.StopVoiceInteractionForMedia();
    InternetRadioPlayer::GetInstance().Stop();
    auto& player = SdMusicPlayer::GetInstance();
    player.Play(index < 0 ? player.GetCurrentTrackIndex() : index);
}

std::string MediaPlayer::ListSdTracks() const {
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylist();
    cJSON* result = cJSON_CreateArray();
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos) title.erase(0, slash + 1);
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", static_cast<double>(i + 1));
        cJSON_AddStringToObject(item, "title", title.c_str());
        cJSON_AddItemToArray(result, item);
    }
    char* output = cJSON_PrintUnformatted(result);
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(result);
    return response;
}

std::string MediaPlayer::SearchSdTracks(const std::string& artist, const std::string& genre, int limit) const {
    limit = std::max(1, std::min(limit, 20));
    cJSON* result = cJSON_CreateArray();
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylist();
    for (size_t i = 0; i < tracks.size() && cJSON_GetArraySize(result) < limit; ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos) title.erase(0, slash + 1);
        std::string title_lower = title;
        std::string wanted_artist = artist;
        std::string wanted_genre = genre;
        auto lowercase = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(), lowercase);
        std::transform(wanted_artist.begin(), wanted_artist.end(), wanted_artist.begin(), lowercase);
        std::transform(wanted_genre.begin(), wanted_genre.end(), wanted_genre.begin(), lowercase);
        if ((!wanted_artist.empty() && title_lower.find(wanted_artist) == std::string::npos) ||
            (!wanted_genre.empty() && title_lower.find(wanted_genre) == std::string::npos)) continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", static_cast<double>(i + 1));
        cJSON_AddStringToObject(item, "title", title.c_str());
        cJSON_AddItemToArray(result, item);
    }
    char* output = cJSON_PrintUnformatted(result);
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(result);
    return response;
}

int MediaPlayer::FindSdTrack(const std::string& query) const {
    std::string needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylist();
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos) title.erase(0, slash + 1);
        std::string lower_title = title;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_title.find(needle) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}

void MediaPlayer::PlayRadio(const std::string& url, const std::string& title) {
    paused_for_voice_ = false;
    radio_url_for_voice_.clear();
    radio_title_for_voice_.clear();
    auto& application = Application::GetInstance();
    application.StopVoiceInteractionForMedia();
    SdMusicPlayer::GetInstance().Stop();
    InternetRadioPlayer::GetInstance().Play(url, title);
}

void MediaPlayer::TogglePlayPause() {
    paused_for_voice_ = false;
    if (InternetRadioPlayer::GetInstance().IsPlaying() ||
        InternetRadioPlayer::GetInstance().IsPaused()) {
        InternetRadioPlayer::GetInstance().TogglePlayPause();
        return;
    }
    SdMusicPlayer::GetInstance().TogglePlayPause();
}

void MediaPlayer::PauseForVoice() {
    auto& radio = InternetRadioPlayer::GetInstance();
    if (radio.IsPlaying()) {
        radio_url_for_voice_ = radio.GetUrl();
        radio_title_for_voice_ = radio.GetTitle();
        radio.Stop();
        paused_for_voice_ = true;
        return;
    }
    if (SdMusicPlayer::GetInstance().IsPlaying()) {
        TogglePlayPause();
        paused_for_voice_ = true;
    }
}

void MediaPlayer::PlayForVoice() {
    if (paused_for_voice_.exchange(false)) {
        if (!radio_url_for_voice_.empty()) {
            const std::string url = std::move(radio_url_for_voice_);
            const std::string title = std::move(radio_title_for_voice_);
            radio_title_for_voice_.clear();
            PlayRadio(url, title);
        } else {
            TogglePlayPause();
        }
    }
}

void MediaPlayer::Next() {
    paused_for_voice_ = false;
    if (InternetRadioPlayer::GetInstance().IsActive()) {
        InternetRadioPlayer::GetInstance().Stop();
        return;
    }
    SdMusicPlayer::GetInstance().Next();
}

void MediaPlayer::Prev() {
    paused_for_voice_ = false;
    if (InternetRadioPlayer::GetInstance().IsActive()) {
        InternetRadioPlayer::GetInstance().Stop();
        return;
    }
    SdMusicPlayer::GetInstance().Prev();
}

void MediaPlayer::Stop() {
    paused_for_voice_ = false;
    radio_url_for_voice_.clear();
    radio_title_for_voice_.clear();
    Application::GetInstance().StopVoiceInteractionForMedia();
    SdMusicPlayer::GetInstance().Stop();
    InternetRadioPlayer::GetInstance().Stop();
}

bool MediaPlayer::IsPlaying() const {
    return SdMusicPlayer::GetInstance().IsPlaying() ||
           InternetRadioPlayer::GetInstance().IsPlaying();
}

bool MediaPlayer::IsPaused() const {
    return SdMusicPlayer::GetInstance().IsPaused() ||
           InternetRadioPlayer::GetInstance().IsPaused();
}

std::string MediaPlayer::GetTitle() const {
    if (InternetRadioPlayer::GetInstance().IsPlaying() ||
        InternetRadioPlayer::GetInstance().IsPaused())
        return InternetRadioPlayer::GetInstance().GetTitle();
    return SdMusicPlayer::GetInstance().GetCurrentTrackName();
}

std::string MediaPlayer::GetStatus() const {
    if (IsPlaying()) return "▶ Воспроизведение";
    if (IsPaused()) return "⏸ Пауза";
    return "⏹ Остановлен";
}
