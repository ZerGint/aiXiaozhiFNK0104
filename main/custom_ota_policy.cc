#include "custom_ota_policy.h"

#include "board.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <strings.h>
#include <utility>

namespace {

constexpr char kTag[] = "CustomOta";
constexpr char kManifestUrl[] =
    "https://api.github.com/repos/ZerGint/FNK0104s_xiaozhi_update/contents/ota/stable.json?ref=main";
constexpr char kUpdateDir[] = "/sdcard/update";
constexpr char kManifestTmp[] = "/sdcard/update/manifest.tmp";
constexpr char kManifestPath[] = "/sdcard/update/manifest.json";
constexpr char kFirmwareTmp[] = "/sdcard/update/firmware.tmp";
constexpr char kFirmwarePath[] = "/sdcard/update/firmware.bin";
constexpr char kStagedInfoTmp[] = "/sdcard/update/staged-info.tmp";
constexpr char kStagedInfoPath[] = "/sdcard/update/staged-info.json";
constexpr size_t kManifestLimit = 8 * 1024;
constexpr size_t kGithubResponseLimit = 16 * 1024;
constexpr size_t kIoBufferSize = 4096;
CustomOtaPolicy::StableUpdateMetadata g_stable_update_metadata;

bool ParseTriplet(const char* value, int out[3]) {
    if (!value || !*value) return false;
    const char* cursor = value;
    for (int index = 0; index < 3; ++index) {
        if (index > 0) {
            if (*cursor != '.') return false;
            ++cursor;
        }
        if (*cursor < '0' || *cursor > '9') return false;
        int number = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            number = std::min(number * 10 + (*cursor - '0'), 100000000);
            ++cursor;
        }
        out[index] = number;
    }
    return *cursor == '\0';
}

bool HexDigestEquals(const char* expected, const std::array<uint8_t, 32>& digest) {
    if (!expected || strlen(expected) != 64) return false;
    char actual[65] = {};
    for (size_t i = 0; i < digest.size(); ++i) {
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    }
    return strcasecmp(actual, expected) == 0;
}

bool IsHexDigest(const char* value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t index = 0; index < 64; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool ValidateImageHeader(const char* path, const char* expected_version) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    std::array<uint8_t, 0x200> data{};
    const size_t count = fread(data.data(), 1, data.size(), file);
    fclose(file);
    constexpr size_t kFirstSegmentHeader = 0x18;
    constexpr size_t kFirstSegmentData = kFirstSegmentHeader + 8;
    if (count < kFirstSegmentData + sizeof(esp_app_desc_t) || data[0] != 0xE9 ||
        data[1] == 0 || data[0xC] != 0x09 || data[0xD] != 0x00) return false;
    const uint32_t first_segment_length = data[kFirstSegmentHeader + 4] |
                                          (data[kFirstSegmentHeader + 5] << 8) |
                                          (data[kFirstSegmentHeader + 6] << 16) |
                                          (data[kFirstSegmentHeader + 7] << 24);
    if (first_segment_length < sizeof(esp_app_desc_t)) return false;
    esp_app_desc_t descriptor{};
    memcpy(&descriptor, data.data() + kFirstSegmentData, sizeof(descriptor));
    return strcmp(descriptor.version, expected_version) == 0 &&
           strcmp(descriptor.project_name, "xiaozhi") == 0;
}

bool ComputeFileSha256(const char* path, std::array<uint8_t, 32>* digest) {
    if (!digest) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool success = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    std::array<uint8_t, kIoBufferSize> buffer{};
    while (success) {
        const size_t count = fread(buffer.data(), 1, buffer.size(), file);
        if (count > 0) {
            success = psa_hash_update(&operation, buffer.data(), count) == PSA_SUCCESS;
        }
        if (count < buffer.size()) {
            success = success && feof(file);
            break;
        }
    }
    fclose(file);
    size_t digest_length = 0;
    success = success && psa_hash_finish(&operation, digest->data(), digest->size(), &digest_length) == PSA_SUCCESS &&
              digest_length == digest->size();
    if (!success) psa_hash_abort(&operation);
    return success;
}

void LogMemory(const char* point) {
    ESP_LOGI(kTag, "OTA_MEM_%s internal=%u largest=%u psram=%u", point,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

bool DecodeGithubManifest(const std::string& response, std::string* manifest) {
    if (!manifest || response.empty() || response.size() > kGithubResponseLimit) return false;
    cJSON* wrapper = cJSON_ParseWithLength(response.data(), response.size());
    if (!wrapper) return false;
    cJSON* content = cJSON_GetObjectItem(wrapper, "content");
    cJSON* encoding = cJSON_GetObjectItem(wrapper, "encoding");
    bool success = cJSON_IsString(content) && cJSON_IsString(encoding) &&
                   strcmp(encoding->valuestring, "base64") == 0;
    if (success) {
        const size_t encoded_length = strlen(content->valuestring);
        std::string decoded(kManifestLimit, '\0');
        size_t decoded_length = 0;
        success = encoded_length > 0 &&
                  mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded.data()),
                                        decoded.size(), &decoded_length,
                                        reinterpret_cast<const unsigned char*>(content->valuestring),
                                        encoded_length) == 0 &&
                  decoded_length > 0 && decoded_length <= kManifestLimit;
        if (success) {
            decoded.resize(decoded_length);
            *manifest = std::move(decoded);
        }
    }
    cJSON_Delete(wrapper);
    return success;
}

bool FetchGithubManifest(NetworkInterface* network, std::string* manifest) {
    if (!network || !manifest) return false;
    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);
    http->SetHeader("Accept", "application/vnd.github+json");
    http->SetHeader("User-Agent", "FNK0104S-OTA");
    http->SetHeader("X-GitHub-Api-Version", "2022-11-28");
    if (!http->Open("GET", kManifestUrl) || http->GetStatusCode() != 200) {
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=http");
        return false;
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > kGithubResponseLimit) {
        http->Close();
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=size");
        return false;
    }
    std::string github_response;
    github_response.reserve(content_length);
    std::array<char, 2048> chunk{};
    int bytes_read = 0;
    while ((bytes_read = http->Read(chunk.data(), chunk.size())) > 0) {
        if (github_response.size() + static_cast<size_t>(bytes_read) > kGithubResponseLimit) {
            bytes_read = -1;
            break;
        }
        github_response.append(chunk.data(), static_cast<size_t>(bytes_read));
    }
    http->Close();
    if (bytes_read < 0 || github_response.empty() || github_response.size() != content_length ||
        !DecodeGithubManifest(github_response, manifest) || manifest->size() > kManifestLimit) {
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=github_contents_decode");
        return false;
    }
    ESP_LOGI(kTag, "OTA_MANIFEST_DECODED bytes=%u", static_cast<unsigned>(manifest->size()));
    return true;
}

}  // namespace

namespace CustomOtaPolicy {

bool IsStrictStableVersion(const char* version) {
    int parts[3] = {};
    return ParseTriplet(version, parts);
}

bool ShouldCheckForUpdates(const char* current_version) {
    return IsStrictStableVersion(current_version);
}

bool IsValidRemoteStableVersion(const char* version) {
    return IsStrictStableVersion(version);
}

int CompareStableVersions(const char* left, const char* right) {
    int lhs[3] = {}, rhs[3] = {};
    if (!ParseTriplet(left, lhs) || !ParseTriplet(right, rhs)) return 0;
    for (int index = 0; index < 3; ++index) {
        if (lhs[index] != rhs[index]) return lhs[index] < rhs[index] ? -1 : 1;
    }
    return 0;
}

void ReportStagedUpdate() {
    struct stat info {};
    if (stat(kStagedInfoPath, &info) != 0) return;
    FILE* file = fopen(kStagedInfoPath, "rb");
    if (!file) return;
    char data[512] = {};
    const size_t count = fread(data, 1, sizeof(data) - 1, file);
    fclose(file);
    cJSON* root = cJSON_ParseWithLength(data, count);
    if (!root) return;
    cJSON* version = cJSON_GetObjectItem(root, "version");
    cJSON* size = cJSON_GetObjectItem(root, "size");
    ESP_LOGI(kTag, "OTA_STAGED_FOUND version=%s size=%lld",
             cJSON_IsString(version) ? version->valuestring : "unknown",
             cJSON_IsNumber(size) ? static_cast<long long>(size->valuedouble) : 0LL);
    cJSON_Delete(root);
}

bool StageStableUpdateImpl(const esp_app_desc_t* descriptor, bool enforce_current_version) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    mkdir(kUpdateDir, 0775);
    LogMemory("BEFORE_MANIFEST");
    ESP_LOGI(kTag, "OTA_STAGE_START mode=%s", enforce_current_version ? "automatic" : "developer");
    ESP_LOGI(kTag, "OTA_MANIFEST_HTTP_REQUEST url=%s", kManifestUrl);
    std::string manifest;
    if (!FetchGithubManifest(network, &manifest)) return false;
    LogMemory("AFTER_MANIFEST");
    FILE* manifest_file = fopen(kManifestTmp, "wb");
    if (!manifest_file || fwrite(manifest.data(), 1, manifest.size(), manifest_file) != manifest.size()) {
        if (manifest_file) fclose(manifest_file);
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=manifest_write");
        return false;
    }
    fclose(manifest_file);

    cJSON* root = cJSON_ParseWithLength(manifest.data(), manifest.size());
    cJSON* firmware = root ? cJSON_GetObjectItem(root, "firmware") : nullptr;
    cJSON* version = firmware ? cJSON_GetObjectItem(firmware, "version") : nullptr;
    cJSON* url = firmware ? cJSON_GetObjectItem(firmware, "url") : nullptr;
    cJSON* size_item = firmware ? cJSON_GetObjectItem(firmware, "size") : nullptr;
    cJSON* sha = firmware ? cJSON_GetObjectItem(firmware, "sha256") : nullptr;
    cJSON* board = firmware ? cJSON_GetObjectItem(firmware, "board") : nullptr;
    cJSON* chip = firmware ? cJSON_GetObjectItem(firmware, "chip") : nullptr;
    cJSON* schema_item = root ? cJSON_GetObjectItem(root, "schema") : nullptr;
    cJSON* channel = root ? cJSON_GetObjectItem(root, "channel") : nullptr;
    const int schema = cJSON_IsNumber(schema_item) ? schema_item->valueint : 0;
    const bool valid = root && schema == 1 && cJSON_IsString(version) &&
                       cJSON_IsString(url) && cJSON_IsNumber(size_item) && cJSON_IsString(sha) &&
                       cJSON_IsString(board) && cJSON_IsString(chip) &&
                       cJSON_IsString(channel) && strcmp(channel->valuestring, "stable") == 0 &&
                       IsValidRemoteStableVersion(version->valuestring) &&
                       strcmp(board->valuestring, "freenove-fnk0104s") == 0 &&
                       strcmp(chip->valuestring, "esp32s3") == 0 &&
                       size_item->valuedouble > 0.0 &&
                       size_item->valuedouble == static_cast<double>(size_item->valueint) &&
                       strncmp(url->valuestring, "https://", 8) == 0 &&
                       (!enforce_current_version ||
                        CompareStableVersions(descriptor->version, version->valuestring) < 0) &&
                       IsHexDigest(sha->valuestring);
    if (!valid) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=validation");
        return false;
    }
    ESP_LOGI(kTag, "OTA_MANIFEST_VALID version=%s size=%u", version->valuestring,
             static_cast<unsigned>(size_item->valuedouble));
    const auto* partition = esp_ota_get_next_update_partition(nullptr);
    const size_t expected_size = static_cast<size_t>(size_item->valuedouble);
    if (!partition || expected_size == 0 || expected_size > partition->size) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=partition_size");
        return false;
    }

    struct stat staged_info {};
    if (stat(kStagedInfoPath, &staged_info) == 0) {
        FILE* staged_file = fopen(kStagedInfoPath, "rb");
        char staged_data[512] = {};
        const size_t staged_count = staged_file ? fread(staged_data, 1, sizeof(staged_data) - 1, staged_file) : 0;
        if (staged_file) fclose(staged_file);
        cJSON* staged_root = cJSON_ParseWithLength(staged_data, staged_count);
        cJSON* staged_version = staged_root ? cJSON_GetObjectItem(staged_root, "version") : nullptr;
        cJSON* staged_sha = staged_root ? cJSON_GetObjectItem(staged_root, "sha256") : nullptr;
        struct stat firmware_info {};
        const bool already_present = staged_root && stat(kFirmwarePath, &firmware_info) == 0 &&
                                      static_cast<size_t>(firmware_info.st_size) == expected_size &&
                                      cJSON_IsString(staged_version) && cJSON_IsString(staged_sha) &&
                                      strcmp(staged_version->valuestring, version->valuestring) == 0 &&
                                      strcasecmp(staged_sha->valuestring, sha->valuestring) == 0;
        if (already_present) {
            ESP_LOGI(kTag, "OTA_STAGE_ALREADY_PRESENT version=%s size=%u", version->valuestring, expected_size);
            cJSON_Delete(staged_root);
            cJSON_Delete(root);
            return true;
        }
        cJSON_Delete(staged_root);
    }

    LogMemory("BEFORE_DOWNLOAD");
    ESP_LOGI(kTag, "OTA_DOWNLOAD_START url=%s", url->valuestring);
    auto firmware_http = network->CreateHttp(0);
    firmware_http->SetTimeout(30000);
    if (!firmware_http->Open("GET", url->valuestring) || firmware_http->GetStatusCode() != 200 ||
        firmware_http->GetBodyLength() != expected_size) {
        firmware_http->Close();
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=download_open");
        return false;
    }
    FILE* output = fopen(kFirmwareTmp, "wb");
    psa_hash_operation_t sha_operation = PSA_HASH_OPERATION_INIT;
    bool success = output && psa_hash_setup(&sha_operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    std::array<char, kIoBufferSize> buffer{};
    size_t total = 0;
    int last_percent = -1;
    while (success) {
        const int count = firmware_http->Read(buffer.data(), buffer.size());
        if (count < 0) { success = false; break; }
        if (count == 0) break;
        success = fwrite(buffer.data(), 1, count, output) == static_cast<size_t>(count) &&
                  psa_hash_update(&sha_operation, reinterpret_cast<uint8_t*>(buffer.data()), count) == PSA_SUCCESS;
        total += static_cast<size_t>(count);
        const int percent = static_cast<int>(total * 100 / expected_size);
        if (percent / 5 != last_percent / 5) {
            last_percent = percent;
            ESP_LOGI(kTag, "OTA_STAGE_DOWNLOAD bytes=%u total=%u percent=%d", total, expected_size, percent);
        }
    }
    firmware_http->Close();
    if (output) { fflush(output); fclose(output); }
    ESP_LOGI(kTag, "OTA_DOWNLOAD_COMPLETE bytes=%u", static_cast<unsigned>(total));
    ESP_LOGI(kTag, "OTA_SIZE_VERIFY expected=%u actual=%u", static_cast<unsigned>(expected_size),
             static_cast<unsigned>(total));
    std::array<uint8_t, 32> digest{};
    size_t digest_length = 0;
    success = success && total == expected_size &&
              psa_hash_finish(&sha_operation, digest.data(), digest.size(), &digest_length) == PSA_SUCCESS &&
              digest_length == digest.size();
    if (!success) psa_hash_abort(&sha_operation);
    if (success) ESP_LOGI(kTag, "OTA_SHA256_VERIFY status=pass");
    LogMemory("MIN_DOWNLOAD");
    const bool sha_matches = success && HexDigestEquals(sha->valuestring, digest);
    if (sha_matches) ESP_LOGI(kTag, "OTA_SHA256_VERIFY manifest_match=pass");
    const bool image_valid = sha_matches && ValidateImageHeader(kFirmwareTmp, version->valuestring);
    if (image_valid) {
        ESP_LOGI(kTag, "OTA_IMAGE_VERIFY status=pass");
        ESP_LOGI(kTag, "OTA_DESCRIPTOR_VERIFY version=%s project=xiaozhi", version->valuestring);
    }
    if (!image_valid) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=verify");
        return false;
    }
    LogMemory("AFTER_VERIFY");
    cJSON* staged = cJSON_CreateObject();
    cJSON_AddNumberToObject(staged, "schema", 1);
    cJSON_AddStringToObject(staged, "state", "staged");
    cJSON_AddStringToObject(staged, "version", version->valuestring);
    cJSON_AddNumberToObject(staged, "size", static_cast<double>(expected_size));
    cJSON_AddStringToObject(staged, "sha256", sha->valuestring);
    cJSON_AddStringToObject(staged, "board", board->valuestring);
    cJSON_AddStringToObject(staged, "chip", chip->valuestring);
    cJSON_AddStringToObject(staged, "source_url", url->valuestring);
    cJSON_AddStringToObject(staged, "image_version", version->valuestring);
    cJSON_AddStringToObject(staged, "image_project", "xiaozhi");
    char* staged_text = cJSON_PrintUnformatted(staged);
    FILE* staged_file = fopen(kStagedInfoTmp, "wb");
    success = staged_text && staged_file && fwrite(staged_text, 1, strlen(staged_text), staged_file) == strlen(staged_text);
    if (staged_file) { fflush(staged_file); fclose(staged_file); }
    if (success) success = rename(kFirmwareTmp, kFirmwarePath) == 0 && rename(kStagedInfoTmp, kStagedInfoPath) == 0;
    if (staged_text) free(staged_text);
    const std::string committed_version = version->valuestring;
    const std::string committed_sha = sha->valuestring;
    cJSON_Delete(staged);
    cJSON_Delete(root);
    if (success) {
        rename(kManifestTmp, kManifestPath);
        ESP_LOGI(kTag, "OTA_STAGE_COMMIT state=staged");
        std::array<uint8_t, 32> readback_digest{};
        const bool readback_ok = ComputeFileSha256(kFirmwarePath, &readback_digest);
        ESP_LOGI(kTag, "OTA_STAGED_READBACK_SHA256 status=%s", readback_ok ? "pass" : "fail");
        if (readback_ok && HexDigestEquals(committed_sha.c_str(), readback_digest)) {
            ESP_LOGI(kTag, "OTA_STAGED_READBACK_SHA256 manifest_match=pass");
        } else {
            ESP_LOGW(kTag, "OTA_STAGED_READBACK_SHA256 manifest_match=fail");
        }
        ESP_LOGI(kTag, "OTA_STAGE_STAGED version=%s size=%u", committed_version.c_str(), expected_size);
        LogMemory("AFTER_STAGE");
    }
    return success;
}

bool StageStableUpdate() {
    const auto* descriptor = esp_app_get_description();
    if (!ShouldCheckForUpdates(descriptor->version)) {
        ESP_LOGI(kTag, "OTA_POLICY_SKIP_NON_STABLE_VERSION current=%s", descriptor->version);
        return false;
    }
    return StageStableUpdateImpl(descriptor, true);
}

bool CheckForStableUpdate() {
    g_stable_update_metadata = {};
    const auto* descriptor = esp_app_get_description();
    if (!ShouldCheckForUpdates(descriptor->version)) {
        ESP_LOGI(kTag, "OTA_POLICY_SKIP_NON_STABLE_VERSION current=%s", descriptor->version);
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    ESP_LOGI(kTag, "OTA_DISCOVERY_START");
    ESP_LOGI(kTag, "OTA_MANIFEST_HTTP_REQUEST url=%s", kManifestUrl);
    std::string manifest;
    if (!FetchGithubManifest(network, &manifest)) return false;
    cJSON* root = cJSON_ParseWithLength(manifest.data(), manifest.size());
    cJSON* firmware = root ? cJSON_GetObjectItem(root, "firmware") : nullptr;
    cJSON* version = firmware ? cJSON_GetObjectItem(firmware, "version") : nullptr;
    cJSON* size = firmware ? cJSON_GetObjectItem(firmware, "size") : nullptr;
    cJSON* sha = firmware ? cJSON_GetObjectItem(firmware, "sha256") : nullptr;
    cJSON* url = firmware ? cJSON_GetObjectItem(firmware, "url") : nullptr;
    const bool parsed = cJSON_IsString(version) && IsValidRemoteStableVersion(version->valuestring);
    if (!parsed) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_DISCOVERY_REJECTED reason=version");
        return false;
    }
    strncpy(g_stable_update_metadata.version, version->valuestring,
            sizeof(g_stable_update_metadata.version) - 1);
    if (cJSON_IsNumber(size) && size->valuedouble > 0.0) {
        g_stable_update_metadata.size = static_cast<uint32_t>(size->valuedouble);
    }
    if (cJSON_IsString(sha)) {
        strncpy(g_stable_update_metadata.sha256, sha->valuestring,
                sizeof(g_stable_update_metadata.sha256) - 1);
    }
    if (cJSON_IsString(url)) {
        strncpy(g_stable_update_metadata.url, url->valuestring,
                sizeof(g_stable_update_metadata.url) - 1);
    }
    ESP_LOGI(kTag, "OTA_VERSION_PARSE status=pass version=%s", g_stable_update_metadata.version);
    g_stable_update_metadata.update_available =
        CompareStableVersions(descriptor->version, g_stable_update_metadata.version) < 0;
    ESP_LOGI(kTag, "OTA_UPDATE_AVAILABLE value=%d version=%s", g_stable_update_metadata.update_available,
             g_stable_update_metadata.version);
    cJSON_Delete(root);
    return g_stable_update_metadata.update_available;
}

bool IsUpdateAvailable() {
    return g_stable_update_metadata.update_available;
}

const StableUpdateMetadata& GetStableUpdateMetadata() {
    return g_stable_update_metadata;
}

}  // namespace CustomOtaPolicy
