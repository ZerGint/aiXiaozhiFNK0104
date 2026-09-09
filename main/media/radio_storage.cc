#include "radio_storage.h"
#include "radio_memory_diag.h"

#include "storage_manager.h"
#include "radio_binary_io.h"

#include <esp_log.h>
#include <cJSON.h>

#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>

#define TAG "RadioStorage"

namespace {
constexpr const char* kFavoritesPath = "/sdcard/radio_favorites.json";
constexpr const char* kFavoritesTmpPath = "/sdcard/radio_favorites.json.tmp";
constexpr const char* kCatalogPath = "/sdcard/radio_catalog.json";
constexpr const char* kCatalogTmpPath = "/sdcard/radio_catalog.json.tmp";
constexpr const char* kBinaryCatalogPath = "/sdcard/radio_catalog.dat";
constexpr const char* kBinaryCatalogTmpPath = "/sdcard/radio_catalog.tmp";
constexpr bool RADIO_LOCAL_SEARCH_RANKING_ENABLED = RadioSearchRanking::Enabled;
constexpr int kNameTokenWeight = RadioSearchRanking::NameWeight;
constexpr int kStateTokenWeight = RadioSearchRanking::StateWeight;
constexpr int kMetadataTokenWeight = RadioSearchRanking::MetadataWeight;
constexpr int kFullNameBonus = RadioSearchRanking::FullNameBonus;
bool ContainsInsensitiveNoAlloc(const std::string& value, const std::string& query);
std::vector<std::string> TokenizeQuery(const std::string& query);

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
    auto add_string_reference = [item](const char* key, const std::string& value) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> value_item(
            cJSON_CreateStringReference(value.c_str()), &cJSON_Delete);
        if (value_item == nullptr || !cJSON_AddItemToObjectCS(item, key, value_item.get())) {
            return false;
        }
        value_item.release();
        return true;
    };
    if (item == nullptr ||
        !add_string_reference("stationuuid", info.stationuuid) ||
        !add_string_reference("name", info.name) ||
        !add_string_reference("url_resolved", info.url_resolved) ||
        !add_string_reference("codec", info.codec) ||
        cJSON_AddNumberToObject(item, "bitrate", info.bitrate) == nullptr ||
        !add_string_reference("country", info.country) ||
        !add_string_reference("countrycode", info.countrycode) ||
        !add_string_reference("state", info.state) ||
        !add_string_reference("language", info.language) ||
        !add_string_reference("tags", info.tags)) {
        cJSON_Delete(item);
        return nullptr;
    }
    return item;
}
} // namespace

RadioStorage& RadioStorage::GetInstance() {
    static RadioStorage instance;
    return instance;
}

bool RadioStorage::LoadJsonFile(const char* path, std::vector<RadioStationInfo>& stations,
                                std::string& err_msg) {
    LogRadioMemory(TAG, "CATALOG_LOAD_BEGIN");
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

    LogRadioMemory(TAG, "CATALOG_BEFORE_FOPEN_READ");
    std::unique_ptr<FILE, decltype(&fclose)> f(fopen(path, "rb"), &fclose);
    LogRadioMemory(TAG, "CATALOG_AFTER_FOPEN_READ");
    if (f == nullptr) {
        err_msg = std::string("Could not open ") + path;
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }

    fseek(f.get(), 0, SEEK_END);
    long size = ftell(f.get());
    fseek(f.get(), 0, SEEK_SET);

    if (size < 0) {
        err_msg = std::string("Invalid file size for ") + path;
        return false;
    }

    std::string buffer;
    buffer.resize(size);
    size_t read_bytes = 0;
    ESP_LOGI(TAG, "[MEM] catalog_file_bytes=%ld buffer_size=%zu buffer_capacity=%zu",
             size, buffer.size(), buffer.capacity());
    if (size > 0) {
        LogRadioMemory(TAG, "CATALOG_BEFORE_READ");
        read_bytes = fread(&buffer[0], 1, size, f.get());
        LogRadioMemory(TAG, "CATALOG_AFTER_READ");
        if (read_bytes != static_cast<size_t>(size)) {
            err_msg = std::string("Failed to read ") + path;
            return false;
        }
    }
    LogRadioMemory(TAG, "CATALOG_LOAD_BEFORE_CLOSE");
    f.reset();
    LogRadioMemory(TAG, "CATALOG_LOAD_AFTER_CLOSE");

    LogRadioMemory(TAG, "CATALOG_BEFORE_JSON_PARSE");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(buffer.c_str()),
                                                         &cJSON_Delete);
    if (root == nullptr || !cJSON_IsArray(root.get())) {
        const char* parse_error = cJSON_GetErrorPtr();
        const size_t error_offset = parse_error != nullptr && parse_error >= buffer.c_str() &&
                                            parse_error <= buffer.c_str() + buffer.size()
                                        ? static_cast<size_t>(parse_error - buffer.c_str())
                                        : 0;
        ESP_LOGE(TAG, "[MEM] JSON_PARSE_FAIL path=%s file_bytes=%ld read_bytes=%zu error_offset=%zu",
                 path, size, read_bytes, error_offset);
        LogRadioMemory(TAG, "CATALOG_AFTER_JSON_PARSE_FAIL");
        err_msg = std::string("Corrupted JSON file on SD card: ") + path;
        ESP_LOGE(TAG, "Corrupted JSON format in %s", path);
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, root.get()) {
        stations.push_back(StationInfoFromCJson(item));
    }
    ESP_LOGI(TAG, "[MEM] catalog_station_count=%zu catalog_capacity=%zu", stations.size(), stations.capacity());
    LogRadioMemory(TAG, "CATALOG_LOAD_END");
    return true;
}

bool RadioStorage::SaveJsonFileAtomic(const char* target_path, const char* tmp_path,
                                      const std::vector<RadioStationInfo>& stations,
                                      std::string& err_msg) {
    LogRadioMemory(TAG, "CATALOG_SAVE_BEGIN");
    if (!StorageManager::GetInstance().IsSdCardMounted()) {
        err_msg = "SD card is not mounted";
        ESP_LOGW(TAG, "SaveJsonFileAtomic failed: SD card not mounted");
        return false;
    }

    size_t value_copy_bytes = 0;
    for (const auto& info : stations) {
        value_copy_bytes += info.stationuuid.size() + 1;
        value_copy_bytes += info.name.size() + 1;
        value_copy_bytes += info.url_resolved.size() + 1;
        value_copy_bytes += info.codec.size() + 1;
        value_copy_bytes += info.country.size() + 1;
        value_copy_bytes += info.countrycode.size() + 1;
        value_copy_bytes += info.state.size() + 1;
        value_copy_bytes += info.language.size() + 1;
        value_copy_bytes += info.tags.size() + 1;
    }
    const size_t station_count = stations.size();
    const size_t cjson_node_count = 1 + station_count * 11;
    ESP_LOGI(TAG, "[MEM] CATALOG_SAVE_COST stations=%zu value_bytes=%zu value_allocs=%zu "
                  "key_bytes=%zu key_allocs=%zu node_count=%zu node_bytes=%zu",
             station_count, value_copy_bytes, station_count * 9, station_count * 84,
             station_count * 10, cjson_node_count, cjson_node_count * sizeof(cJSON));

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateArray(), &cJSON_Delete);
    if (root == nullptr) {
        err_msg = "Failed to allocate JSON catalog";
        return false;
    }

    for (const auto& info : stations) {
        cJSON* item = StationInfoToCJson(info);
        if (item == nullptr || !cJSON_AddItemToArray(root.get(), item)) {
            cJSON_Delete(item);
            err_msg = "Failed to allocate JSON catalog station";
            return false;
        }
    }

    LogRadioMemory(TAG, "CATALOG_BEFORE_SERIALIZE");
    char* output = cJSON_PrintUnformatted(root.get());
    LogRadioMemory(TAG, "CATALOG_AFTER_SERIALIZE");
    if (output == nullptr) {
        err_msg = "Failed to serialize JSON catalog";
        return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> serialized(output, &cJSON_free);
    const size_t serialized_size = strlen(serialized.get());
    ESP_LOGI(TAG, "[MEM] catalog_station_count=%zu serialized_bytes=%zu", stations.size(), serialized_size);

    // Atomic write to tmp file first
    LogRadioMemory(TAG, "CATALOG_BEFORE_FOPEN_WRITE");
    FILE* f = fopen(tmp_path, "wb");
    LogRadioMemory(TAG, "CATALOG_AFTER_FOPEN_WRITE");
    if (f == nullptr) {
        err_msg = std::string("Failed to create temporary file ") + tmp_path;
        ESP_LOGE(TAG, "Failed to open %s for writing", tmp_path);
        return false;
    }

    LogRadioMemory(TAG, "CATALOG_BEFORE_WRITE");
    size_t written = fwrite(serialized.get(), 1, serialized_size, f);
    LogRadioMemory(TAG, "CATALOG_AFTER_WRITE");
    ESP_LOGI(TAG, "[MEM] catalog_write_requested=%zu catalog_write_completed=%zu", serialized_size, written);
    LogRadioMemory(TAG, "CATALOG_SAVE_BEFORE_CLOSE");
    fflush(f);
    fclose(f);
    LogRadioMemory(TAG, "CATALOG_SAVE_AFTER_CLOSE");

    if (written != serialized_size) {
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
    LogRadioMemory(TAG, "CATALOG_BEFORE_RENAME");
    if (rename(tmp_path, target_path) != 0) {
        unlink(tmp_path);
        err_msg = std::string("Failed to rename temporary file ") + tmp_path + " -> " + target_path;
        ESP_LOGE(TAG, "Rename %s -> %s failed", tmp_path, target_path);
        return false;
    }
    LogRadioMemory(TAG, "CATALOG_AFTER_RENAME");

    ESP_LOGI(TAG, "Successfully saved %d stations to %s", (int)stations.size(), target_path);
    LogRadioMemory(TAG, "CATALOG_SAVE_END");
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
    stations.clear();
    struct stat st;
    if (stat(kBinaryCatalogPath, &st) != 0) return true;
    RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
    uint32_t count = 0;
    if (!reader.Open(count)) { err_msg = "Failed to open binary catalog"; return false; }
    stations.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        RadioStationInfo station;
        if (!reader.ReadNext(station)) { err_msg = "Corrupted binary catalog"; return false; }
        stations.push_back(std::move(station));
    }
    if (!reader.Finish()) { err_msg = "Corrupted binary catalog"; return false; }
    return true;
}

bool RadioStorage::SaveCatalogInternal(const std::vector<RadioStationInfo>& stations, std::string& err_msg) {
    RadioBinaryIO::CatalogWriter writer(kBinaryCatalogTmpPath, static_cast<uint32_t>(stations.size()));
    if (!writer.Begin()) { err_msg = "Failed to create binary catalog"; return false; }
    for (const auto& station : stations) if (!writer.Write(station)) { err_msg = "Failed to write binary catalog"; return false; }
    if (!writer.Finish() || !RadioBinaryIO::ReplaceTarget(kBinaryCatalogTmpPath, kBinaryCatalogPath)) { err_msg = "Failed to replace binary catalog"; return false; }
    return true;
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
    for (const auto& incoming : new_stations) {
        struct stat st;
        const bool exists = stat(kBinaryCatalogPath, &st) == 0;
        uint32_t count = 0;
        RadioStationInfo existing;
        bool found = false;
        if (exists) {
            RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
            if (!reader.Open(count)) { err_msg = "Failed to open binary catalog"; return false; }
            for (uint32_t i = 0; i < count; ++i) {
                RadioStationInfo candidate;
                if (!reader.ReadNext(candidate)) { err_msg = "Corrupted binary catalog"; return false; }
                if (!incoming.stationuuid.empty() && incoming.stationuuid == candidate.stationuuid) {
                    existing = std::move(candidate); found = true;
                }
            }
            if (!reader.Finish()) { err_msg = "Corrupted binary catalog"; return false; }
        }
        if (found) {
            RadioStationInfo merged = existing;
            if (!incoming.name.empty()) merged.name = incoming.name;
            if (!incoming.url_resolved.empty()) merged.url_resolved = incoming.url_resolved;
            if (!incoming.codec.empty()) merged.codec = incoming.codec;
            if (incoming.bitrate > 0) merged.bitrate = incoming.bitrate;
            if (!incoming.country.empty()) merged.country = incoming.country;
            if (!incoming.countrycode.empty()) merged.countrycode = incoming.countrycode;
            if (!incoming.state.empty()) merged.state = incoming.state;
            if (!incoming.language.empty()) merged.language = incoming.language;
            if (!incoming.tags.empty()) merged.tags = incoming.tags;
            if (merged.stationuuid == existing.stationuuid && merged.name == existing.name && merged.url_resolved == existing.url_resolved &&
                merged.codec == existing.codec && merged.bitrate == existing.bitrate && merged.country == existing.country &&
                merged.countrycode == existing.countrycode && merged.state == existing.state && merged.language == existing.language && merged.tags == existing.tags) continue;
            RadioBinaryIO::CatalogWriter writer(kBinaryCatalogTmpPath, count);
            if (!writer.Begin()) { err_msg = "Failed to create binary catalog"; return false; }
            RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
            if (!reader.Open(count)) { err_msg = "Failed to reopen binary catalog"; return false; }
            for (uint32_t i = 0; i < count; ++i) { RadioStationInfo candidate; if (!reader.ReadNext(candidate) || !writer.Write(candidate.stationuuid == existing.stationuuid ? merged : candidate)) { err_msg = "Failed to rewrite binary catalog"; return false; } }
            if (!reader.Finish() || !writer.Finish() || !RadioBinaryIO::ReplaceTarget(kBinaryCatalogTmpPath, kBinaryCatalogPath)) { err_msg = "Failed to replace binary catalog"; return false; }
        } else {
            if (count >= RadioBinaryCodec::kMaxRecords) { err_msg = "Binary catalog record limit exceeded"; return false; }
            RadioBinaryIO::CatalogWriter writer(kBinaryCatalogTmpPath, count + 1);
            if (!writer.Begin()) { err_msg = "Failed to create binary catalog"; return false; }
            if (exists) {
                RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
                if (!reader.Open(count)) { err_msg = "Failed to reopen binary catalog"; return false; }
                for (uint32_t i = 0; i < count; ++i) { RadioStationInfo candidate; if (!reader.ReadNext(candidate) || !writer.Write(candidate)) { err_msg = "Failed to rewrite binary catalog"; return false; } }
                if (!reader.Finish()) { err_msg = "Corrupted binary catalog"; return false; }
            }
            if (!writer.Write(incoming) || !writer.Finish() || !RadioBinaryIO::ReplaceTarget(kBinaryCatalogTmpPath, kBinaryCatalogPath)) { err_msg = "Failed to replace binary catalog"; return false; }
        }
    }
    return true;
}

std::vector<RadioStationInfo> RadioStorage::SearchCatalog(const std::string& query,
                                                          const std::string& countrycode,
                                                          const std::string& language,
                                                          const std::string& tag,
                                                          int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    struct stat st;
    if (stat(kBinaryCatalogPath, &st) != 0) return {};
    RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
    uint32_t catalog_count = 0;
    if (!reader.Open(catalog_count) || catalog_count == 0) return {};

    limit = std::clamp(limit, 1, 20);
    std::vector<RadioStationInfo> results;
    results.reserve(static_cast<size_t>(limit));

    if constexpr (RADIO_LOCAL_SEARCH_RANKING_ENABLED) {
        struct ScoredStation { int score; RadioStationInfo station; };
        std::vector<ScoredStation> ranked;
        ranked.reserve(static_cast<size_t>(limit));
        const auto tokens = TokenizeQuery(query);
        for (uint32_t index = 0; index < catalog_count; ++index) {
            RadioStationInfo station;
            if (!reader.ReadNext(station)) return {};
            if (!IsSupportedCodec(station.codec)) continue;
            if (!countrycode.empty() && !ContainsInsensitiveNoAlloc(station.countrycode, countrycode) &&
                !ContainsInsensitiveNoAlloc(station.country, countrycode)) continue;
            if (!language.empty() && !ContainsInsensitiveNoAlloc(station.language, language)) continue;
            if (!tag.empty() && !ContainsInsensitiveNoAlloc(station.tags, tag)) continue;
            int score = 0;
            int useful_tokens = 0;
            for (const auto& token : tokens) {
                if (ContainsInsensitiveNoAlloc(station.name, token)) { score += kNameTokenWeight; ++useful_tokens; }
                else if (ContainsInsensitiveNoAlloc(station.state, token)) { score += kStateTokenWeight; ++useful_tokens; }
                else if (ContainsInsensitiveNoAlloc(station.tags, token) || ContainsInsensitiveNoAlloc(station.country, token)) {
                    score += kMetadataTokenWeight; ++useful_tokens;
                }
            }
            if (!query.empty() && useful_tokens == 0) continue;
            if (!query.empty() && ContainsInsensitiveNoAlloc(station.name, query)) score += kFullNameBonus;
            auto position = std::find_if(ranked.begin(), ranked.end(), [score](const ScoredStation& item) {
                return score > item.score;
            });
            if (position == ranked.end() && ranked.size() >= static_cast<size_t>(limit)) continue;
            ranked.insert(position, {score, station});
            if (ranked.size() > static_cast<size_t>(limit)) ranked.pop_back();
        }
        for (auto& item : ranked) results.push_back(std::move(item.station));
        if (!reader.Finish()) return {};
        return results;
    }

    for (uint32_t index = 0; index < catalog_count; ++index) {
        RadioStationInfo station;
        if (!reader.ReadNext(station)) return {};
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

    if (!reader.Finish()) return {};
    return results;
}

namespace {
bool ContainsInsensitiveNoAlloc(const std::string& value, const std::string& query) {
    if (query.empty() || query.size() > value.size()) return query.empty();
    for (size_t start = 0; start <= value.size() - query.size(); ++start) {
        bool match = true;
        for (size_t i = 0; i < query.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
                std::tolower(static_cast<unsigned char>(query[i]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

std::vector<std::string> TokenizeQuery(const std::string& query) {
    std::istringstream stream(query);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(std::move(token));
    return tokens;
}
} // namespace

bool RadioStorage::GetCatalogStationByUuid(const std::string& station_uuid,
                                            RadioStationInfo& station) {
    if (station_uuid.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    struct stat st;
    if (stat(kBinaryCatalogPath, &st) != 0) return false;
    RadioBinaryIO::CatalogReader reader(kBinaryCatalogPath);
    uint32_t count = 0;
    if (!reader.Open(count)) return false;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        RadioStationInfo candidate;
        if (!reader.ReadNext(candidate)) return false;
        if (candidate.stationuuid == station_uuid) { station = std::move(candidate); found = true; }
    }
    return reader.Finish() && found;
}
