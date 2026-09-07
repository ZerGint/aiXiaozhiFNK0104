#include "radio_browser.h"

#include "media_player.h"
#include "mcp_server.h"
#include "radio_storage.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>

#define TAG "RadioBrowser"

namespace {
constexpr const char* kFavoritesKey = "favorites";
constexpr size_t kMaxFavorites = 10;

static void LogHeapDiag(const char* stage) {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "[HEAP_DIAG] stage=%s internal_free=%zu internal_largest=%zu dma_free=%zu",
             stage, internal_free, internal_largest, dma_free);
}

std::string JsonString(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : "";
}

bool ContainsInsensitive(const std::string& value, const std::string& query) {
    if (query.empty()) return true;
    std::string lower_value = value;
    std::string lower_query = query;
    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower_value.find(lower_query) != std::string::npos;
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
    LogHeapDiag("after_http_response");

    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return raw;
    }

    std::vector<RadioStationInfo> catalog_stations;
    cJSON* station = nullptr;
    cJSON_ArrayForEach(station, root) {
        std::string codec_str = JsonString(station, "codec");
        std::string codec_lower = codec_str;
        std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool is_supported = (codec_lower.find("mp3") != std::string::npos ||
                                   codec_lower.find("aac") != std::string::npos);

        if (is_supported) {
            ESP_LOGI(TAG, "RadioBrowser: accepted codec %s", codec_str.c_str());
            catalog_stations.push_back(StationInfoFromJson(station));
            if (catalog_stations.size() >= static_cast<size_t>(limit)) {
                break;
            }
        } else {
            ESP_LOGD(TAG, "RadioBrowser: rejected unsupported codec %s", codec_str.c_str());
        }
    }

    LogHeapDiag("after_parse_filter");

    // Free the heavy cJSON root tree and raw HTTP response string IMMEDIATELY
    cJSON_Delete(root);
    root = nullptr;
    raw.clear();
    raw.shrink_to_fit();

    LogHeapDiag("after_free_root_raw");

    if (!catalog_stations.empty()) {
        LogHeapDiag("before_add_catalog");
        std::string err_msg;
        if (!RadioStorage::GetInstance().AddOrUpdateCatalogStations(catalog_stations, err_msg)) {
            ESP_LOGW(TAG, "Failed to update radio catalog: %s", err_msg.c_str());
        } else {
            ESP_LOGI(TAG, "Radio catalog updated: %d search results", static_cast<int>(catalog_stations.size()));
        }
        LogHeapDiag("after_add_catalog");
    }

    cJSON* presentation = cJSON_CreateArray();
    for (const auto& info : catalog_stations) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", info.name.c_str());
        cJSON_AddStringToObject(item, "stationuuid", info.stationuuid.c_str());
        cJSON_AddStringToObject(item, "country", info.country.c_str());
        cJSON_AddStringToObject(item, "state", info.state.c_str());
        cJSON_AddItemToArray(presentation, item);
    }

    char* output = cJSON_PrintUnformatted(presentation);
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(presentation);
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

    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root != nullptr) cJSON_Delete(root);
        err_msg = "Failed to parse RadioBrowser response";
        ESP_LOGW(TAG, "GetStationByUuid failed: invalid JSON for UUID %s", stationuuid.c_str());
        return false;
    }

    if (cJSON_GetArraySize(root) == 0) {
        cJSON_Delete(root);
        err_msg = "Station not found by UUID";
        ESP_LOGW(TAG, "GetStationByUuid: station not found for UUID %s", stationuuid.c_str());
        return false;
    }

    cJSON* item = cJSON_GetArrayItem(root, 0);
    RadioStationInfo parsed_info = StationInfoFromJson(item);
    cJSON_Delete(root);

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
} // namespace

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

    if (static_cast<int>(local_results.size()) >= required) {
        ESP_LOGI(TAG, "Local radio catalog: %d matches. Using local radio catalog results",
                 static_cast<int>(local_results.size()));
        cJSON* root = cJSON_CreateArray();
        for (const auto& info : local_results) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", info.name.c_str());
            cJSON_AddStringToObject(item, "stationuuid", info.stationuuid.c_str());
            cJSON_AddStringToObject(item, "country", info.country.c_str());
            cJSON_AddStringToObject(item, "state", info.state.c_str());
            cJSON_AddItemToArray(root, item);
        }
        char* output = cJSON_PrintUnformatted(root);
        std::string response = output != nullptr ? output : "[]";
        if (output != nullptr) cJSON_free(output);
        cJSON_Delete(root);
        return response;
    }

    ESP_LOGI(TAG, "Local catalog insufficient (%d matches), querying RadioBrowser online",
             static_cast<int>(local_results.size()));

    return PerformOnlineSearch(query, countrycode, language, tag, limit);
}

std::string RadioBrowser::PlayStation(const std::string& url, const std::string& title, const std::string& station_uuid) {
    if (!station_uuid.empty()) {
        ESP_LOGI(TAG, "Playing radio by station UUID: %s", station_uuid.c_str());
        RadioStationInfo fresh_station;
        std::string err_msg;
        if (!GetStationByUuid(station_uuid, fresh_station, err_msg)) {
            return "Failed to resolve radio station: " + err_msg;
        }

        ESP_LOGI(TAG, "Resolved radio station by UUID: %s", fresh_station.name.c_str());

        std::string cat_err;
        if (!RadioStorage::GetInstance().AddOrUpdateCatalogStation(fresh_station, cat_err)) {
            ESP_LOGW(TAG, "Failed to refresh radio catalog after UUID lookup: %s", cat_err.c_str());
        }

        std::string play_err;
        if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err)) {
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

std::string RadioBrowser::AddFavorite(const std::string& name, const std::string& country,
                                      const std::string& city, const std::string& keywords,
                                      const std::string& station_uuid) {
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

    return RadioStorage::GetInstance().AddFavorite(target_station);
}

std::string RadioBrowser::ListFavorites() const {
    return RadioStorage::GetInstance().ListFavorites();
}

std::string RadioBrowser::PlayFavorite(const std::string& name) {
    std::vector<RadioStationInfo> favorites;
    if (!RadioStorage::GetInstance().GetFavorites(favorites) || favorites.empty()) {
        return "Favorites list is empty";
    }

    RadioStationInfo target_favorite;
    size_t target_index = 0;
    bool found = false;
    for (size_t i = 0; i < favorites.size(); ++i) {
        const auto& fav = favorites[i];
        if (ContainsInsensitive(fav.name, name) ||
            ContainsInsensitive(fav.country, name) ||
            ContainsInsensitive(fav.state, name) ||
            ContainsInsensitive(fav.tags, name)) {
            target_favorite = fav;
            target_index = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return "Favorite station not found";
    }

    ESP_LOGI(TAG, "[FAVORITE_PLAY] name=%s uuid=%s", target_favorite.name.c_str(), target_favorite.stationuuid.c_str());

    if (!target_favorite.stationuuid.empty()) {
        ESP_LOGI(TAG, "[FAVORITE_PLAY] resolving by UUID");
        RadioStationInfo fresh_station;
        std::string err_msg;
        if (GetStationByUuid(target_favorite.stationuuid, fresh_station, err_msg)) {
            ESP_LOGI(TAG, "[FAVORITE_PLAY] resolved fresh station by UUID");
            RadioStorage::GetInstance().AddOrUpdateCatalogStation(fresh_station, err_msg);
            std::string play_err;
            if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err)) {
                return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
            }
            return "Playing favorite station: " + fresh_station.name;
        }
    }

    ESP_LOGI(TAG, "[FAVORITE_PLAY] UUID missing, trying local catalog");
    const auto catalog_results = RadioStorage::GetInstance().SearchCatalog(
        target_favorite.name, target_favorite.countrycode, target_favorite.language, target_favorite.tags, 10);

    std::vector<RadioStationInfo> exact_matches;
    for (const auto& station : catalog_results) {
        if (station.stationuuid.empty()) continue;
        if (!EqualsInsensitive(station.name, target_favorite.name)) continue;
        if (!target_favorite.countrycode.empty() &&
            !ContainsInsensitive(station.countrycode, target_favorite.countrycode) &&
            !ContainsInsensitive(station.country, target_favorite.countrycode)) {
            continue;
        }
        exact_matches.push_back(station);
    }

    if (exact_matches.size() == 1) {
        const std::string recovered_uuid = exact_matches[0].stationuuid;
        ESP_LOGI(TAG, "[FAVORITE_PLAY] local UUID recovered: %s", recovered_uuid.c_str());
        RadioStationInfo fresh_station;
        std::string err_msg;
        if (GetStationByUuid(recovered_uuid, fresh_station, err_msg)) {
            ESP_LOGI(TAG, "[FAVORITE_PLAY] fresh station resolved by recovered UUID");
            favorites[target_index] = fresh_station;
            std::string save_err;
            if (RadioStorage::GetInstance().SaveFavorites(favorites, save_err)) {
                ESP_LOGI(TAG, "[FAVORITE_PLAY] favorite UUID backfilled");
            } else {
                ESP_LOGW(TAG, "[FAVORITE_PLAY] failed to backfill favorite: %s", save_err.c_str());
            }
            RadioStorage::GetInstance().AddOrUpdateCatalogStation(fresh_station, err_msg);
            std::string play_err;
            if (!MediaPlayer::GetInstance().PlayRadio(fresh_station, play_err)) {
                return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
            }
            return "Playing favorite station: " + fresh_station.name;
        }
    }

    ESP_LOGI(TAG, "[FAVORITE_PLAY] local UUID recovery failed, using direct URL fallback");
    if (!target_favorite.url_resolved.empty()) {
        std::string play_err;
        if (!MediaPlayer::GetInstance().PlayRadio(target_favorite, play_err)) {
            return "Radio station is unavailable: " + (play_err.empty() ? "stream connection failed" : play_err);
        }
        return "Playing favorite station: " + target_favorite.name;
    }

    return "Favorite station has no valid URL";
}

std::string RadioBrowser::RemoveFavorite(const std::string& name) {
    return RadioStorage::GetInstance().RemoveFavorite(name);
}

void RadioBrowser::RegisterMcpTools() {
    McpServer::GetInstance().AddTool(
        "radio.add_favorite",
        "Save an internet radio station to hidden favorites.\n"
        "Two modes of operation:\n"
        "1. Save a specified station: pass 'name' and optional country, city, keywords, stationuuid.\n"
        "2. Save the currently playing or last played station: omit 'name' (or pass empty string ''). The device automatically uses current station details.\n"
        "Use mode 2 when the user asks to save the active or currently playing radio (e.g. 'Add this station to favorites', 'Save current radio').\n"
        "Do NOT guess or invent a station name from dialogue history when user asks to save current radio; call this tool without 'name'.",
        PropertyList({
            Property("name", kPropertyTypeString, std::string("")),
            Property("country", kPropertyTypeString, std::string("")),
            Property("city", kPropertyTypeString, std::string("")),
            Property("keywords", kPropertyTypeString, std::string("")),
            Property("stationuuid", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string name = properties.HasProperty("name") ? properties["name"].value<std::string>() : "";
            const std::string country = properties.HasProperty("country") ? properties["country"].value<std::string>() : "";
            const std::string city = properties.HasProperty("city") ? properties["city"].value<std::string>() : "";
            const std::string keywords = properties.HasProperty("keywords") ? properties["keywords"].value<std::string>() : "";
            const std::string stationuuid = properties.HasProperty("stationuuid") ? properties["stationuuid"].value<std::string>() : "";
            return AddFavorite(name, country, city, keywords, stationuuid);
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
        "Play a hidden favorite station by its saved name or key name fragment.\n"
        "Use this tool when the requested station matches an item returned by radio.list_favorites (including minor name/spelling variations).",
        PropertyList({
            Property("name", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            if (!properties.HasProperty("name")) {
                return "Favorite station name is required";
            }
            const std::string name = properties["name"].value<std::string>();
            if (name.empty()) {
                return "Favorite station name is required";
            }
            return PlayFavorite(name);
        });
    McpServer::GetInstance().AddTool(
        "radio.remove_favorite",
        "Remove a hidden favorite radio station by name.",
        PropertyList({
            Property("name", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return RemoveFavorite(properties["name"].value<std::string>());
        });
    McpServer::GetInstance().AddTool(
        "radio.search_stations",
        "Search internet radio stations in Radio Browser or local catalog with structured filters.\n"
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
        "When presenting search results to the user, normally mention only station names. Do not read station UUIDs, URLs, codecs, bitrates, tags, languages, or other technical metadata unless explicitly requested. If several stations are found, briefly list their names and ask which one to play. Use location (country/state) only to distinguish stations with similar names.",
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
        "Play a Radio Browser station using its stationuuid or url_resolved value.",
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
