#include "radio_storage.h"

#include "storage_manager.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "RadioStorage"

namespace {
constexpr const char* kFavoritesPath = "/sdcard/radio_favorites.json";
constexpr const char* kFavoritesTmpPath = "/sdcard/radio_favorites.json.tmp";
constexpr const char* kCatalogPath = "/sdcard/radio_catalog.json";
constexpr const char* kCatalogTmpPath = "/sdcard/radio_catalog.json.tmp";

std::string JsonString(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return (cJSON_IsString(value) && value->valuestring != nullptr) ? value->valuestring : "";
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

bool IsSupportedCodec(const std::string& codec) {
    if (codec.empty()) return true;
    std::string lower = codec;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (lower.find("mp3") != std::string::npos || lower.find("aac") != std::string::npos);
}

RadioStationInfo StationInfoFromCJson(cJSON* object) {
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

cJSON* StationInfoToCJson(const RadioStationInfo& info) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "stationuuid", info.stationuuid.c_str());
    cJSON_AddStringToObject(item, "name", info.name.c_str());
    cJSON_AddStringToObject(item, "url_resolved", info.url_resolved.c_str());
    cJSON_AddStringToObject(item, "codec", info.codec.c_str());
    cJSON_AddNumberToObject(item, "bitrate", info.bitrate);
    cJSON_AddStringToObject(item, "country", info.country.c_str());
    cJSON_AddStringToObject(item, "countrycode", info.countrycode.c_str());
    cJSON_AddStringToObject(item, "state", info.state.c_str());
    cJSON_AddStringToObject(item, "language", info.language.c_str());
    cJSON_AddStringToObject(item, "tags", info.tags.c_str());
    return item;
}
} // namespace

RadioStorage& RadioStorage::GetInstance() {
    static RadioStorage instance;
    return instance;
}

bool RadioStorage::LoadJsonFile(const char* path, std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    stations.clear();
    if (!StorageManager::GetInstance().IsSdCardMounted()) {
        err_msg = "SD card is not mounted";
        ESP_LOGW(TAG, "LoadJsonFile failed: SD card not mounted");
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        // File does not exist yet. Initialize clean [] file.
        ESP_LOGI(TAG, "File %s does not exist. Initializing empty array on SD.", path);
        std::string tmp_path = std::string(path) + ".tmp";
        return SaveJsonFileAtomic(path, tmp_path.c_str(), stations, err_msg);
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        err_msg = std::string("Could not open ") + path;
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        err_msg = std::string("Invalid file size for ") + path;
        return false;
    }

    std::string buffer;
    buffer.resize(size);
    if (size > 0) {
        size_t read_bytes = fread(&buffer[0], 1, size, f);
        if (read_bytes != static_cast<size_t>(size)) {
            fclose(f);
            err_msg = std::string("Failed to read ") + path;
            return false;
        }
    }
    fclose(f);

    cJSON* root = cJSON_Parse(buffer.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root != nullptr) cJSON_Delete(root);
        err_msg = std::string("Corrupted JSON file on SD card: ") + path;
        ESP_LOGE(TAG, "Corrupted JSON format in %s", path);
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        stations.push_back(StationInfoFromCJson(item));
    }
    cJSON_Delete(root);
    return true;
}

bool RadioStorage::SaveJsonFileAtomic(const char* target_path, const char* tmp_path, const std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    if (!StorageManager::GetInstance().IsSdCardMounted()) {
        err_msg = "SD card is not mounted";
        ESP_LOGW(TAG, "SaveJsonFileAtomic failed: SD card not mounted");
        return false;
    }

    cJSON* root = cJSON_CreateArray();
    for (const auto& info : stations) {
        cJSON_AddItemToArray(root, StationInfoToCJson(info));
    }

    char* output = cJSON_PrintUnformatted(root);
    std::string serialized = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(root);

    // Atomic write to tmp file first
    FILE* f = fopen(tmp_path, "wb");
    if (f == nullptr) {
        err_msg = std::string("Failed to create temporary file ") + tmp_path;
        ESP_LOGE(TAG, "Failed to open %s for writing", tmp_path);
        return false;
    }

    size_t written = fwrite(serialized.c_str(), 1, serialized.size(), f);
    fflush(f);
    fclose(f);

    if (written != serialized.size()) {
        unlink(tmp_path);
        err_msg = std::string("Failed to write complete data to temporary file ") + tmp_path;
        ESP_LOGE(TAG, "Short write on %s", tmp_path);
        return false;
    }

    // Replace target file atomically
    struct stat st;
    if (stat(target_path, &st) == 0) {
        unlink(target_path);
    }
    if (rename(tmp_path, target_path) != 0) {
        unlink(tmp_path);
        err_msg = std::string("Failed to rename temporary file ") + tmp_path + " -> " + target_path;
        ESP_LOGE(TAG, "Rename %s -> %s failed", tmp_path, target_path);
        return false;
    }

    ESP_LOGI(TAG, "Successfully saved %d stations to %s", (int)stations.size(), target_path);
    return true;
}

// Favorites implementation
bool RadioStorage::LoadFavorites(std::vector<RadioStationInfo>& favorites, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadJsonFile(kFavoritesPath, favorites, err_msg);
}

bool RadioStorage::SaveFavorites(const std::vector<RadioStationInfo>& favorites, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SaveJsonFileAtomic(kFavoritesPath, kFavoritesTmpPath, favorites, err_msg);
}

std::string RadioStorage::AddFavorite(const RadioStationInfo& station) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (station.name.empty()) {
        return "No current radio station is available";
    }

    std::vector<RadioStationInfo> favorites;
    std::string err_msg;
    if (!LoadJsonFile(kFavoritesPath, favorites, err_msg)) {
        return err_msg.empty() ? "Failed to load favorites" : err_msg;
    }

    for (const auto& fav : favorites) {
        const bool same_uuid = !station.stationuuid.empty() && (station.stationuuid == fav.stationuuid);
        const bool same_name = ContainsInsensitive(fav.name, station.name) &&
                               (station.country.empty() || ContainsInsensitive(fav.country, station.country));
        if (same_uuid || same_name) {
            return "Station is already in favorites";
        }
    }

    favorites.push_back(station);
    if (!SaveJsonFileAtomic(kFavoritesPath, kFavoritesTmpPath, favorites, err_msg)) {
        return err_msg.empty() ? "Failed to save favorite station" : err_msg;
    }

    return "Station added to favorites: " + station.name;
}

std::string RadioStorage::RemoveFavorite(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name.empty()) {
        return "Favorite station name is required";
    }

    std::vector<RadioStationInfo> favorites;
    std::string err_msg;
    if (!LoadJsonFile(kFavoritesPath, favorites, err_msg)) {
        return err_msg.empty() ? "Failed to load favorites" : err_msg;
    }

    if (favorites.empty()) {
        return "Favorites list is empty";
    }

    auto it = std::remove_if(favorites.begin(), favorites.end(), [&](const RadioStationInfo& fav) {
        return ContainsInsensitive(fav.name, name);
    });

    if (it == favorites.end()) {
        return "Favorite station not found";
    }

    favorites.erase(it, favorites.end());
    if (!SaveJsonFileAtomic(kFavoritesPath, kFavoritesTmpPath, favorites, err_msg)) {
        return err_msg.empty() ? "Failed to save favorites after removal" : err_msg;
    }

    return "Station removed from favorites";
}

std::string RadioStorage::ListFavorites() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RadioStationInfo> favorites;
    std::string err_msg;
    if (!LoadJsonFile(kFavoritesPath, favorites, err_msg)) {
        return "[]";
    }

    cJSON* root = cJSON_CreateArray();
    for (const auto& info : favorites) {
        cJSON_AddItemToArray(root, StationInfoToCJson(info));
    }

    char* output = cJSON_PrintUnformatted(root);
    std::string result = output != nullptr ? output : "[]";
    if (output != nullptr) cJSON_free(output);
    cJSON_Delete(root);
    return result;
}

bool RadioStorage::GetFavorites(std::vector<RadioStationInfo>& favorites) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string err_msg;
    return LoadJsonFile(kFavoritesPath, favorites, err_msg);
}

// Catalog implementation
bool RadioStorage::LoadCatalogInternal(std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    return LoadJsonFile(kCatalogPath, stations, err_msg);
}

bool RadioStorage::SaveCatalogInternal(const std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    return SaveJsonFileAtomic(kCatalogPath, kCatalogTmpPath, stations, err_msg);
}

bool RadioStorage::LoadCatalog(std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadCatalogInternal(stations, err_msg);
}

bool RadioStorage::SaveCatalog(const std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SaveCatalogInternal(stations, err_msg);
}

bool RadioStorage::GetCatalog(std::vector<RadioStationInfo>& stations) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string err_msg;
    return LoadCatalogInternal(stations, err_msg);
}

bool RadioStorage::AddOrUpdateCatalogStation(const RadioStationInfo& station, std::string& err_msg) {
    return AddOrUpdateCatalogStations({station}, err_msg);
}

bool RadioStorage::AddOrUpdateCatalogStations(const std::vector<RadioStationInfo>& new_stations, std::string& err_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RadioStationInfo> catalog;
    if (!LoadCatalogInternal(catalog, err_msg)) {
        return false;
    }

    for (const auto& incoming : new_stations) {
        bool found = false;
        for (auto& existing : catalog) {
            const bool uuid_match = !incoming.stationuuid.empty() && (incoming.stationuuid == existing.stationuuid);
            const bool fallback_match = incoming.stationuuid.empty() && existing.stationuuid.empty() &&
                                         ContainsInsensitive(existing.name, incoming.name) &&
                                         (incoming.country.empty() || ContainsInsensitive(existing.country, incoming.country));
            if (uuid_match || fallback_match) {
                if (!incoming.name.empty()) existing.name = incoming.name;
                if (!incoming.url_resolved.empty()) existing.url_resolved = incoming.url_resolved;
                if (!incoming.codec.empty()) existing.codec = incoming.codec;
                if (incoming.bitrate > 0) existing.bitrate = incoming.bitrate;
                if (!incoming.country.empty()) existing.country = incoming.country;
                if (!incoming.countrycode.empty()) existing.countrycode = incoming.countrycode;
                if (!incoming.state.empty()) existing.state = incoming.state;
                if (!incoming.language.empty()) existing.language = incoming.language;
                if (!incoming.tags.empty()) existing.tags = incoming.tags;
                found = true;
                break;
            }
        }
        if (!found) {
            catalog.push_back(incoming);
        }
    }

    return SaveCatalogInternal(catalog, err_msg);
}

std::vector<RadioStationInfo> RadioStorage::SearchCatalog(const std::string& query,
                                                          const std::string& countrycode,
                                                          const std::string& language,
                                                          const std::string& tag,
                                                          int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RadioStationInfo> catalog;
    std::string err_msg;
    if (!LoadCatalogInternal(catalog, err_msg) || catalog.empty()) {
        return {};
    }

    limit = std::clamp(limit, 1, 20);
    std::vector<RadioStationInfo> results;

    for (const auto& station : catalog) {
        if (!IsSupportedCodec(station.codec)) {
            continue;
        }

        if (!query.empty()) {
            const bool match_name = ContainsInsensitive(station.name, query);
            const bool match_tags = ContainsInsensitive(station.tags, query);
            const bool match_country = ContainsInsensitive(station.country, query);
            const bool match_state = ContainsInsensitive(station.state, query);
            if (!match_name && !match_tags && !match_country && !match_state) {
                continue;
            }
        }

        if (!countrycode.empty()) {
            const bool match_cc = ContainsInsensitive(station.countrycode, countrycode);
            const bool match_country = ContainsInsensitive(station.country, countrycode);
            if (!match_cc && !match_country) {
                continue;
            }
        }

        if (!language.empty()) {
            if (!ContainsInsensitive(station.language, language)) {
                continue;
            }
        }

        if (!tag.empty()) {
            if (!ContainsInsensitive(station.tags, tag)) {
                continue;
            }
        }

        results.push_back(station);
        if (static_cast<int>(results.size()) >= limit) {
            break;
        }
    }

    return results;
}
