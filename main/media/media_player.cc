#include "media_player.h"

#include "application.h"
#include "internet_radio_player.h"
#include "radio_browser.h"
#include "sd_music_player.h"
#include "music/music_service.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <utility>

static const char* TAG = "MediaPlayer";

MediaPlayer& MediaPlayer::GetInstance() {
    static MediaPlayer instance;
    return instance;
}

void MediaPlayer::EnsureControlTask() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (control_task_handle_ != nullptr) {
        return;
    }

    TaskHandle_t task = nullptr;
    const BaseType_t result = xTaskCreatePinnedToCore(
        ControlTaskFunction, "MediaControl", 8192, this, 4, &task, 0);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create media control task");
        return;
    }
    control_task_handle_ = task;
}

bool MediaPlayer::EnqueueControlCommand(ControlCommand&& command,
                                        bool replace_pending_source) {
    constexpr size_t kMaxPendingCommands = 8;
    EnsureControlTask();
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (control_task_handle_ == nullptr) {
        return false;
    }

    command.sequence = control_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (replace_pending_source) {
        latest_source_sequence_.store(command.sequence, std::memory_order_release);
    }
    if (replace_pending_source) {
        control_queue_.erase(
            std::remove_if(control_queue_.begin(), control_queue_.end(), [](const auto& pending) {
                return pending.type == ControlCommandType::PlaySd ||
                       pending.type == ControlCommandType::PlayRadio ||
                       pending.type == ControlCommandType::PlayRadioByUuid ||
                       pending.type == ControlCommandType::Stop;
            }),
            control_queue_.end());
    }

    if (!replace_pending_source &&
        (command.type == ControlCommandType::PauseForVoice ||
         command.type == ControlCommandType::PlayForVoice ||
         command.type == ControlCommandType::PauseForGeneratedDownload ||
         command.type == ControlCommandType::ResumeAfterGeneratedDownload)) {
        const auto duplicate = std::find_if(
            control_queue_.begin(), control_queue_.end(), [&command](const auto& pending) {
                return pending.type == command.type;
            });
        if (duplicate != control_queue_.end()) {
            *duplicate = std::move(command);
            control_cv_.notify_one();
            return true;
        }
    }

    if (control_queue_.size() >= kMaxPendingCommands) {
        const auto discard = std::find_if(control_queue_.begin(), control_queue_.end(),
                                          [](const auto& pending) {
                                              return pending.type != ControlCommandType::PlaySd &&
                                                     pending.type != ControlCommandType::PlayRadio &&
                                                     pending.type != ControlCommandType::PlayRadioByUuid &&
                                                     pending.type != ControlCommandType::Stop;
                                          });
        if (discard != control_queue_.end()) {
            ESP_LOGW(TAG, "MEDIA_COMMAND_DROP type=%d queue_full", static_cast<int>(discard->type));
            control_queue_.erase(discard);
        } else {
            ESP_LOGW(TAG, "MEDIA_COMMAND_DROP type=%d queue_full", static_cast<int>(
                         control_queue_.front().type));
            control_queue_.pop_front();
        }
    }
    control_queue_.push_back(std::move(command));
    control_cv_.notify_one();
    return true;
}

bool MediaPlayer::IsLatestSourceCommand(uint64_t sequence) const {
    return latest_source_sequence_.load(std::memory_order_acquire) == sequence;
}

void MediaPlayer::ControlTaskFunction(void* arg) {
    auto* player = static_cast<MediaPlayer*>(arg);
    while (true) {
        ControlCommand command;
        {
            std::unique_lock<std::mutex> lock(player->control_mutex_);
            player->control_cv_.wait(lock, [player]() { return !player->control_queue_.empty(); });
            command = std::move(player->control_queue_.front());
            player->control_queue_.pop_front();
        }
        player->ProcessControlCommand(std::move(command));
    }
}

void MediaPlayer::ProcessControlCommand(ControlCommand&& command) {
    switch (command.type) {
        case ControlCommandType::PlaySd:
            if (IsLatestSourceCommand(command.sequence)) {
                PlaySdNow(command.sd_index, command.sequence);
            } else {
                ESP_LOGI(TAG, "MEDIA_COMMAND_STALE type=sd sequence=%llu",
                         static_cast<unsigned long long>(command.sequence));
            }
            break;
        case ControlCommandType::PlayRadio:
            if (IsLatestSourceCommand(command.sequence)) {
                PlayRadioNow(std::move(command));
            } else {
                ESP_LOGI(TAG, "MEDIA_COMMAND_STALE type=radio sequence=%llu",
                         static_cast<unsigned long long>(command.sequence));
            }
            break;
        case ControlCommandType::PlayRadioByUuid:
            if (IsLatestSourceCommand(command.sequence)) {
                PlayRadioByUuidNow(std::move(command));
            } else {
                ESP_LOGI(TAG, "MEDIA_COMMAND_STALE type=radio_uuid sequence=%llu",
                         static_cast<unsigned long long>(command.sequence));
            }
            break;
        case ControlCommandType::Stop:
            StopNow();
            break;
        case ControlCommandType::TogglePlayPause:
            TogglePlayPauseNow();
            break;
        case ControlCommandType::PauseForVoice:
            PauseForVoiceNow();
            break;
        case ControlCommandType::PlayForVoice:
            PlayForVoiceNow();
            break;
        case ControlCommandType::PauseForGeneratedDownload:
            PauseForGeneratedDownloadNow();
            break;
        case ControlCommandType::ResumeAfterGeneratedDownload:
            ResumeAfterGeneratedDownloadNow();
            break;
        case ControlCommandType::Next:
            NextNow();
            break;
        case ControlCommandType::Prev:
            PrevNow();
            break;
    }
}

bool MediaPlayer::StopVoiceInteractionForMediaSynchronously() {
    struct Completion {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool cancelled = false;
    };

    auto completion = std::make_shared<Completion>();
    Application::GetInstance().Schedule([completion]() {
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            if (completion->cancelled) {
                completion->done = true;
                completion->cv.notify_one();
                return;
            }
        }
        Application::GetInstance().StopVoiceInteractionForMedia();
        {
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->done = true;
        }
        completion->cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->cv.wait_for(lock, std::chrono::seconds(2), [&completion]() {
            return completion->done;
        })) {
        completion->cancelled = true;
        ESP_LOGE(TAG, "MEDIA_STOP_VOICE_TIMEOUT");
        return false;
    }
    return true;
}

void MediaPlayer::ScanSd() { SdMusicPlayer::GetInstance().ScanPlaylist(); }

bool MediaPlayer::PrepareGeneratedPlaylist(std::string& err_msg) {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        err_msg = "generated_download_busy";
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=generated_prepare");
        return false;
    }
    // This method is also used by tab navigation. Browsing a different list
    // must not stop radio, SD playback, or an active voice interaction;
    // explicit PlayGenerated() performs the source switch through PlaySd().
    SdMusicPlayer::GetInstance().EnsurePlaylist(SdMusicPlayer::PlaylistSource::Generated);
    return true;
}

bool MediaPlayer::PrepareSdPlaylist(std::string& err_msg) {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        err_msg = "generated_download_busy";
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=sd_prepare");
        return false;
    }
    SdMusicPlayer::GetInstance().EnsurePlaylist(SdMusicPlayer::PlaylistSource::Normal);
    return true;
}

bool MediaPlayer::PlayGenerated(const std::string& query, std::string& err_msg) {
    if (query.empty()) {
        err_msg = "query_required";
        return false;
    }
    if (!PrepareGeneratedPlaylist(err_msg))
        return false;
    bool ambiguous = false;
    const int index = SdMusicPlayer::GetInstance().FindTrack(query, ambiguous);
    if (index < 0) {
        err_msg = ambiguous ? "ambiguous_generated_track" : "generated_track_not_found";
        return false;
    }
    SdMusicPlayer::GetInstance().SetSelectedTrackIndex(index);
    PlaySd(index);
    return true;
}

void MediaPlayer::PlaySd(int index) {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=sd");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        paused_for_voice_ = false;
        sd_paused_for_voice_ = false;
        radio_station_for_voice_ = {};
        paused_by_user_ = false;
        radio_station_for_manual_pause_ = {};
    }
    ControlCommand command;
    command.type = ControlCommandType::PlaySd;
    command.sd_index = index;
    if (!EnqueueControlCommand(std::move(command), true)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=sd");
    }
}

void MediaPlayer::PlaySdNow(int index, uint64_t sequence) {
    ESP_LOGI(TAG, "MEDIA_COMMAND_EXECUTE type=sd index=%d task=%s", index, pcTaskGetName(nullptr));
    if (!StopVoiceInteractionForMediaSynchronously()) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_ABORT type=sd reason=voice_stop_timeout");
        return;
    }
    InternetRadioPlayer::GetInstance().Stop();
    if (!IsLatestSourceCommand(sequence)) {
        ESP_LOGI(TAG, "MEDIA_COMMAND_STALE_AFTER_STOP type=sd sequence=%llu",
                 static_cast<unsigned long long>(sequence));
        return;
    }
    auto& player = SdMusicPlayer::GetInstance();
    player.Play(index < 0 ? player.GetCurrentTrackIndex() : index);
}

std::string MediaPlayer::ListSdTracks() const {
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylistSnapshot();
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_CreateArray(), &cJSON_Delete);
    if (result == nullptr)
        return "[]";
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos)
            title.erase(0, slash + 1);
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", static_cast<double>(i + 1));
        cJSON_AddStringToObject(item, "title", title.c_str());
        if (item == nullptr || !cJSON_AddItemToArray(result.get(), item)) {
            cJSON_Delete(item);
            return "[]";
        }
    }
    char* output = cJSON_PrintUnformatted(result.get());
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr)
        cJSON_free(output);
    return response;
}

std::string MediaPlayer::SearchSdTracks(const std::string& artist, const std::string& genre,
                                        int limit) const {
    limit = std::max(1, std::min(limit, 20));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> result(cJSON_CreateArray(), &cJSON_Delete);
    if (result == nullptr)
        return "[]";
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylistSnapshot();
    for (size_t i = 0; i < tracks.size() && cJSON_GetArraySize(result.get()) < limit; ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos)
            title.erase(0, slash + 1);
        std::string title_lower = title;
        std::string wanted_artist = artist;
        std::string wanted_genre = genre;
        auto lowercase = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(), lowercase);
        std::transform(wanted_artist.begin(), wanted_artist.end(), wanted_artist.begin(),
                       lowercase);
        std::transform(wanted_genre.begin(), wanted_genre.end(), wanted_genre.begin(), lowercase);
        if ((!wanted_artist.empty() && title_lower.find(wanted_artist) == std::string::npos) ||
            (!wanted_genre.empty() && title_lower.find(wanted_genre) == std::string::npos))
            continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", static_cast<double>(i + 1));
        cJSON_AddStringToObject(item, "title", title.c_str());
        if (item == nullptr || !cJSON_AddItemToArray(result.get(), item)) {
            cJSON_Delete(item);
            return "[]";
        }
    }
    char* output = cJSON_PrintUnformatted(result.get());
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr)
        cJSON_free(output);
    return response;
}

int MediaPlayer::FindSdTrack(const std::string& query) const {
    std::string needle = query;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto& tracks = SdMusicPlayer::GetInstance().GetPlaylistSnapshot();
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string title = tracks[i];
        const size_t slash = title.find_last_of('/');
        if (slash != std::string::npos)
            title.erase(0, slash + 1);
        std::string lower_title = title;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_title.find(needle) != std::string::npos)
            return static_cast<int>(i);
    }
    return -1;
}

bool MediaPlayer::PlayRadio(const RadioStationInfo& station) {
    std::string err_msg;
    return PlayRadio(station, err_msg);
}

bool MediaPlayer::PlayRadioByUuid(
    const std::string& station_uuid, const std::string& title,
    std::function<void(const RadioStationInfo&)> on_startup_ready,
    std::function<void()> on_startup_failed, bool emit_failure_bip) {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=radio_uuid");
        return false;
    }
    if (station_uuid.empty()) {
        return false;
    }

    bool restore_voice_pause = false;
    bool restore_manual_pause = false;
    RadioStationInfo voice_station;
    RadioStationInfo manual_station;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        restore_voice_pause = paused_for_voice_.load();
        restore_manual_pause = paused_by_user_.load();
        voice_station = radio_station_for_voice_;
        manual_station = radio_station_for_manual_pause_;
        paused_for_voice_ = false;
        radio_station_for_voice_ = {};
        sd_paused_for_voice_ = false;
        paused_by_user_ = false;
        radio_station_for_manual_pause_ = {};
    }

    ControlCommand command;
    command.type = ControlCommandType::PlayRadioByUuid;
    command.station.stationuuid = station_uuid;
    command.station.name = title;
    command.on_station_resolved = std::move(on_startup_ready);
    command.on_startup_failed = std::move(on_startup_failed);
    command.emit_failure_bip = emit_failure_bip;
    command.restore_voice_pause = restore_voice_pause;
    command.restore_manual_pause = restore_manual_pause;
    command.voice_station = voice_station;
    command.manual_station = manual_station;
    if (!EnqueueControlCommand(std::move(command), true)) {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        if (restore_voice_pause) {
            paused_for_voice_ = true;
            radio_station_for_voice_ = std::move(voice_station);
        }
        if (restore_manual_pause) {
            paused_by_user_ = true;
            radio_station_for_manual_pause_ = std::move(manual_station);
        }
        return false;
    }
    return true;
}

bool MediaPlayer::PlayRadio(const std::string& url, const std::string& title) {
    std::string err_msg;
    return PlayRadio(url, title, err_msg);
}

bool MediaPlayer::PlayRadio(const std::string& url, const std::string& title,
                            std::string& err_msg) {
    RadioStationInfo station;
    station.url_resolved = url;
    station.name = title;
    return PlayRadio(station, err_msg);
}

bool MediaPlayer::PlayRadio(const RadioStationInfo& station, std::string& err_msg,
                            std::function<void()> on_startup_ready,
                            std::function<void()> on_startup_failed, bool emit_failure_bip) {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        err_msg = "generated_download_busy";
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=radio");
        return false;
    }
    if (station.url_resolved.empty()) {
        err_msg = "Station URL is empty";
        return false;
    }
    bool restore_voice_pause = false;
    bool restore_manual_pause = false;
    RadioStationInfo voice_station;
    RadioStationInfo manual_station;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        restore_voice_pause = paused_for_voice_.load();
        restore_manual_pause = paused_by_user_.load();
        voice_station = radio_station_for_voice_;
        manual_station = radio_station_for_manual_pause_;
        paused_for_voice_ = false;
        radio_station_for_voice_ = {};
        sd_paused_for_voice_ = false;
        paused_by_user_ = false;
        radio_station_for_manual_pause_ = {};
    }
    ControlCommand command;
    command.type = ControlCommandType::PlayRadio;
    command.station = station;
    command.on_startup_ready = std::move(on_startup_ready);
    command.on_startup_failed = std::move(on_startup_failed);
    command.emit_failure_bip = emit_failure_bip;
    command.restore_voice_pause = restore_voice_pause;
    command.restore_manual_pause = restore_manual_pause;
    command.voice_station = voice_station;
    command.manual_station = manual_station;
    if (!EnqueueControlCommand(std::move(command), true)) {
        err_msg = "Unable to queue media command";
        std::lock_guard<std::mutex> lock(voice_mutex_);
        if (restore_voice_pause) {
            paused_for_voice_ = true;
            radio_station_for_voice_ = std::move(voice_station);
        }
        if (restore_manual_pause) {
            paused_by_user_ = true;
            radio_station_for_manual_pause_ = std::move(manual_station);
        }
        return false;
    }
    return true;
}

void MediaPlayer::PlayRadioNow(ControlCommand&& command) {
    if (!IsLatestSourceCommand(command.sequence)) {
        return;
    }

    ESP_LOGI(TAG, "MEDIA_COMMAND_EXECUTE type=radio name=%s task=%s",
             command.station.name.c_str(), pcTaskGetName(nullptr));
    Application::GetInstance().GetAudioService().CheckHeapIntegrity("play_radio_begin");

    auto restore_pause_state = [&command, this]() {
        if (command.restore_voice_pause || command.restore_manual_pause) {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            if (command.restore_voice_pause) {
                paused_for_voice_ = true;
                radio_station_for_voice_ = std::move(command.voice_station);
            }
            if (command.restore_manual_pause) {
                paused_by_user_ = true;
                radio_station_for_manual_pause_ = std::move(command.manual_station);
            }
        }
    };

    // Stop the AI interaction before creating the radio task. The application
    // mutation runs on the main task, while this control task waits for the
    // completion so the two audio sources cannot overlap.
    if (!StopVoiceInteractionForMediaSynchronously()) {
        restore_pause_state();
        ESP_LOGE(TAG, "MEDIA_COMMAND_ABORT type=radio reason=voice_stop_timeout");
        return;
    }

    // Ensure the SD decoder task and audio pipeline are fully stopped before
    // starting the radio source; this prevents a transient dual-source period.
    SdMusicPlayer::GetInstance().Stop();
    if (!IsLatestSourceCommand(command.sequence)) {
        ESP_LOGI(TAG, "MEDIA_COMMAND_STALE_AFTER_STOP type=radio sequence=%llu",
                 static_cast<unsigned long long>(command.sequence));
        return;
    }

    std::string err_msg;
    const auto startup_failed = command.on_startup_failed;
    const bool success = InternetRadioPlayer::GetInstance().Play(
        command.station, err_msg, std::move(command.on_startup_ready),
        std::move(command.on_startup_failed), command.emit_failure_bip);
    if (success) {
        ESP_LOGI(TAG, "MEDIA_VOICE_STOPPED_BEFORE_RADIO");
    } else {
        restore_pause_state();
        if (startup_failed) {
            startup_failed();
        }
        ESP_LOGW(TAG, "MEDIA_COMMAND_FAILED type=radio error=%s", err_msg.c_str());
    }
}

void MediaPlayer::PlayRadioByUuidNow(ControlCommand&& command) {
    ESP_LOGI(TAG, "MEDIA_COMMAND_EXECUTE type=radio_uuid uuid=%s task=%s",
             command.station.stationuuid.c_str(), pcTaskGetName(nullptr));
    Application::GetInstance().GetAudioService().CheckHeapIntegrity("play_radio_uuid_begin");

    auto restore_pause_state = [&command, this]() {
        if (command.restore_voice_pause || command.restore_manual_pause) {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            if (command.restore_voice_pause) {
                paused_for_voice_ = true;
                radio_station_for_voice_ = std::move(command.voice_station);
            }
            if (command.restore_manual_pause) {
                paused_by_user_ = true;
                radio_station_for_manual_pause_ = std::move(command.manual_station);
            }
        }
    };

    // Cancel the previous source before doing network resolution. This keeps
    // a reconnecting station from competing with the selected station while
    // the UUID lookup is running, without blocking the LVGL task.
    if (!StopVoiceInteractionForMediaSynchronously()) {
        restore_pause_state();
        ESP_LOGE(TAG, "MEDIA_COMMAND_ABORT type=radio_uuid reason=voice_stop_timeout");
        return;
    }
    SdMusicPlayer::GetInstance().Stop();
    InternetRadioPlayer::GetInstance().Stop();
    if (!IsLatestSourceCommand(command.sequence)) {
        ESP_LOGI(TAG, "MEDIA_COMMAND_STALE_AFTER_STOP type=radio_uuid sequence=%llu",
                 static_cast<unsigned long long>(command.sequence));
        return;
    }

    RadioStationInfo station;
    std::string err_msg;
    if (!RadioBrowser::GetInstance().GetStationByUuid(command.station.stationuuid, station,
                                                       err_msg)) {
        restore_pause_state();
        if (command.emit_failure_bip) {
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_RADIO_ERROR);
            });
        }
        if (command.on_startup_failed)
            std::move(command.on_startup_failed)();
        ESP_LOGW(TAG, "MEDIA_COMMAND_FAILED type=radio_uuid uuid=%s error=%s",
                 command.station.stationuuid.c_str(), err_msg.c_str());
        return;
    }

    if (!IsLatestSourceCommand(command.sequence)) {
        ESP_LOGI(TAG, "MEDIA_COMMAND_STALE_AFTER_RESOLVE type=radio_uuid sequence=%llu",
                 static_cast<unsigned long long>(command.sequence));
        return;
    }

    auto on_station_resolved = std::move(command.on_station_resolved);
    if (on_station_resolved) {
        command.on_startup_ready = [station, callback = std::move(on_station_resolved)]() mutable {
            callback(station);
        };
    }
    command.on_station_resolved = {};
    command.type = ControlCommandType::PlayRadio;
    command.station = std::move(station);
    PlayRadioNow(std::move(command));
}

void MediaPlayer::TogglePlayPause() {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=toggle");
        return;
    }
    ControlCommand command;
    command.type = ControlCommandType::TogglePlayPause;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=toggle");
    }
}

void MediaPlayer::TogglePlayPauseNow() {
    auto& radio = InternetRadioPlayer::GetInstance();

    // A user action during a voice-interrupted Radio session cancels the
    // pending automatic resume. Keep the station as a manual pause target so
    // a later explicit Play can reconnect it, but never restart it implicitly.
    if (paused_for_voice_.load()) {
        RadioStationInfo station;
        bool converted_to_manual_pause = false;
        bool resume_sd = false;
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            station = radio_station_for_voice_;
            if (!station.url_resolved.empty()) {
                radio_station_for_manual_pause_ = station;
                paused_by_user_ = true;
                radio_station_for_voice_ = {};
                paused_for_voice_ = false;
                converted_to_manual_pause = true;
            } else if (sd_paused_for_voice_.load()) {
                sd_paused_for_voice_ = false;
                paused_for_voice_ = false;
                resume_sd = true;
            }
        }
        if (converted_to_manual_pause) {
            ESP_LOGI(TAG, "[RADIO_MANUAL_PAUSE] station=%s active_before=0 voice_pending=1",
                     station.name.c_str());
            return;
        }
        if (resume_sd) {
            ESP_LOGI(TAG, "[SD_VOICE_MANUAL_RESUME]");
            SdMusicPlayer::GetInstance().ResumeFromVoice();
            return;
        }
        paused_for_voice_ = false;
    }

    if (radio.IsPlaying()) {
        const RadioStationInfo station = radio.GetCurrentStation();
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            radio_station_for_manual_pause_ = station;
            paused_by_user_ = true;
            paused_for_voice_ = false;
            radio_station_for_voice_ = {};
        }
        ESP_LOGI(TAG, "[RADIO_MANUAL_PAUSE] station=%s active_before=1", station.name.c_str());
        radio.Stop();
        ESP_LOGI(TAG, "[RADIO_MANUAL_PAUSE_DONE] active=%d", radio.IsActive() ? 1 : 0);
        return;
    }

    if (paused_by_user_.load()) {
        RadioStationInfo station;
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            station = radio_station_for_manual_pause_;
        }
        if (radio.IsActive() || station.url_resolved.empty()) {
            return;
        }
        ESP_LOGI(TAG, "[RADIO_MANUAL_RESUME] station=%s", station.name.c_str());
        std::string err_msg;
        const bool success = PlayRadio(station, err_msg, {}, [this, station]() {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            radio_station_for_manual_pause_ = station;
            paused_by_user_ = true;
        });
        if (!success) {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            radio_station_for_manual_pause_ = station;
            paused_by_user_ = true;
        }
        ESP_LOGI(TAG, "[RADIO_MANUAL_RESUME_RESULT] success=%d", success ? 1 : 0);
        return;
    }

    paused_for_voice_ = false;
    if (radio.IsPaused()) {
        // Preserve compatibility with any non-UI caller that still uses the
        // legacy in-task pause state. Manual UI pause never enters this path.
        radio.TogglePlayPause();
        return;
    }
    SdMusicPlayer::GetInstance().TogglePlayPause();
}

void MediaPlayer::PauseForVoice() {
    ControlCommand command;
    command.type = ControlCommandType::PauseForVoice;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=pause_voice");
    }
}

void MediaPlayer::PauseForVoiceNow() {
    auto& radio = InternetRadioPlayer::GetInstance();
    if (paused_by_user_.load()) {
        return;
    }
    if (radio.IsPlaying()) {
        RadioStationInfo station;
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            if (paused_by_user_.load())
                return;
            sd_paused_for_voice_ = false;
            station = radio.GetCurrentStation();
            radio_station_for_voice_ = station;
            paused_for_voice_ = true;
        }
        ESP_LOGI(TAG, "[RADIO_VOICE_PAUSE] station=%s", station.name.c_str());
        radio.Stop();
        return;
    }
    if (sd_paused_for_voice_.load()) {
        return;
    }
    if (SdMusicPlayer::GetInstance().IsPlaying()) {
        auto& sd = SdMusicPlayer::GetInstance();
        if (!sd.PauseForVoice()) {
            return;
        }
        // Drop already decoded media PCM, but keep the SD file and decoder
        // position alive. This prevents old music frames from competing with
        // the voice path while preserving resume from the current position.
        Application::GetInstance().GetAudioService().ResetDecoder();
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            radio_station_for_voice_ = {};
            sd_paused_for_voice_ = true;
        }
        ESP_LOGI(TAG, "[SD_VOICE_PAUSE]");
        paused_for_voice_ = true;
    }
}

void MediaPlayer::PlayForVoice() {
    ControlCommand command;
    command.type = ControlCommandType::PlayForVoice;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=play_voice");
    }
}

void MediaPlayer::PlayForVoiceNow() {
    if (paused_by_user_.load()) {
        paused_for_voice_ = false;
        sd_paused_for_voice_ = false;
        return;
    }
    if (paused_for_voice_.exchange(false)) {
        RadioStationInfo station;
        bool resume_sd = false;
        {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            station = std::move(radio_station_for_voice_);
            radio_station_for_voice_ = {};
            resume_sd = sd_paused_for_voice_.exchange(false);
        }
        if (!station.url_resolved.empty()) {
            ESP_LOGI(TAG, "[RADIO_VOICE_RESUME] station=%s", station.name.c_str());
            ESP_LOGI(TAG, "[RADIO_RESUME]\nname=%s\nuuid=%s\ncodec=%s\nurl=%s",
                     station.name.c_str(), station.stationuuid.c_str(), station.codec.c_str(),
                     station.url_resolved.c_str());
            std::string err_msg;
            auto restore_voice_pause = [this, station]() {
                std::lock_guard<std::mutex> lock(voice_mutex_);
                if (!paused_by_user_.load()) {
                    radio_station_for_voice_ = station;
                    paused_for_voice_ = true;
                }
            };
            if (!PlayRadio(station, err_msg, {}, restore_voice_pause)) {
                restore_voice_pause();
            }
        } else if (resume_sd) {
            ESP_LOGI(TAG, "[SD_VOICE_RESUME]");
            SdMusicPlayer::GetInstance().ResumeFromVoice();
        }
    }
}

void MediaPlayer::PauseForGeneratedDownload() {
    ControlCommand command;
    command.type = ControlCommandType::PauseForGeneratedDownload;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=pause_generated");
    }
}

void MediaPlayer::PauseForGeneratedDownloadNow() {
    auto& radio = InternetRadioPlayer::GetInstance();
    RadioStationInfo radio_station;
    bool pause_radio = false;
    bool pause_sd = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        if (generated_download_sd_paused_ || generated_download_radio_paused_) return;
        // A download is admitted only while AI is idle. Existing voice/user
        // ownership therefore wins and must not be overwritten here.
        if (paused_for_voice_.load() || paused_by_user_.load()) return;
        if (radio.IsActive()) {
            radio_station = radio.GetCurrentStation();
            pause_radio = !radio_station.url_resolved.empty();
            generated_download_radio_paused_ = pause_radio;
            radio_station_for_generated_download_ = radio_station;
        } else if (SdMusicPlayer::GetInstance().IsPlaying()) {
            pause_sd = true;
            generated_download_sd_paused_ = true;
        }
    }
    if (pause_radio) {
        ESP_LOGI(TAG, "[MUSIC_DOWNLOAD_STOP_RADIO] station=%s", radio_station.name.c_str());
        radio.Stop();
    } else if (pause_sd) {
        ESP_LOGI(TAG, "[MUSIC_DOWNLOAD_PAUSE_MEDIA] source=%s",
                 SdMusicPlayer::GetInstance().IsGeneratedPlayback() ? "generated" : "sd");
        SdMusicPlayer::GetInstance().TogglePlayPause();
    }
}

void MediaPlayer::ResumeAfterGeneratedDownload() {
    ControlCommand command;
    command.type = ControlCommandType::ResumeAfterGeneratedDownload;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=resume_generated");
    }
}

void MediaPlayer::ResumeAfterGeneratedDownloadNow() {
    RadioStationInfo radio_station;
    bool resume_radio = false;
    bool resume_sd = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        resume_radio = generated_download_radio_paused_;
        resume_sd = generated_download_sd_paused_;
        radio_station = radio_station_for_generated_download_;
        generated_download_radio_paused_ = false;
        generated_download_sd_paused_ = false;
        radio_station_for_generated_download_ = {};
    }
    if (resume_radio && !radio_station.url_resolved.empty()) {
        std::string error;
        ESP_LOGI(TAG, "[MUSIC_DOWNLOAD_RESTORE_RADIO] station=%s", radio_station.name.c_str());
        if (!PlayRadio(radio_station, error)) {
            ESP_LOGW(TAG, "[MUSIC_DOWNLOAD_RESTORE_RADIO_FAIL] station=%s error=%s",
                     radio_station.name.c_str(), error.c_str());
        }
    } else if (resume_sd) {
        ESP_LOGI(TAG, "[MUSIC_DOWNLOAD_RESTORE_MEDIA] source=%s",
                 SdMusicPlayer::GetInstance().IsGeneratedPlayback() ? "generated" : "sd");
        if (!SdMusicPlayer::GetInstance().IsPlaying() && !SdMusicPlayer::GetInstance().IsPaused()) {
            SdMusicPlayer::GetInstance().TogglePlayPause();
        } else if (SdMusicPlayer::GetInstance().IsPaused()) {
            SdMusicPlayer::GetInstance().TogglePlayPause();
        }
    }
}

void MediaPlayer::Next() {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=next");
        return;
    }
    ControlCommand command;
    command.type = ControlCommandType::Next;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=next");
    }
}

void MediaPlayer::NextNow() {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=next");
        return;
    }
    paused_for_voice_ = false;
    sd_paused_for_voice_ = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        radio_station_for_voice_ = {};
    }
    if (InternetRadioPlayer::GetInstance().IsActive()) {
        RadioBrowser::GetInstance().MoveActiveStation(1);
        return;
    }
    SdMusicPlayer::GetInstance().Next();
}

void MediaPlayer::Prev() {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=previous");
        return;
    }
    ControlCommand command;
    command.type = ControlCommandType::Prev;
    if (!EnqueueControlCommand(std::move(command), false)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=previous");
    }
}

void MediaPlayer::PrevNow() {
    if (MusicService::GetInstance().IsGeneratedDownloadBusy()) {
        ESP_LOGI(TAG, "MEDIA_START_BLOCKED reason=generated_download_busy type=previous");
        return;
    }
    paused_for_voice_ = false;
    sd_paused_for_voice_ = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        radio_station_for_voice_ = {};
    }
    if (InternetRadioPlayer::GetInstance().IsActive()) {
        RadioBrowser::GetInstance().MoveActiveStation(-1);
        return;
    }
    SdMusicPlayer::GetInstance().Prev();
}

void MediaPlayer::Stop() {
    paused_for_voice_ = false;
    sd_paused_for_voice_ = false;
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        radio_station_for_voice_ = {};
        paused_by_user_ = false;
        radio_station_for_manual_pause_ = {};
    }
    ControlCommand command;
    command.type = ControlCommandType::Stop;
    if (!EnqueueControlCommand(std::move(command), true)) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_REJECTED type=stop");
    }
}

void MediaPlayer::StopNow() {
    if (!StopVoiceInteractionForMediaSynchronously()) {
        ESP_LOGE(TAG, "MEDIA_COMMAND_ABORT type=stop reason=voice_stop_timeout");
        return;
    }
    Application::GetInstance().GetAudioService().CheckHeapIntegrity("media_stop_begin");
    SdMusicPlayer::GetInstance().Stop();
    InternetRadioPlayer::GetInstance().Stop();
}

bool MediaPlayer::IsPlaying() const {
    return SdMusicPlayer::GetInstance().IsPlaying() ||
           InternetRadioPlayer::GetInstance().IsPlaying();
}

bool MediaPlayer::HasRadioPausedByUser() const { return paused_by_user_.load(); }

bool MediaPlayer::GetRadioStationPausedByUser(RadioStationInfo& station) const {
    if (!paused_by_user_.load())
        return false;
    std::lock_guard<std::mutex> lock(voice_mutex_);
    station = radio_station_for_manual_pause_;
    return !station.url_resolved.empty();
}

bool MediaPlayer::GetCurrentRadioStationForAction(RadioStationInfo& station) const {
    if (InternetRadioPlayer::GetInstance().IsPlaying()) {
        station = InternetRadioPlayer::GetInstance().GetCurrentStation();
        return !station.stationuuid.empty();
    }
    if (paused_by_user_.load()) {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        station = radio_station_for_manual_pause_;
        return !station.stationuuid.empty();
    }
    if (!paused_for_voice_.load())
        return false;
    std::lock_guard<std::mutex> lock(voice_mutex_);
    station = radio_station_for_voice_;
    return !station.stationuuid.empty();
}

bool MediaPlayer::GetRadioStationPausedForVoice(RadioStationInfo& station) const {
    if (!paused_for_voice_.load())
        return false;
    std::lock_guard<std::mutex> lock(voice_mutex_);
    station = radio_station_for_voice_;
    return !station.url_resolved.empty();
}

bool MediaPlayer::IsPaused() const {
    return paused_by_user_.load() || SdMusicPlayer::GetInstance().IsPaused() ||
           InternetRadioPlayer::GetInstance().IsPaused();
}

std::string MediaPlayer::GetTitle() const {
    if (InternetRadioPlayer::GetInstance().IsPlaying() ||
        InternetRadioPlayer::GetInstance().IsPaused())
        return InternetRadioPlayer::GetInstance().GetTitle();

    if (paused_for_voice_.load()) {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        if (!radio_station_for_voice_.name.empty())
            return radio_station_for_voice_.name;
    }

    if (paused_by_user_.load()) {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        if (!radio_station_for_manual_pause_.name.empty())
            return radio_station_for_manual_pause_.name;
    }

    return SdMusicPlayer::GetInstance().GetCurrentTrackName();
}

std::string MediaPlayer::GetStatus() const {
    if (IsPlaying())
        return "▶ Воспроизведение";
    if (IsPaused())
        return "⏸ Пауза";
    return "⏹ Остановлен";
}
