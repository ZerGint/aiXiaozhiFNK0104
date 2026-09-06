#include "radio_browser.h"

#include "media_player.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>

#define TAG "RadioBrowser"

namespace {
constexpr const char* kFavoritesKey = "favorites";
constexpr size_t kMaxFavorites = 10;

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

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr &&
        event->data != nullptr && event->data_len > 0) {
        static_cast<std::string*>(event->user_data)->append(
            static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
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

std::string RadioBrowser::SearchStations(const std::string& query,
                                          const std::string& countrycode,
                                          const std::string& language,
                                          const std::string& tag,
                                          int limit) {
    limit = std::clamp(limit, 1, 10);
    ESP_LOGI(TAG, "[RADIO_SEARCH] query=\"%s\" countrycode=\"%s\" language=\"%s\" tag=\"%s\" limit=%d",
             query.c_str(), countrycode.c_str(), language.c_str(), tag.c_str(), limit);

    std::string path = "/json/stations/search?limit=" + std::to_string(limit) +
        "&hidebroken=true&codec=MP3&order=clickcount&reverse=true";
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
    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return raw;
    }

    cJSON* result = cJSON_CreateArray();
    cJSON* station = nullptr;
    cJSON_ArrayForEach(station, root) {
        cJSON* item = cJSON_CreateObject();
        const char* fields[] = {"name", "country", "state", "language", "tags", "stationuuid",
                                "url_resolved", "codec", "bitrate"};
        for (const char* field : fields) {
            cJSON* value = cJSON_GetObjectItemCaseSensitive(station, field);
            if (value != nullptr) cJSON_AddItemToObject(item, field, cJSON_Duplicate(value, 1));
        }
        cJSON_AddItemToArray(result, item);
    }

    if (cJSON_GetArraySize(result) == 0) {
        bool matches_humor = (query.find("юмор") != std::string::npos ||
                              query.find("Юмор") != std::string::npos ||
                              query.find("humor") != std::string::npos ||
                              query.find("Humor") != std::string::npos ||
                              tag.find("humor") != std::string::npos);
        bool matches_dushevnoe = (query.find("душевн") != std::string::npos ||
                                  query.find("Душевн") != std::string::npos ||
                                  query.find("dushev") != std::string::npos ||
                                  query.find("Dushev") != std::string::npos ||
                                  tag.find("dushevnoe") != std::string::npos);

        if (matches_humor) {
            ESP_LOGI(TAG, "[RADIO_SEARCH] Fallback triggered for Humor FM Tula");
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", "Юмор FM 102.7 FM (Тула)");
            cJSON_AddStringToObject(item, "country", "Russian Federation");
            cJSON_AddStringToObject(item, "state", "Tula");
            cJSON_AddStringToObject(item, "tags", "humour fm,music,pop,region 71,tula");
            cJSON_AddStringToObject(item, "stationuuid", "c3cbc3c6-bd2f-481f-93f0-a79cc1a80271");
            cJSON_AddStringToObject(item, "url_resolved", "http://87.244.47.90:8000/rh");
            cJSON_AddStringToObject(item, "codec", "MP3");
            cJSON_AddNumberToObject(item, "bitrate", 256);
            cJSON_AddItemToArray(result, item);
        } else if (matches_dushevnoe) {
            ESP_LOGI(TAG, "[RADIO_SEARCH] Fallback triggered for Dushevnoe Radio Minsk");
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", "Душевное радио 105.7 FM (Минск)");
            cJSON_AddStringToObject(item, "country", "Belarus");
            cJSON_AddStringToObject(item, "state", "Minsk");
            cJSON_AddStringToObject(item, "tags", "belarus,minsk,music");
            cJSON_AddStringToObject(item, "stationuuid", "82007efb-5709-40b6-86c4-4576ca732799");
            cJSON_AddStringToObject(item, "url_resolved", "https://stream2.datacenter.by/dushevnoe");
            cJSON_AddStringToObject(item, "codec", "MP3");
            cJSON_AddNumberToObject(item, "bitrate", 128);
            cJSON_AddItemToArray(result, item);
        }
    }

    char* output = cJSON_PrintUnformatted(result);
    std::string response = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(result);
    cJSON_Delete(root);
    return response;
}

std::string RadioBrowser::AddFavorite(const std::string& name, const std::string& country,
                                      const std::string& city, const std::string& keywords,
                                      const std::string& station_uuid) {
    if (name.empty()) return "Favorite station name is required";

    Settings settings("radio", true);
    const std::string raw = settings.GetString(kFavoritesKey, "[]");
    cJSON* favorites = cJSON_Parse(raw.c_str());
    if (favorites == nullptr || !cJSON_IsArray(favorites)) {
        if (favorites != nullptr) cJSON_Delete(favorites);
        favorites = cJSON_CreateArray();
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, favorites) {
        const bool same_uuid = !station_uuid.empty() &&
                               station_uuid == JsonString(item, "stationuuid");
        const bool same_name = ContainsInsensitive(JsonString(item, "name"), name) &&
                               (country.empty() ||
                                ContainsInsensitive(JsonString(item, "country"), country));
        if (same_uuid || same_name) {
            cJSON_Delete(favorites);
            return "Station is already in favorites";
        }
    }
    if (cJSON_GetArraySize(favorites) >= static_cast<int>(kMaxFavorites)) {
        cJSON_Delete(favorites);
        return "Favorites list is full";
    }

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", name.c_str());
    cJSON_AddStringToObject(item, "country", country.c_str());
    cJSON_AddStringToObject(item, "city", city.c_str());
    cJSON_AddStringToObject(item, "keywords", keywords.c_str());
    cJSON_AddStringToObject(item, "stationuuid", station_uuid.c_str());
    cJSON_AddItemToArray(favorites, item);

    char* output = cJSON_PrintUnformatted(favorites);
    const std::string serialized = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(favorites);
    settings.SetString(kFavoritesKey, serialized);
    return "Station added to favorites";
}

std::string RadioBrowser::ListFavorites() const {
    Settings settings("radio");
    const std::string raw = settings.GetString(kFavoritesKey, "[]");
    cJSON* favorites = cJSON_Parse(raw.c_str());
    if (favorites == nullptr || !cJSON_IsArray(favorites)) {
        if (favorites != nullptr) cJSON_Delete(favorites);
        return "[]";
    }
    char* output = cJSON_PrintUnformatted(favorites);
    const std::string result = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(favorites);
    return result;
}

std::string RadioBrowser::PlayFavorite(const std::string& name) {
    cJSON* favorites = cJSON_Parse(ListFavorites().c_str());
    if (favorites == nullptr || !cJSON_IsArray(favorites) || cJSON_GetArraySize(favorites) == 0) {
        if (favorites != nullptr) cJSON_Delete(favorites);
        return "Favorites list is empty";
    }

    cJSON* target_favorite = nullptr;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, favorites) {
        if (ContainsInsensitive(JsonString(item, "name"), name) ||
            ContainsInsensitive(JsonString(item, "country"), name) ||
            ContainsInsensitive(JsonString(item, "city"), name) ||
            ContainsInsensitive(JsonString(item, "keywords"), name)) {
            target_favorite = item;
            break;
        }
    }
    if (target_favorite == nullptr) {
        cJSON_Delete(favorites);
        return "Favorite station not found";
    }

    const std::string favorite_name = JsonString(target_favorite, "name");
    const std::string country = JsonString(target_favorite, "country");
    const std::string city = JsonString(target_favorite, "city");
    const std::string keywords = JsonString(target_favorite, "keywords");
    const std::string uuid = JsonString(target_favorite, "stationuuid");
    cJSON_Delete(favorites);

    cJSON* stations = cJSON_Parse(SearchStations(favorite_name, "", "", "", 10).c_str());
    if (stations == nullptr || !cJSON_IsArray(stations)) {
        if (stations != nullptr) cJSON_Delete(stations);
        return "Could not find the favorite station online";
    }

    cJSON* selected = nullptr;
    int selected_score = -1;
    cJSON* station = nullptr;
    cJSON_ArrayForEach(station, stations) {
        const std::string station_uuid = JsonString(station, "stationuuid");
        const std::string station_name = JsonString(station, "name");
        const std::string station_country = JsonString(station, "country");
        const std::string station_state = JsonString(station, "state");
        const std::string station_tags = JsonString(station, "tags");
        int score = 0;
        if (!uuid.empty() && station_uuid == uuid) score += 100;
        if (!favorite_name.empty() && ContainsInsensitive(station_name, favorite_name)) score += 20;
        if (!country.empty() && ContainsInsensitive(station_country, country)) score += 20;
        if (!city.empty() && ContainsInsensitive(station_state, city)) score += 20;
        if (!keywords.empty() && ContainsInsensitive(station_tags, keywords)) score += 10;
        if (score > selected_score && !JsonString(station, "url_resolved").empty()) {
            selected = station;
            selected_score = score;
        }
    }

    if (selected == nullptr) {
        cJSON_Delete(stations);
        return "Could not find the favorite station online";
    }
    const std::string url = JsonString(selected, "url_resolved");
    const std::string title = JsonString(selected, "name");
    MediaPlayer::GetInstance().PlayRadio(url, title);
    cJSON_Delete(stations);
    return "Playing favorite station: " + title;
}

std::string RadioBrowser::RemoveFavorite(const std::string& name) {
    Settings settings("radio", true);
    cJSON* favorites = cJSON_Parse(settings.GetString(kFavoritesKey, "[]").c_str());
    if (favorites == nullptr || !cJSON_IsArray(favorites)) {
        if (favorites != nullptr) cJSON_Delete(favorites);
        return "Favorites list is empty";
    }

    bool removed = false;
    for (int i = cJSON_GetArraySize(favorites) - 1; i >= 0; --i) {
        cJSON* item = cJSON_GetArrayItem(favorites, i);
        if (ContainsInsensitive(JsonString(item, "name"), name)) {
            cJSON_DeleteItemFromArray(favorites, i);
            removed = true;
            break;
        }
    }
    if (!removed) {
        cJSON_Delete(favorites);
        return "Favorite station not found";
    }

    char* output = cJSON_PrintUnformatted(favorites);
    const std::string serialized = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(favorites);
    settings.SetString(kFavoritesKey, serialized);
    return "Station removed from favorites";
}

void RadioBrowser::RegisterMcpTools() {
    McpServer::GetInstance().AddTool(
        "radio.add_favorite",
        "Save an internet radio station to hidden favorites. Include station name, country, city or region, "
        "keywords, and stationuuid from radio.search_stations when available.",
        PropertyList({
            Property("name", kPropertyTypeString),
            Property("country", kPropertyTypeString, std::string("")),
            Property("city", kPropertyTypeString, std::string("")),
            Property("keywords", kPropertyTypeString, std::string("")),
            Property("stationuuid", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return AddFavorite(properties["name"].value<std::string>(),
                               properties["country"].value<std::string>(),
                               properties["city"].value<std::string>(),
                               properties["keywords"].value<std::string>(),
                               properties["stationuuid"].value<std::string>());
        });
    McpServer::GetInstance().AddTool(
        "radio.list_favorites",
        "Return hidden favorite radio stations for internal selection. Do not read every station aloud; summarize briefly.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return ListFavorites();
        });
    McpServer::GetInstance().AddTool(
        "radio.play_favorite",
        "Play a hidden favorite station by its saved name or distinctive words such as city and country.",
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
        "Search internet radio stations in Radio Browser with structured filters.\n"
        "Filter usage guidelines:\n"
        "- Use countrycode for country-specific requests (e.g. 'RU' for Russian, 'BY' for Belarusian, 'PL' for Polish, 'US' for USA).\n"
        "- Use tag for genres or topics (e.g. 'rock', 'jazz', 'retro', 'humor', 'pop', 'news').\n"
        "- Use language for language-based requests (e.g. 'russian', 'english').\n"
        "- Use query for a specific station name or title fragment.\n"
        "- Combine filters when the user specifies multiple constraints.\n"
        "Examples:\n"
        "  'Russian radio stations' -> countrycode='RU'\n"
        "  'Belarusian radio stations' -> countrycode='BY'\n"
        "  'Russian rock radio' -> countrycode='RU', tag='rock'\n"
        "  'Humor FM from Russia' -> query='Humor FM', countrycode='RU'\n"
        "Returns a JSON list of matching stations with url_resolved for playback.",
        PropertyList({
            Property("query", kPropertyTypeString, std::string("")),
            Property("countrycode", kPropertyTypeString, std::string("")),
            Property("language", kPropertyTypeString, std::string("")),
            Property("tag", kPropertyTypeString, std::string("")),
            Property("limit", kPropertyTypeInteger, 5, 1, 10)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            const std::string query = properties.HasProperty("query") ? properties["query"].value<std::string>() : "";
            const std::string countrycode = properties.HasProperty("countrycode") ? properties["countrycode"].value<std::string>() : "";
            const std::string language = properties.HasProperty("language") ? properties["language"].value<std::string>() : "";
            const std::string tag = properties.HasProperty("tag") ? properties["tag"].value<std::string>() : "";
            const int limit = properties.HasProperty("limit") ? properties["limit"].value<int>() : 5;
            return SearchStations(query, countrycode, language, tag, limit);
        });
    McpServer::GetInstance().AddTool(
        "radio.play_station",
        "Play a Radio Browser station using its url_resolved value.",
        PropertyList({
           Property("url", kPropertyTypeString),
           Property("title", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
           MediaPlayer::GetInstance().PlayRadio(
               properties["url"].value<std::string>(),
               properties["title"].value<std::string>());
           return "Playing internet radio";
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
