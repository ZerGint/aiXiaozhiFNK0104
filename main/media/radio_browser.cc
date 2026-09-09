#include "radio_browser.h"

#include "application.h"
#include "media_player.h"
#include "mcp_server.h"
#include "radio_storage.h"
#include "settings.h"
#include "radio_memory_diag.h"
#include "radio_json_framer.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "RadioBrowser"

namespace {
constexpr bool RADIO_SEARCH_DIAGNOSTICS_ENABLED = true;
std::atomic<bool> radio_test_running{false};
std::atomic<int> radio_test_step{0};
std::atomic<bool> radio_test_ready{false};
std::string radio_test_uuid;
void ScheduleRadioErrorBip() {
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_RADIO_ERROR);
    });
}

constexpr const char* kFavoritesKey = "favorites";
constexpr size_t kMaxFavorites = 10;

struct RadioSearchResult {
    std::string stationuuid;
    std::string name;
    std::string state;
};
int FavoriteScore(const RadioStationInfo& station, const std::string& query);

#define LogHeapDiag(point) LogRadioMemory(TAG, point)

std::string JsonString(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

bool ContainsInsensitive(const std::string& value, const std::string& query) {
    if (query.empty()) return true;
    if (query.size() > value.size()) return false;
    for (size_t start = 0; start <= value.size() - query.size(); ++start) {
        bool match = true;
        for (size_t i = 0; i < query.size(); ++i) {
            const auto value_char = static_cast<unsigned char>(value[start + i]);
            const auto query_char = static_cast<unsigned char>(query[i]);
            if (std::tolower(value_char) != std::tolower(query_char)) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool EqualsInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](unsigned char ca, unsigned char cb) {
                          return std::tolower(ca) == std::tolower(cb);
                      });
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr &&
        event->data != nullptr && event->data_len > 0) {
        static_cast<std::string*>(event->user_data)->append(
            static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

RadioStationInfo StationInfoFromJson(cJSON* object) {
    RadioStationInfo info;
    if (object == nullptr) return info;
    info.stationuuid = JsonString(object, "stationuuid");
    info.name = JsonString(object, "name");
    info.url_resolved = JsonString(object, "url_resolved");
    info.codec = JsonString(object, "codec");
    cJSON* bitrate_item = cJSON_GetObjectItemCaseSensitive(object, "bitrate");
    if (cJSON_IsNumber(bitrate_item)) {
        info.bitrate = static_cast<uint32_t>(bitrate_item->valuedouble);
    }
    info.country = JsonString(object, "country");
    info.countrycode = JsonString(object, "countrycode");
    info.state = JsonString(object, "state");
    info.language = JsonString(object, "language");
    info.tags = JsonString(object, "tags");
    return info;
}
}

RadioBrowser& RadioBrowser::GetInstance() {
    static RadioBrowser instance;
    return instance;
}

std::string RadioBrowser::UrlEncode(const std::string& value) const {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string RadioBrowser::PerformRequest(const std::string& path) {
    if (mirrors_.empty()) {
        mirrors_ = {
            "http://de1.api.radio-browser.info",
            "http://nl1.api.radio-browser.info",
            "http://at1.api.radio-browser.info",
            "http://fi1.api.radio-browser.info"
        };
    }

    const size_t total_attempts = mirrors_.size();
    for (size_t attempt = 0; attempt < total_attempts; ++attempt) {
        size_t index = (current_mirror_index_ + attempt) % mirrors_.size();
        const std::string& server = mirrors_[index];

        ESP_LOGI(TAG, "[RADIO_MIRROR] trying %s", server.c_str());

        const std::string url = server + path;
        std::string response;
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 5000;
        config.buffer_size = 4096;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = true;
        config.event_handler = HttpEventHandler;
        config.user_data = &response;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ESP_LOGW(TAG, "[RADIO_MIRROR] failed %s reason=client_init_failed", server.c_str());
            continue;
        }
        esp_http_client_set_header(client, "User-Agent", "xiaozhi-esp32-radio/1.0");
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status >= 200 && status < 300) {
            ESP_LOGI(TAG, "[RADIO_MIRROR] success %s", server.c_str());
            current_mirror_index_ = index;
            server_url_ = server;
            ESP_LOGI(TAG, "Search request succeeded: status=%d response_len=%d",
                     status, (int)response.size());
            return response;
        }

        ESP_LOGW(TAG, "[RADIO_MIRROR] failed %s reason=err:%s status:%d",
                 server.c_str(), esp_err_to_name(err), status);

        if (err == ESP_OK && status >= 400) {
            return "{\"error\":\"Radio Browser request failed\"}";
        }
    }

    return "{\"error\":\"Radio Browser request failed\"}";
}

std::string RadioBrowser::PerformOnlineSearch(const std::string& query,
                                                const std::string& countrycode,
                                                const std::string& language,
                                                const std::string& tag,
                                                int limit) {
    limit = std::clamp(limit, 1, 10);
    const int server_limit = std::min(limit * 2, 20);
    if constexpr (RADIO_SEARCH_DIAGNOSTICS_ENABLED) LogHeapDiag("RADIO_SEARCH_BEFORE_HTTP");
    ESP_LOGI(TAG, "[RADIO_SEARCH] query=\"%s\" countrycode=\"%s\" language=\"%s\" tag=\"%s\" limit=%d",
             query.c_str(), countrycode.c_str(), language.c_str(), tag.c_str(), limit);

    std::string path = "/json/stations/search?limit=" + std::to_string(server_limit) +
        "&hidebroken=true&order=clickcount&reverse=true";
    if (!query.empty()) {
        path += "&name=" + UrlEncode(query);
    }
    if (!countrycode.empty()) {
        path += "&countrycode=" + UrlEncode(countrycode);
    }
    if (!language.empty()) {
        path += "&language=" + UrlEncode(language);
    }
    if (!tag.empty()) {
        path += "&tag=" + UrlEncode(tag);
    }

    std::string raw = PerformRequest(path);
    if constexpr (RADIO_SEARCH_DIAGNOSTICS_ENABLED) LogHeapDiag("RADIO_SEARCH_AFTER_HTTP");
    LogHeapDiag("after_http_response");

    std::vector<RadioSearchResult> catalog_stations;
    catalog_stations.reserve(static_cast<size_t>(limit));
    int server_objects = 0;
    int accepted_codec = 0;
    int rejected_codec = 0;
    bool stopped_after_limit = false;
    const bool valid = ForEachJsonObject(raw, [&](std::string_view object) {
        ++server_objects;
        if (catalog_stations.size() >= static_cast<size_t>(limit)) {
            stopped_after_limit = true;
            return true;
        }
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> station(
            cJSON_ParseWithLength(object.data(), object.size()), &cJSON_Delete);
        if (station == nullptr || !cJSON_IsObject(station.get())) return false;
        std::string codec_str = JsonString(station.get(), "codec");
        std::string codec_lower = codec_str;
        std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool is_supported = (codec_lower.find("mp3") != std::string::npos ||
                                   codec_lower.find("aac") != std::string::npos);
        if (is_supported) {
            ++accepted_codec;
            ESP_LOGI(TAG, "RadioBrowser: accepted codec %s", codec_str.c_str());
            catalog_stations.push_back({JsonString(station.get(), "stationuuid"),
                                        JsonString(station.get(), "name"),
                                        JsonString(station.get(), "state")});
            if (catalog_stations.size() >= static_cast<size_t>(limit)) stopped_after_limit = true;
        } else {
            ++rejected_codec;
            ESP_LOGD(TAG, "RadioBrowser: rejected unsupported codec %s", codec_str.c_str());
        }
        return true;
    });
    if (!valid) {
        ESP_LOGI(TAG, "[RADIO_SEARCH_DIAG] user_limit=%d server_limit=%d http_response_bytes=%u server_objects=0 accepted_codec=0 rejected_codec=0 returned_results=0 stopped_after_limit=NO", limit, server_limit, static_cast<unsigned>(raw.size()));
        return raw;
    }
    if constexpr (RADIO_SEARCH_DIAGNOSTICS_ENABLED) LogHeapDiag("RADIO_SEARCH_AFTER_PARSE");

    LogHeapDiag("after_parse_filter");
    if constexpr (RADIO_SEARCH_DIAGNOSTICS_ENABLED) {
        ESP_LOGI(TAG, "[RADIO_SEARCH_DIAG] user_limit=%d server_limit=%d http_response_bytes=%u server_objects=%d accepted_codec=%d rejected_codec=%d returned_results=%u stopped_after_limit=%s", limit, server_limit, static_cast<unsigned>(raw.size()), server_objects, accepted_codec, rejected_codec, static_cast<unsigned>(catalog_stations.size()), stopped_after_limit ? "YES" : "NO");
    }

    // Free the heavy cJSON root tree and raw HTTP response string IMMEDIATELY
    raw.clear();
    raw.shrink_to_fit();

    LogHeapDiag("after_free_root_raw");

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> presentation(cJSON_CreateArray(), &cJSON_Delete);
    if (presentation == nullptr) return "[]";
    for (const auto& info : catalog_stations) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", info.name.c_str());
        cJSON_AddStringToObject(item, "stationuuid", info.stationuuid.c_str());
        cJSON_AddStringToObject(item, "state", info.state.c_str());
        if (item == nullptr || !cJSON_AddItemToArray(presentation.get(), item)) {
            cJSON_Delete(item);
            return "[]";
        }
    }

    char* output = cJSON_PrintUnformatted(presentation.get());
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    if constexpr (RADIO_SEARCH_DIAGNOSTICS_ENABLED) LogHeapDiag("RADIO_SEARCH_AFTER_OUTPUT");
    return response;
}

bool RadioBrowser::GetStationByUuid(const std::string& stationuuid,
                                     RadioStationInfo& station,
                                     std::string& err_msg) {
    if (stationuuid.empty()) {
        err_msg = "Station UUID is empty";
        return false;
    }

    ESP_LOGI(TAG, "Looking up radio station by UUID: %s", stationuuid.c_str());

    const std::string path = "/json/stations/byuuid/" + UrlEncode(stationuuid);
    const std::string raw = PerformRequest(path);

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(raw.c_str()), &cJSON_Delete);
    if (root == nullptr || !cJSON_IsArray(root.get())) {
        err_msg = "Failed to parse RadioBrowser response";
        ESP_LOGW(TAG, "GetStationByUuid failed: invalid JSON for UUID %s", stationuuid.c_str());
        return false;
    }

    if (cJSON_GetArraySize(root.get()) == 0) {
        err_msg = "Station not found by UUID";
        ESP_LOGW(TAG, "GetStationByUuid: station not found for UUID %s", stationuuid.c_str());
        return false;
    }

    cJSON* item = cJSON_GetArrayItem(root.get(), 0);
    RadioStationInfo parsed_info = StationInfoFromJson(item);

    std::string codec_lower = parsed_info.codec;
    std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool is_supported = (codec_lower.find("mp3") != std::string::npos ||
                               codec_lower.find("aac") != std::string::npos);

    if (!is_supported) {
        err_msg = "Unsupported station codec: " + parsed_info.codec;
        ESP_LOGW(TAG, "GetStationByUuid: rejected unsupported codec %s for UUID %s",
                 parsed_info.codec.c_str(), stationuuid.c_str());
        return false;
    }

    if (parsed_info.url_resolved.empty()) {
        err_msg = "Resolved stream URL is empty";
        ESP_LOGW(TAG, "GetStationByUuid: url_resolved empty for UUID %s", stationuuid.c_str());
        return false;
    }

    station = parsed_info;
    return true;
}

namespace {
constexpr int kLocalCatalogMinResults = 3;
bool HasNameToken(const std::string& name, const std::string& query) {
    std::istringstream stream(query);
    std::string token;
    while (stream >> token) if (ContainsInsensitive(name, token)) return true;
    return query.empty();
}

} // namespace

void RadioBrowser::TestOnlineSearch() {
    // TEMPORARY: binary storage end-to-end diagnostic.
    const int step = radio_test_step.load() + 1;
    if (step > 10) { ESP_LOGI(TAG, "[RADIO_TEST] binary sequence complete"); return; }
    bool expected = false;
    if (!radio_test_running.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "[RADIO_TEST] busy step=%d", radio_test_step.load() + 1); return;
    }
    radio_test_step.store(step);
    ESP_LOGI(TAG, "TEST_BINARY step=%d BEFORE", step);
    if (xTaskCreate([](void* arg) {
        auto* browser = static_cast<RadioBrowser*>(arg);
        const int step = radio_test_step.load();
        constexpr const char* kUuid = "01b61e49-18bd-486d-b0e1-cb51cbaf9a6d";
        bool ok = true; std::string error;
        if (step == 1 || step == 10) {
            const std::string response = browser->PlayStation("", "", kUuid);
            ok = response.find("Playing internet radio:") != std::string::npos;
            radio_test_uuid = kUuid; radio_test_ready.store(ok);
        } else if (step == 2 || step == 3 || step == 6) {
            const std::string response = browser->AddFavorite();
            ESP_LOGI(TAG, "[RADIO_TEST] favorite_result=%s", response.c_str());
            ok = (step == 3) ? response.find("already") != std::string::npos : (step == 2 ? response.find("added") != std::string::npos : response.find("No current") != std::string::npos);
        } else if (step == 4 || step == 9) {
            ESP_LOGI(TAG, "[RADIO_TEST] favorites=%s", browser->ListFavorites().c_str());
        } else if (step == 5) {
            InternetRadioPlayer::GetInstance().Stop();
        } else if (step == 7) {
            ok = browser->PlayFavorite(kUuid).find("Playing favorite") != std::string::npos;
        } else if (step == 8) {
            ok = browser->RemoveFavorite(kUuid).find("removed") != std::string::npos;
        }
        LogRadioMemory(TAG, "TEST_BINARY_AFTER");
        ESP_LOGI(TAG, "[RADIO_TEST] STEP %d/10 %s", step, ok ? "PASS" : "FAIL");
        radio_test_running.store(false); vTaskDelete(nullptr);
    }, "RadioTest", 6144, this, 3, nullptr) != pdPASS) {
        radio_test_running.store(false); ESP_LOGE(TAG, "[RADIO_TEST] task creation failed");
    }
}

std::string RadioBrowser::SearchStations(const std::string& query,
                                          const std::string& countrycode,
                                          const std::string& language,
                                          const std::string& tag,
                                          int limit,
                                          bool force_online) {
    limit = std::clamp(limit, 1, 10);

    if (force_online) {
        ESP_LOGI(TAG, "Force-online radio search: bypassing local catalog");
        return PerformOnlineSearch(query, countrycode, language, tag, limit);
    }

    const int required = std::min(limit, kLocalCatalogMinResults);

    std::vector<RadioStationInfo> local_results =
        RadioStorage::GetInstance().SearchCatalog(query, countrycode, language, tag, limit);

    bool meaningful_local = false;
    if constexpr (RadioSearchRanking::Enabled) {
        for (const auto& station : local_results) {
            if (HasNameToken(station.name, query)) { meaningful_local = true; break; }
        }
    } else {
        meaningful_local = static_cast<int>(local_results.size()) >= required;
    }
    if (meaningful_local && (!RadioSearchRanking::Enabled || !local_results.empty())) {
        ESP_LOGI(TAG, "Local radio catalog: %d matches. Using local radio catalog results",
                 static_cast<int>(local_results.size()));
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateArray(), &cJSON_Delete);
        if (root == nullptr) return "[]";
        for (const auto& info : local_results) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", info.name.c_str());
            cJSON_AddStringToObject(item, "stationuuid", info.stationuuid.c_str());
            cJSON_AddStringToObject(item, "country", info.country.c_str());
            cJSON_AddStringToObject(item, "state", info.state.c_str());
            if (item == nullptr || !cJSON_AddItemToArray(root.get(), item)) {
                cJSON_Delete(item);
                return "[]";
            }
        }
        char* output = cJSON_PrintUnformatted(root.get());
        std::string response = output != nullptr ? output : "[]";
        if (output != nullptr) cJSON_free(output);
        return response;
    }

    ESP_LOGI(TAG, "Local catalog insufficient (%d matches), querying RadioBrowser online",
             static_cast<int>(local_results.size()));

    return PerformOnlineSearch(query, countrycode, language, tag, limit);
}

std::string RadioBrowser::PlayStation(const std::string& url, const std::string& title, const std::string& station_uuid) {
    if (!station_uuid.empty()) {
        ESP_LOGI(TAG, "Playing radio by station UUID: %s", station_uuid.c_str());
        RadioStationInfo cached_station;
        if (RadioStorage::GetInstance().GetCatalogStationByUuid(station_uuid, cached_station) &&
            !cached_station.url_resolved.empty()) {
            std::string play_err;
            auto retry = [station_uuid]() {
                Application::GetInstance().Schedule([station_uuid]() {
                    RadioStationInfo fresh_station;
                    std::string err_msg;
                    if (!RadioBrowser::GetInstance().GetStationByUuid(station_uuid, fresh_station, err_msg)) {
                        ScheduleRadioErrorBip();
                        return;
                    }
                    std::string play_err;
                    auto admission = [station = fresh_station]() mutable {
                        Application::GetInstance().Schedule([station = std::move(station)]() mutable {
                            std::string err;
                            RadioStorage::GetInstance().AddOrUpdateCatalogStation(station, err);
                        });
                    };
                    if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err,
                                                               std::move(admission), {}, true)) {
                        Application::GetInstance().Schedule([]() {
                            Application::GetInstance().PlaySound(Lang::Sounds::OGG_RADIO_ERROR);
                        });
                    }
                });
            };
            if (!MediaPlayer::GetInstance().PlayRadio(cached_station, play_err, {},
                                                       std::move(retry), false)) {
                return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
            }
            return "Playing internet radio: " + (cached_station.name.empty() ? title : cached_station.name);
        }
        RadioStationInfo fresh_station;
        std::string err_msg;
        if (!GetStationByUuid(station_uuid, fresh_station, err_msg)) {
            ScheduleRadioErrorBip();
            return "Failed to resolve radio station: " + err_msg;
        }

        ESP_LOGI(TAG, "Resolved radio station by UUID: %s", fresh_station.name.c_str());

        std::string play_err;
        auto admission = [station = fresh_station]() mutable {
            ESP_LOGI(TAG, "[RADIO_ADMISSION_DIAG] callback uuid=%s", station.stationuuid.c_str());
            Application::GetInstance().Schedule([station = std::move(station)]() mutable {
                ESP_LOGI(TAG, "[RADIO_ADMISSION_DIAG] scheduled_begin uuid=%s", station.stationuuid.c_str());
                std::string err;
                const bool ok = RadioStorage::GetInstance().AddOrUpdateCatalogStation(station, err);
                ESP_LOGI(TAG, "[RADIO_ADMISSION_DIAG] storage_result uuid=%s ok=%d err=%s",
                         station.stationuuid.c_str(), ok ? 1 : 0, err.c_str());
                if (!ok) {
                    ESP_LOGW(TAG, "Failed to refresh radio catalog after startup: %s", err.c_str());
                }
            });
        };
        if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err, std::move(admission))) {
            return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
        }
        return "Playing internet radio: " + (fresh_station.name.empty() ? title : fresh_station.name);
    }

    if (url.empty()) {
        return "Station URL or stationuuid is required";
    }

    std::string play_err;
    if (!MediaPlayer::GetInstance().PlayRadio(url, title, play_err)) {
        return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
    }
    return "Playing internet radio";
}

std::string RadioBrowser::AddFavorite() {
    const auto& radio = InternetRadioPlayer::GetInstance();
    if (!radio.IsPlaying()) return "No current radio station is available";
    const auto current = radio.GetCurrentStation();
    if (current.stationuuid.empty()) return "Current radio station has no UUID";
    return RadioStorage::GetInstance().AddFavoriteUuid(current.stationuuid);
/*
    RadioStationInfo target_station;
    target_station.name = name;
    target_station.country = country;
    target_station.state = city;
    target_station.tags = keywords;
    target_station.stationuuid = station_uuid;

    if (target_station.name.empty()) {
        const RadioStationInfo current = InternetRadioPlayer::GetInstance().GetCurrentStation();
        if (current.name.empty() && current.url_resolved.empty()) {
            return "No current radio station is available";
        }
        target_station = current;
        if (!country.empty()) target_station.country = country;
        if (!city.empty()) target_station.state = city;
        if (!station_uuid.empty()) target_station.stationuuid = station_uuid;
        if (!keywords.empty()) target_station.tags = keywords;
    }

    if (target_station.name.empty()) {
        return "No current radio station is available";
    }

    return RadioStorage::GetInstance().AddFavoriteUuid(target_station.stationuuid);
*/
}

std::string RadioBrowser::ListFavorites() const {
    return RadioStorage::GetInstance().ListFavorites();
}

std::string RadioBrowser::PlayFavorite(const std::string& station_uuid) {
    if (station_uuid.empty() || !RadioStorage::GetInstance().ContainsFavoriteUuid(station_uuid)) return "Favorite station not found";
    RadioStationInfo target_favorite;
    target_favorite.stationuuid = station_uuid;

    ESP_LOGI(TAG, "[FAVORITE_PLAY] name=%s uuid=%s", target_favorite.name.c_str(), target_favorite.stationuuid.c_str());
    bool uuid_lookup_failed = false;

    if (!target_favorite.stationuuid.empty()) {
        ESP_LOGI(TAG, "[FAVORITE_PLAY] resolving by UUID");
        RadioStationInfo cached_station;
        const bool catalog_found = RadioStorage::GetInstance().GetCatalogStationByUuid(target_favorite.stationuuid, cached_station);
        ESP_LOGI(TAG, "[RADIO_ADMISSION_DIAG] favorite_catalog uuid=%s found=%d url_empty=%d",
                 target_favorite.stationuuid.c_str(), catalog_found ? 1 : 0,
                 cached_station.url_resolved.empty() ? 1 : 0);
        if (catalog_found && !cached_station.url_resolved.empty()) {
            std::string play_err;
            const std::string station_uuid = target_favorite.stationuuid;
            auto retry = [station_uuid]() {
                Application::GetInstance().Schedule([station_uuid]() {
                    RadioStationInfo fresh_station;
                    std::string err_msg;
                    if (!RadioBrowser::GetInstance().GetStationByUuid(station_uuid, fresh_station, err_msg)) {
                        ScheduleRadioErrorBip();
                        return;
                    }
                    std::string play_err;
                    auto admission = [station = fresh_station]() mutable {
                        Application::GetInstance().Schedule([station = std::move(station)]() mutable {
                            std::string err;
                            RadioStorage::GetInstance().AddOrUpdateCatalogStation(station, err);
                        });
                    };
                    if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err,
                                                               std::move(admission), {}, true)) {
                        Application::GetInstance().Schedule([]() {
                            Application::GetInstance().PlaySound(Lang::Sounds::OGG_RADIO_ERROR);
                        });
                    }
                });
            };
            if (!MediaPlayer::GetInstance().PlayRadio(cached_station, play_err, {},
                                                       std::move(retry), false)) {
                return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
            }
            return "Playing favorite station: " + cached_station.name;
        }
        RadioStationInfo fresh_station;
        std::string err_msg;
        if (GetStationByUuid(target_favorite.stationuuid, fresh_station, err_msg)) {
            ESP_LOGI(TAG, "[FAVORITE_PLAY] resolved fresh station by UUID");
            std::string play_err;
            auto admission = [station = fresh_station]() mutable {
                Application::GetInstance().Schedule([station = std::move(station)]() mutable {
                    std::string err;
                    RadioStorage::GetInstance().AddOrUpdateCatalogStation(station, err);
                });
            };
            if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err, std::move(admission))) {
                return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
            }
            return "Playing favorite station: " + fresh_station.name;
        }
        uuid_lookup_failed = true;
    }

    if (uuid_lookup_failed) ScheduleRadioErrorBip();
    return "Favorite station has no valid URL";
}

namespace {
int FavoriteScore(const RadioStationInfo& station, const std::string& query) {
    std::istringstream stream(query);
    std::string token;
    int score = 0;
    while (stream >> token) {
        if (ContainsInsensitive(station.name, token)) score += RadioSearchRanking::NameWeight;
        else if (ContainsInsensitive(station.state, token)) score += RadioSearchRanking::StateWeight;
        else if (ContainsInsensitive(station.tags, token) || ContainsInsensitive(station.country, token)) score += RadioSearchRanking::MetadataWeight;
    }
    if (!query.empty() && ContainsInsensitive(station.name, query)) score += RadioSearchRanking::FullNameBonus;
    return score;
}
} // namespace

std::string RadioBrowser::RemoveFavorite(const std::string& station_uuid) {
    return RadioStorage::GetInstance().RemoveFavoriteUuid(station_uuid);
}

void RadioBrowser::RegisterMcpTools() {
    McpServer::GetInstance().AddTool(
        "radio.add_favorite",
        "Add the currently playing radio station to favorites. No arguments are required.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            return AddFavorite();
        });
    McpServer::GetInstance().AddTool(
        "radio.get_current",
        "Get information about the currently playing or last played internet radio station.\n"
        "Use this tool when the user asks:\n"
        "- What radio station is currently playing?\n"
        "- What is the name of the current radio station?\n"
        "- What radio am I listening to?\n"
        "- Show details or information about the current internet radio station.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& radio = InternetRadioPlayer::GetInstance();
            const RadioStationInfo station = radio.GetCurrentStation();
            const bool active = radio.IsActive();
            const bool playing = radio.IsPlaying();
            const bool paused = radio.IsPaused();

            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "active", active);
            cJSON_AddBoolToObject(root, "playing", playing);
            cJSON_AddBoolToObject(root, "paused", paused);

            if (station.name.empty() && station.url_resolved.empty()) {
                cJSON_AddNullToObject(root, "station");
            } else {
                cJSON_AddStringToObject(root, "name", station.name.c_str());
                cJSON_AddStringToObject(root, "stationuuid", station.stationuuid.c_str());
                cJSON_AddStringToObject(root, "url_resolved", station.url_resolved.c_str());
                cJSON_AddStringToObject(root, "codec", station.codec.c_str());
                cJSON_AddNumberToObject(root, "bitrate", station.bitrate);
                cJSON_AddStringToObject(root, "country", station.country.c_str());
                cJSON_AddStringToObject(root, "countrycode", station.countrycode.c_str());
                cJSON_AddStringToObject(root, "state", station.state.c_str());
                cJSON_AddStringToObject(root, "language", station.language.c_str());
                cJSON_AddStringToObject(root, "tags", station.tags.c_str());
            }

            char* output = cJSON_PrintUnformatted(root);
            std::string response = output != nullptr ? output : "{}";
            if (output != nullptr) cJSON_free(output);
            cJSON_Delete(root);
            return response;
        });
    McpServer::GetInstance().AddTool(
        "radio.list_favorites",
        "Return hidden favorite radio stations for internal selection.\n"
        "Usage guidelines:\n"
        "- When the user asks to play a specific radio station by name (e.g. 'Включи Radius FM', 'Поставь Ретро FM', 'Хочу послушать Юмор FM'), call radio.list_favorites FIRST before searching.\n"
        "- Treat minor name variations as equivalent: spaces, hyphens, case, Cyrillic/Latin variants when obvious from context (e.g. 'Ретро-ФМ' ≈ 'Ретро FM' ≈ 'Retro FM', 'Радиус ФМ' ≈ 'Radius FM').\n"
        "- If a clear match is found in favorites, use radio.play_favorite. Do NOT call radio.search_stations if the requested station is already present in favorites.\n"
        "- Do not read every favorite station aloud to the user unless explicitly asked; summarize briefly.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return ListFavorites();
        });
    McpServer::GetInstance().AddTool(
        "radio.play_favorite",
        "Play a hidden favorite station by its stationuuid from radio.list_favorites.",
        PropertyList({
            Property("stationuuid", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!properties.HasProperty("stationuuid")) {
                return "Favorite station UUID is required";
            }
            const std::string uuid = properties["stationuuid"].value<std::string>();
            if (uuid.empty()) {
                return "Favorite station UUID is required";
            }
            return PlayFavorite(uuid);
        });
    McpServer::GetInstance().AddTool(
        "radio.remove_favorite",
        "Remove a hidden favorite radio station by stationuuid.",
        PropertyList({
            Property("stationuuid", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!properties.HasProperty("stationuuid")) return "Favorite station UUID is required";
            return RemoveFavorite(properties["stationuuid"].value<std::string>());
        });
    McpServer::GetInstance().AddTool(
        "radio.search_stations",
        "Search radio stations and return candidate discovery results.\n"
        "Present each candidate as name — state when state is non-empty, otherwise name.\n"
        "Use stationuuid to identify the selected station; do not require country in the result.\n"
        "Usage guidelines:\n"
        "- When the user asks to play a SPECIFIC named station (e.g. 'Включи Radius FM', 'Поставь Юмор FM', 'Хочу послушать Ретро FM'):\n"
        "  1. Check radio.list_favorites first. If present in favorites, use radio.play_favorite.\n"
        "  2. If not in favorites, call radio.search_stations with query='<station name>', limit=1, force_online=false.\n"
        "     Setting limit=1 allows a single local catalog match to satisfy the request without triggering an unnecessary online search.\n"
        "- When the user asks a BROWSE or LISTING request for multiple stations (e.g. 'Какие есть станции Беларуси?', 'Найди популярные станции Минска', 'Покажи радио в жанре rock'):\n"
        "  Use limit=5 or limit=10 (do NOT restrict browse/listing requests to limit=1).\n"
        "- Filter usage:\n"
        "  * Use countrycode for country-specific requests (e.g. 'RU' for Russian, 'BY' for Belarusian, 'PL' for Polish, 'US' for USA).\n"
        "  * Use tag for genres or topics (e.g. 'rock', 'jazz', 'retro', 'pop', 'news').\n"
        "  * Use language for language-based requests (e.g. 'russian', 'english').\n"
        "  * Use query for a specific station name or title fragment.\n"
        "  * Set force_online=true ONLY when the user explicitly asks for new, fresh, updated, additional, or internet/online radio stations (e.g. 'Search for new stations online', 'Refresh radio list', 'Search internet for more'). Default is false (local-first search).\n"
        "  * Combine filters when the user specifies multiple constraints.\n"
        "When presenting search results, use name and state to distinguish stations with similar names. After the user chooses a result, call radio.play_station with that result's stationuuid. Do not read URLs, codecs, bitrates, tags, languages, or other technical metadata unless explicitly requested.",
        PropertyList({
            Property("query", kPropertyTypeString, std::string("")),
            Property("countrycode", kPropertyTypeString, std::string("")),
            Property("language", kPropertyTypeString, std::string("")),
            Property("tag", kPropertyTypeString, std::string("")),
            Property("limit", kPropertyTypeInteger, 5, 1, 10),
            Property("force_online", kPropertyTypeBoolean, false)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string query = properties.HasProperty("query") ? properties["query"].value<std::string>() : "";
            const std::string countrycode = properties.HasProperty("countrycode") ? properties["countrycode"].value<std::string>() : "";
            const std::string language = properties.HasProperty("language") ? properties["language"].value<std::string>() : "";
            const std::string tag = properties.HasProperty("tag") ? properties["tag"].value<std::string>() : "";
            const int limit = properties.HasProperty("limit") ? properties["limit"].value<int>() : 5;
            const bool force_online = properties.HasProperty("force_online") ? properties["force_online"].value<bool>() : false;
            return SearchStations(query, countrycode, language, tag, limit, force_online);
        });
    McpServer::GetInstance().AddTool(
        "radio.play_station",
        "Play a selected Radio Browser station. Prefer stationuuid from radio.search_stations because it uniquely identifies the chosen station; do not guess by name when the UUID is known. A direct URL may be used when no stationuuid is available.",
        PropertyList({
           Property("url", kPropertyTypeString, std::string("")),
           Property("title", kPropertyTypeString, std::string("")),
           Property("stationuuid", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
           const std::string url = properties.HasProperty("url") ? properties["url"].value<std::string>() : "";
           const std::string title = properties.HasProperty("title") ? properties["title"].value<std::string>() : "";
           const std::string stationuuid = properties.HasProperty("stationuuid") ? properties["stationuuid"].value<std::string>() : "";
           return PlayStation(url, title, stationuuid);
        });
    McpServer::GetInstance().AddTool(
        "radio.stop",
        "Stop the currently playing internet radio station.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().Stop();
           return "Internet radio stopped";
        });
    McpServer::GetInstance().AddTool(
        "media.play_sd",
        "Play a music track from the SD card. Use query to match artist or title in the filename. "
        "If using index, it is one-based: 1 means the first track.",
        PropertyList({
           Property("index", kPropertyTypeInteger, -1),
           Property("query", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
           MediaPlayer::GetInstance().ScanSd();
           const std::string query = properties["query"].value<std::string>();
           int index = properties["index"].value<int>();
           if (index > 0) --index;
           if (!query.empty()) index = MediaPlayer::GetInstance().FindSdTrack(query);
           if (index < 0) return "No matching SD music track found";
           MediaPlayer::GetInstance().PlaySd(index);
           return "Playing music from the SD card";
        });
    McpServer::GetInstance().AddTool(
        "media.list_sd",
        "List music tracks available on the SD card by filename.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().ScanSd();
           return MediaPlayer::GetInstance().ListSdTracks();
        });
    McpServer::GetInstance().AddTool(
        "media.search_sd",
        "Search SD card tracks by text in the filename. No internet metadata lookup is used.",
        PropertyList({
           Property("artist", kPropertyTypeString, std::string("")),
           Property("genre", kPropertyTypeString, std::string("")),
           Property("limit", kPropertyTypeInteger, 10, 1, 20)
        }),
        [](const PropertyList& properties) -> ReturnValue {
           MediaPlayer::GetInstance().ScanSd();
           return MediaPlayer::GetInstance().SearchSdTracks(
               properties["artist"].value<std::string>(),
               properties["genre"].value<std::string>(),
               properties["limit"].value<int>());
        });
    McpServer::GetInstance().AddTool(
        "media.pause",
        "Pause or resume the currently playing SD music or internet radio. "
        "Use this when the user asks to pause, continue, or resume music or radio.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().TogglePlayPause();
           return "Media playback toggled";
        });
    McpServer::GetInstance().AddTool(
        "media.next",
        "Play the next SD music track.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().Next();
           return "Next track selected";
        });
    McpServer::GetInstance().AddTool(
        "media.previous",
        "Play the previous SD music track.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().Prev();
           return "Previous track selected";
        });
    McpServer::GetInstance().AddTool(
        "media.stop",
        "Stop all SD music and internet radio playback.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
           MediaPlayer::GetInstance().Stop();
           return "Media playback stopped";
        });
}
