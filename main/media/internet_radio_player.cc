#include "internet_radio_player.h"

#include "application.h"
#include "audio_manager.h"
#include "board.h"
#include "audio_codec.h"
#include "media_audio_output.h"
#include "system_info.h"

#include <decoder/impl/esp_mp3_dec.h>
#include <decoder/impl/esp_aac_dec.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#define TAG "InternetRadio"

namespace {
constexpr EventBits_t kStartupBitReady = (1 << 0);
constexpr EventBits_t kStartupBitFailed = (1 << 1);

std::string ResolveRedirectUrl(const std::string& current_url, const std::string& location) {
    if (location.empty()) return "";
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    size_t scheme_end = current_url.find("://");
    if (scheme_end == std::string::npos) return location;

    size_t host_start = scheme_end + 3;
    size_t path_start = current_url.find('/', host_start);

    std::string origin;
    if (path_start == std::string::npos) {
        origin = current_url;
    } else {
        origin = current_url.substr(0, path_start);
    }

    if (location[0] == '/') {
        return origin + location;
    } else {
        size_t last_slash = current_url.rfind('/');
        if (last_slash != std::string::npos && last_slash >= host_start) {
            return current_url.substr(0, last_slash + 1) + location;
        } else {
            return origin + "/" + location;
        }
    }
}

esp_err_t HttpEventHandler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value && evt->user_data) {
            if (strcasecmp(evt->header_key, "Location") == 0) {
                std::string *loc = static_cast<std::string *>(evt->user_data);
                *loc = evt->header_value;
            }
        }
    }
    return ESP_OK;
}

const char* GetWifiPsModeName(wifi_ps_type_t type) {
    switch (type) {
        case WIFI_PS_NONE: return "NONE (PERFORMANCE)";
        case WIFI_PS_MIN_MODEM: return "MIN_MODEM";
        case WIFI_PS_MAX_MODEM: return "MAX_MODEM (LOW_POWER)";
        default: return "UNKNOWN";
    }
}

void LogWifiPsStatus(const char* stage) {
    wifi_ps_type_t ps_type = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps_type) == ESP_OK) {
        ESP_LOGI(TAG, "[WIFI_PS_DIAG] stage=%s mode=%d (%s)", stage, static_cast<int>(ps_type), GetWifiPsModeName(ps_type));
    } else {
        ESP_LOGW(TAG, "[WIFI_PS_DIAG] stage=%s failed to get wifi ps mode", stage);
    }
}
} // namespace

InternetRadioPlayer& InternetRadioPlayer::GetInstance() {
    static InternetRadioPlayer instance;
    return instance;
}

InternetRadioPlayer::InternetRadioPlayer() {
    startup_event_group_ = xEventGroupCreate();
}

InternetRadioPlayer::~InternetRadioPlayer() {
    Stop();
    if (startup_event_group_ != nullptr) {
        vEventGroupDelete(startup_event_group_);
        startup_event_group_ = nullptr;
    }
}

std::string InternetRadioPlayer::GetTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_.empty() ? "Internet radio" : title_;
}

std::string InternetRadioPlayer::GetUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

RadioStationInfo InternetRadioPlayer::GetCurrentStation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_station_;
}

bool InternetRadioPlayer::Play(const RadioStationInfo& station) {
    std::string err_msg;
    return Play(station, err_msg);
}

bool InternetRadioPlayer::Play(const std::string& url, const std::string& title) {
    std::string err_msg;
    return Play(url, title, err_msg);
}

bool InternetRadioPlayer::Play(const std::string& url, const std::string& title, std::string& err_msg) {
    RadioStationInfo station;
    station.url_resolved = url;
    station.name = title;
    return Play(station, err_msg);
}

bool InternetRadioPlayer::Play(const RadioStationInfo& station, std::string& err_msg) {
    if (station.url_resolved.empty()) {
        err_msg = "Station URL is empty";
        return false;
    }

    if (playing_) {
        Stop();
    }

    SystemInfo::PrintRamSnapshot("RADIO_START");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_station_ = station;
        url_ = station.url_resolved;
        title_ = station.name;
        startup_err_msg_.clear();
    }
    ESP_LOGI(TAG, "Radio current:\nname=%s\nuuid=%s\ncodec=%s\nbitrate=%lu\ncountry=%s\nurl=%s",
             current_station_.name.c_str(),
             current_station_.stationuuid.c_str(),
             current_station_.codec.c_str(),
             static_cast<unsigned long>(current_station_.bitrate),
             current_station_.country.c_str(),
             current_station_.url_resolved.c_str());

    stop_requested_ = false;
    reconnect_requested_ = false;
    paused_ = false;
    playing_ = true;
    initial_ready_ = false;

    if (startup_event_group_ != nullptr) {
        xEventGroupClearBits(startup_event_group_, kStartupBitReady | kStartupBitFailed);
    }

    ESP_LOGI(TAG, "Radio stream startup: connecting");

    if (xTaskCreatePinnedToCore(TaskFunction, "InternetRadio", 6144, this, 3,
                                &task_handle_, 1) != pdPASS) {
        playing_ = false;
        task_handle_ = nullptr;
        err_msg = "Unable to create stream task";
        ESP_LOGE(TAG, "%s", err_msg.c_str());
        return false;
    }

    EventBits_t bits = 0;
    if (startup_event_group_ != nullptr) {
        bits = xEventGroupWaitBits(startup_event_group_,
                                   kStartupBitReady | kStartupBitFailed,
                                   pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(3000));
    }

    if (bits & kStartupBitReady) {
        ESP_LOGI(TAG, "Radio stream startup: ready");
        return true;
    }

    std::string failure_reason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_reason = startup_err_msg_;
    }

    if (bits & kStartupBitFailed) {
        if (failure_reason.empty()) failure_reason = "Stream connection failed";
        ESP_LOGW(TAG, "Radio stream startup failed: %s", failure_reason.c_str());
    } else {
        failure_reason = "Connection timeout";
        ESP_LOGW(TAG, "Radio stream startup timeout");
    }

    err_msg = failure_reason;
    Stop();
    return false;
}

void InternetRadioPlayer::TogglePlayPause() {
    if (!playing_) return;
    paused_ = !paused_;
}

void InternetRadioPlayer::Stop() {
    SystemInfo::PrintRamSnapshot("RADIO_STOP");
    stop_requested_ = true;
    paused_ = false;
    if (startup_event_group_ != nullptr) {
        xEventGroupSetBits(startup_event_group_, kStartupBitFailed);
    }
    Application::GetInstance().GetAudioService().ResetDecoder();
    if (task_handle_ != nullptr && xTaskGetCurrentTaskHandle() != task_handle_) {
        for (int i = 0; playing_ && i < 300; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    }
    AudioManager::GetInstance().ReleaseAudioFocus(kAudioSourceInternetRadio);
}

void InternetRadioPlayer::TaskFunction(void* arg) {
    auto* player = static_cast<InternetRadioPlayer*>(arg);
    player->StreamLoop();
    player->playing_ = false;
    player->paused_ = false;
    player->task_handle_ = nullptr;
    AudioManager::GetInstance().ReleaseAudioFocus(kAudioSourceInternetRadio);
    vTaskDelete(nullptr);
}

void InternetRadioPlayer::StreamLoop() {
    AudioManager::GetInstance().RequestAudioFocus(kAudioSourceInternetRadio);
    EnsureMp3DecoderRegistered();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    ESP_LOGI(TAG, "[WIFI_PS_RADIO] switched to PERFORMANCE (NONE)");
    auto* codec = Board::GetInstance().GetAudioCodec();
    uint32_t target_rate = codec ? codec->output_sample_rate() : 24000;
    if (target_rate == 0) target_rate = 24000;

    while (!stop_requested_) {
        std::string current_url;
        { std::lock_guard<std::mutex> lock(mutex_); current_url = url_; }

        int status = -1;
        int redirect_count = 0;
        const int kMaxRedirects = 5;
        bool connect_success = false;
        esp_http_client_handle_t client = nullptr;

        while (redirect_count <= kMaxRedirects && !stop_requested_) {
            std::string redirect_location;
            esp_http_client_config_t config = {};
            config.url = current_url.c_str();
            config.method = HTTP_METHOD_GET;
            config.event_handler = HttpEventHandler;
            config.user_data = &redirect_location;
            config.timeout_ms = 2000;
            config.buffer_size = 4096;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.skip_cert_common_name_check = true;
            config.disable_auto_redirect = true;

            client = esp_http_client_init(&config);
            if (!client) {
                ESP_LOGE(TAG, "[RADIO_HTTP_ERROR] Failed to initialize HTTP client for %s", current_url.c_str());
                break;
            }

            // Metadata interleaving is not MP3 data and would confuse the decoder.
            esp_http_client_set_header(client, "Icy-MetaData", "0");
            esp_http_client_set_header(client, "User-Agent", "xiaozhi-esp32-radio/1.0");

            LogWifiPsStatus("before_connect");

            if (esp_http_client_open(client, 0) != ESP_OK) {
                ESP_LOGW(TAG, "[RADIO_HTTP_ERROR] Failed to open stream: %s", current_url.c_str());
                esp_http_client_cleanup(client);
                client = nullptr;
                break;
            }

            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);

            if (status >= 300 && status < 400) {
                std::string new_url = ResolveRedirectUrl(current_url, redirect_location);

                ESP_LOGI(TAG, "[RADIO_REDIRECT] status=%d from=%s to=%s redirect=%d",
                         status, current_url.c_str(), new_url.c_str(), redirect_count + 1);

                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;

                if (new_url.empty()) {
                    ESP_LOGE(TAG, "[RADIO_HTTP_ERROR] Redirect Location header missing or invalid");
                    break;
                }

                redirect_count++;
                if (redirect_count > kMaxRedirects) {
                    ESP_LOGE(TAG, "[RADIO_HTTP_ERROR] Exceeded maximum redirects (%d)", kMaxRedirects);
                    break;
                }

                current_url = new_url;
                continue;
            }

            if (status >= 200 && status < 300) {
                if (redirect_count > 0) {
                    ESP_LOGI(TAG, "[RADIO_REDIRECT] followed successfully to %s", current_url.c_str());
                }
                connect_success = true;
            }
            break;
        }

        if (!client || !connect_success || status < 200 || status >= 400) {
            if (client) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
            }
            if (!initial_ready_) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (status >= 300 && status < 400) {
                        startup_err_msg_ = "HTTP " + std::to_string(status) + " redirect failed";
                    } else if (status > 0) {
                        startup_err_msg_ = "HTTP " + std::to_string(status);
                    } else {
                        startup_err_msg_ = "Connection failed";
                    }
                }
                if (startup_event_group_ != nullptr) {
                    xEventGroupSetBits(startup_event_group_, kStartupBitFailed);
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        char* content_type = nullptr;
        esp_http_client_get_header(client, "Content-Type", &content_type);
        ESP_LOGI(TAG, "Stream connected: status=%d content_type=%s",
                 status, content_type != nullptr ? content_type : "unknown");
        LogWifiPsStatus("connected");

        std::string st_codec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            st_codec = current_station_.codec;
        }

        std::string st_codec_lower = st_codec;
        for (auto& c : st_codec_lower) c = std::tolower(static_cast<unsigned char>(c));

        std::string ct_lower = content_type != nullptr ? content_type : "";
        for (auto& c : ct_lower) c = std::tolower(static_cast<unsigned char>(c));

        bool is_aac = false;
        if (st_codec_lower.find("aac") != std::string::npos) {
            is_aac = true;
        } else if (st_codec_lower.find("mp3") != std::string::npos) {
            is_aac = false;
        } else {
            // Fallback for direct URL playback when station codec is empty or unknown
            if (ct_lower.find("audio/aac") != std::string::npos ||
                ct_lower.find("audio/aacp") != std::string::npos ||
                ct_lower.find("audio/x-aac") != std::string::npos ||
                ct_lower.find("aac") != std::string::npos) {
                is_aac = true;
            } else if (current_url.find(".aac") != std::string::npos) {
                is_aac = true;
            }
        }

        ESP_LOGI(TAG, "Radio codec selection:\nstation_codec=%s\ncontent_type=%s\nselected_decoder=%s",
                 st_codec.empty() ? "unknown" : st_codec.c_str(),
                 content_type != nullptr ? content_type : "unknown",
                 is_aac ? "AAC" : "MP3");

        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        aac_cfg.aac_plus_enable = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        if (is_aac) {
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            dec_cfg.dec_cfg = &aac_cfg;
            dec_cfg.cfg_size = sizeof(aac_cfg);
        } else {
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }

        esp_audio_simple_dec_handle_t decoder = nullptr;
        const esp_err_t decoder_error = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (decoder_error != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "%s decoder open failed: %d", is_aac ? "AAC" : "MP3", static_cast<int>(decoder_error));
            esp_http_client_close(client); esp_http_client_cleanup(client);
            if (!initial_ready_) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    startup_err_msg_ = "Decoder open failed";
                }
                if (startup_event_group_ != nullptr) {
                    xEventGroupSetBits(startup_event_group_, kStartupBitFailed);
                }
            }
            break;
        }
        std::vector<uint8_t> in(2048);
        std::vector<uint8_t> out(16384);
        std::vector<uint8_t> pending(in.size() * 4);
        size_t pending_len = 0;
        bool first_read_logged = false;
        bool first_frame_logged = false;
        bool invalid_info_logged = false;
        int64_t last_radio_log_time = 0;
        while (!stop_requested_) {
            if (initial_ready_ && (paused_ ||
                Application::GetInstance().GetDeviceState() == kDeviceStateListening ||
                Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            int64_t now = esp_timer_get_time();
            if (now - last_radio_log_time >= 5000000) {
                last_radio_log_time = now;
                LogWifiPsStatus("playing_periodic");
                uint32_t buf_ms = Application::GetInstance().GetAudioService().GetRadioBufferedMs();
                size_t queue_len = Application::GetInstance().GetAudioService().GetRadioQueueSize();
                ESP_LOGI(TAG, "[RADIO] buffer=%lu ms queue=%u underruns=%lu reconnects=%lu dec_err=%lu status=%s",
                         (unsigned long)buf_ms, (unsigned)queue_len,
                         (unsigned long)underrun_count_.load(),
                         (unsigned long)reconnect_count_.load(),
                         (unsigned long)decoder_error_count_.load(),
                         paused_ ? "PAUSED" : "PLAYING");
            }
            int64_t t_read_start = esp_timer_get_time();
            int read = esp_http_client_read(client, reinterpret_cast<char*>(in.data()), in.size());
            int64_t t_read_dur_ms = (esp_timer_get_time() - t_read_start) / 1000;
            if (t_read_dur_ms > 250) {
                ESP_LOGW(TAG, "[RADIO_HTTP_GAP] read_time=%lld ms bytes=%d", t_read_dur_ms, read);
            }
            if (read <= 0) {
                if (!initial_ready_) {
                    ESP_LOGD(TAG, "Waiting for initial stream data... (read=%d)", read);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                ESP_LOGW(TAG, "[RADIO_HTTP_ERROR] read returned %d", read);
                reconnect_count_++;
                break;
            }
            if (!initial_ready_) {
                initial_ready_ = true;
                if (startup_event_group_ != nullptr) {
                    xEventGroupSetBits(startup_event_group_, kStartupBitReady);
                }
            }
            if (!first_read_logged) {
                ESP_LOGI(TAG, "Received first stream data: bytes=%d", read);
                first_read_logged = true;
            }
            if (pending_len + static_cast<size_t>(read) > pending.size()) {
                ESP_LOGW(TAG, "%s decoder made no progress; reconnecting", is_aac ? "AAC" : "MP3");
                reconnect_count_++;
                break;
            }
            std::memcpy(pending.data() + pending_len, in.data(), read);
            pending_len += read;
            size_t consumed_total = 0;
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = pending.data();
            raw.len = pending_len;
            while (raw.len > 0 && !stop_requested_ && !reconnect_requested_) {
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = out.data();
                frame.len = out.size();
                int64_t t_dec_start = esp_timer_get_time();
                esp_err_t result = esp_audio_simple_dec_process(decoder, &raw, &frame);
                int64_t t_dec_dur_ms = (esp_timer_get_time() - t_dec_start) / 1000;
                if (t_dec_dur_ms > 30) {
                    ESP_LOGW(TAG, "[RADIO_DEC_GAP] decode_time=%lld ms decoded_bytes=%d", t_dec_dur_ms, frame.decoded_size);
                }
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    const size_t new_size = frame.needed_size > out.size()
                        ? frame.needed_size
                        : out.size() * 2;
                    out.resize(new_size);
                    continue;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    decoder_error_count_++;
                    ESP_LOGW(TAG, "%s decode failed: ret=%d consumed=%u input=%u",
                             is_aac ? "AAC" : "MP3",
                             static_cast<int>(result),
                             static_cast<unsigned>(raw.consumed),
                             static_cast<unsigned>(raw.len));
                    break;
                }
                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK &&
                        info.sample_rate > 0 && info.bits_per_sample == 16) {
                        if (!first_frame_logged) {
                            ESP_LOGI(TAG, "First %s frame decoded: rate=%lu channels=%lu bits=%lu bytes=%d",
                                     is_aac ? "AAC" : "MP3",
                                     static_cast<unsigned long>(info.sample_rate),
                                     static_cast<unsigned long>(info.channel),
                                     static_cast<unsigned long>(info.bits_per_sample),
                                     static_cast<int>(frame.decoded_size));
                            first_frame_logged = true;
                            SystemInfo::PrintRamSnapshot("RADIO_PLAYING");
                        }
                        PushMediaPcm(codec, reinterpret_cast<int16_t*>(out.data()),
                                     frame.decoded_size / 2, info.channel, info.sample_rate, target_rate, true);
                    } else if (!invalid_info_logged) {
                        ESP_LOGW(TAG, "Decoded frame has unsupported audio info: rate=%lu channels=%lu bits=%lu",
                                 static_cast<unsigned long>(info.sample_rate),
                                 static_cast<unsigned long>(info.channel),
                                 static_cast<unsigned long>(info.bits_per_sample));
                        invalid_info_logged = true;
                    }
                }
                if (raw.consumed == 0) break;
                consumed_total += raw.consumed;
                raw.buffer += raw.consumed; raw.len -= raw.consumed; raw.consumed = 0;
            }
            if (consumed_total > 0) {
                std::memmove(pending.data(), pending.data() + consumed_total,
                             pending_len - consumed_total);
                pending_len -= consumed_total;
            }
        }
        esp_audio_simple_dec_close(decoder);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (reconnect_requested_) {
            reconnect_count_++;
        }
        reconnect_requested_ = false;
        if (!stop_requested_) vTaskDelay(pdMS_TO_TICKS(500));
    }
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    ESP_LOGI(TAG, "[WIFI_PS_RADIO] restored to LOW_POWER (MAX_MODEM)");
    LogWifiPsStatus("stopped");
    Application::GetInstance().GetAudioService().ResetDecoder();
}
