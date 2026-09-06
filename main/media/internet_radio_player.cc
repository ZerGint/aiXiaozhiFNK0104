#include "internet_radio_player.h"

#include "application.h"
#include "audio_manager.h"
#include "board.h"
#include "audio_codec.h"
#include "media_audio_output.h"
#include "system_info.h"

#include <decoder/impl/esp_mp3_dec.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <simple_dec/esp_audio_simple_dec_default.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <vector>

#define TAG "InternetRadio"

InternetRadioPlayer& InternetRadioPlayer::GetInstance() {
    static InternetRadioPlayer instance;
    return instance;
}

InternetRadioPlayer::~InternetRadioPlayer() {
    Stop();
}

std::string InternetRadioPlayer::GetTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_.empty() ? "Internet radio" : title_;
}

std::string InternetRadioPlayer::GetUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

void InternetRadioPlayer::Play(const std::string& url, const std::string& title) {
    if (url.empty()) return;
    SystemInfo::PrintRamSnapshot("RADIO_START");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        title_ = title;
    }
    if (playing_) {
        reconnect_requested_ = true;
        paused_ = false;
        return;
    }
    stop_requested_ = false;
    reconnect_requested_ = false;
    paused_ = false;
    playing_ = true;
    if (xTaskCreatePinnedToCore(TaskFunction, "InternetRadio", 6144, this, 3,
                                &task_handle_, 1) != pdPASS) {
        playing_ = false;
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "unable to create stream task");
    }
}

void InternetRadioPlayer::TogglePlayPause() {
    if (!playing_) return;
    paused_ = !paused_;
}

void InternetRadioPlayer::Stop() {
    SystemInfo::PrintRamSnapshot("RADIO_STOP");
    stop_requested_ = true;
    paused_ = false;
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
    auto* codec = Board::GetInstance().GetAudioCodec();
    uint32_t target_rate = codec ? codec->output_sample_rate() : 24000;
    if (target_rate == 0) target_rate = 24000;

    while (!stop_requested_) {
        std::string url;
        { std::lock_guard<std::mutex> lock(mutex_); url = url_; }
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 2000;
        config.buffer_size = 4096;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = true;
        auto client = esp_http_client_init(&config);
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        // Metadata interleaving is not MP3 data and would confuse the decoder.
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "User-Agent", "xiaozhi-esp32-radio/1.0");
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open stream: %s", url.c_str());
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        char* content_type = nullptr;
        esp_http_client_get_header(client, "Content-Type", &content_type);
        ESP_LOGI(TAG, "Stream connected: status=%d content_type=%s",
                 status, content_type != nullptr ? content_type : "unknown");
        if (status < 200 || status >= 400) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        esp_audio_simple_dec_handle_t decoder = nullptr;
        const esp_err_t decoder_error = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (decoder_error != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "MP3 decoder open failed: %d", static_cast<int>(decoder_error));
            esp_http_client_close(client); esp_http_client_cleanup(client); break;
        }
        std::vector<uint8_t> in(2048);
        std::vector<uint8_t> out(16384);
        std::vector<uint8_t> pending(in.size() * 4);
        size_t pending_len = 0;
        bool first_read_logged = false;
        bool first_frame_logged = false;
        bool invalid_info_logged = false;
        int64_t last_radio_log_time = 0;
        while (!stop_requested_ && !reconnect_requested_) {
            if (paused_ ||
                Application::GetInstance().GetDeviceState() == kDeviceStateListening ||
                Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            int64_t now = esp_timer_get_time();
            if (now - last_radio_log_time >= 5000000) {
                last_radio_log_time = now;
                uint32_t buf_ms = Application::GetInstance().GetAudioService().GetRadioBufferedMs();
                size_t queue_len = Application::GetInstance().GetAudioService().GetRadioQueueSize();
                ESP_LOGI(TAG, "[RADIO] buffer=%lu ms queue=%u underruns=%lu reconnects=%lu dec_err=%lu status=%s",
                         (unsigned long)buf_ms, (unsigned)queue_len,
                         (unsigned long)underrun_count_.load(),
                         (unsigned long)reconnect_count_.load(),
                         (unsigned long)decoder_error_count_.load(),
                         paused_ ? "PAUSED" : "PLAYING");
            }
            int read = esp_http_client_read(client, reinterpret_cast<char*>(in.data()), in.size());
            if (read <= 0) {
                reconnect_count_++;
                break;
            }
            if (!first_read_logged) {
                ESP_LOGI(TAG, "Received first stream data: bytes=%d", read);
                first_read_logged = true;
            }
            if (pending_len + static_cast<size_t>(read) > pending.size()) {
                ESP_LOGW(TAG, "MP3 decoder made no progress; reconnecting");
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
                esp_err_t result = esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    const size_t new_size = frame.needed_size > out.size()
                        ? frame.needed_size
                        : out.size() * 2;
                    out.resize(new_size);
                    continue;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    decoder_error_count_++;
                    ESP_LOGW(TAG, "MP3 decode failed: ret=%d consumed=%u input=%u",
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
                            ESP_LOGI(TAG, "First MP3 frame decoded: rate=%lu channels=%lu bits=%lu bytes=%d",
                                     static_cast<unsigned long>(info.sample_rate),
                                     static_cast<unsigned long>(info.channel),
                                     static_cast<unsigned long>(info.bits_per_sample),
                                     static_cast<int>(frame.decoded_size));
                            first_frame_logged = true;
                            SystemInfo::PrintRamSnapshot("RADIO_PLAYING");
                        }
                        PushMediaPcm(codec, reinterpret_cast<int16_t*>(out.data()),
                                     frame.decoded_size / 2, info.channel, info.sample_rate, target_rate);
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
}
