#include "musicbrainz_metadata.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>

#define TAG "MusicBrainz"

namespace {
struct Response {
    std::string body;
    size_t max_size = 12288;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr &&
        event->data != nullptr && event->data_len > 0) {
        auto* response = static_cast<Response*>(event->user_data);
        if (response->body.size() < response->max_size) {
            size_t count = std::min(static_cast<size_t>(event->data_len),
                                    response->max_size - response->body.size());
            response->body.append(static_cast<const char*>(event->data), count);
        }
    }
    return ESP_OK;
}

std::string FileTitle(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.erase(dot);
    return name;
}
}

MusicBrainzMetadata& MusicBrainzMetadata::GetInstance() {
    static MusicBrainzMetadata instance;
    return instance;
}

std::string MusicBrainzMetadata::UrlEncode(const std::string& value) const {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}

MusicMetadata MusicBrainzMetadata::Lookup(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = cache_.find(path);
    if (found != cache_.end()) return found->second;

    MusicMetadata guess;
    guess.title = FileTitle(path);
    const size_t separator = guess.title.find(" - ");
    if (separator != std::string::npos) {
        guess.artist = guess.title.substr(0, separator);
        guess.title.erase(0, separator + 3);
    }
    MusicMetadata result = LookupRemote(guess);
    if (cache_.size() >= 32) cache_.erase(cache_.begin());
    cache_[path] = result;
    return result;
}

MusicMetadata MusicBrainzMetadata::LookupRemote(const MusicMetadata& guess) {
    MusicMetadata result = guess;
    std::string query;
    if (!guess.artist.empty()) {
        query = "artist:%22" + UrlEncode(guess.artist) + "%22%20AND%20";
    }
    query += "recording:%22" + UrlEncode(guess.title) + "%22";
    const std::string url = "https://musicbrainz.org/ws/2/recording?query=" + query +
                            "&fmt=json&limit=1&inc=tags";

    Response response;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return guess;
    esp_http_client_set_header(client, "User-Agent",
                               "xiaozhi-esp32/1.0 (https://github.com/78/xiaozhi-esp32)");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Lookup failed: err=%s status=%d", esp_err_to_name(err), status);
        return guess;
    }

    cJSON* root = cJSON_Parse(response.body.c_str());
    cJSON* recordings = root == nullptr ? nullptr : cJSON_GetObjectItem(root, "recordings");
    cJSON* recording = cJSON_IsArray(recordings) ? cJSON_GetArrayItem(recordings, 0) : nullptr;
    if (recording != nullptr) {
        cJSON* title = cJSON_GetObjectItem(recording, "title");
        if (cJSON_IsString(title) && title->valuestring != nullptr) result.title = title->valuestring;
        cJSON* credits = cJSON_GetObjectItem(recording, "artist-credit");
        if (cJSON_IsArray(credits) && cJSON_GetArraySize(credits) > 0) {
            cJSON* credit = cJSON_GetArrayItem(credits, 0);
            cJSON* artist = credit == nullptr ? nullptr : cJSON_GetObjectItem(credit, "artist");
            cJSON* name = artist == nullptr ? nullptr : cJSON_GetObjectItem(artist, "name");
            if (cJSON_IsString(name) && name->valuestring != nullptr) result.artist = name->valuestring;
        }
        cJSON* tags = cJSON_GetObjectItem(recording, "tags");
        cJSON* tag = cJSON_IsArray(tags) ? cJSON_GetArrayItem(tags, 0) : nullptr;
        cJSON* name = tag == nullptr ? nullptr : cJSON_GetObjectItem(tag, "name");
        if (cJSON_IsString(name) && name->valuestring != nullptr) result.genre = name->valuestring;
    }
    if (root != nullptr) cJSON_Delete(root);
    return result;
}
