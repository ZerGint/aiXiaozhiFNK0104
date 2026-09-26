#include "custom_ota_policy.h"

#include "board.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <strings.h>

namespace {

constexpr char kTag[] = "CustomOta";
constexpr char kManifestUrl[] =
    "https://raw.githubusercontent.com/ZerGint/FNK0104s_xiaozhi_update/main/ota/stable.json";
constexpr char kUpdateDir[] = "/sdcard/update";
constexpr char kManifestTmp[] = "/sdcard/update/manifest.tmp";
constexpr char kManifestPath[] = "/sdcard/update/manifest.json";
constexpr char kFirmwareTmp[] = "/sdcard/update/firmware.tmp";
constexpr char kFirmwarePath[] = "/sdcard/update/firmware.bin";
constexpr char kStagedInfoTmp[] = "/sdcard/update/staged-info.tmp";
constexpr char kStagedInfoPath[] = "/sdcard/update/staged-info.json";
constexpr size_t kManifestLimit = 8 * 1024;
constexpr size_t kIoBufferSize = 4096;

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
    std::array<uint8_t, 0x4000> data{};
    const size_t count = fread(data.data(), 1, data.size(), file);
    fclose(file);
    if (count < 0x40 || data[0] != 0xE9 || data[0xC] != 0x09 || data[0xD] != 0x00) return false;
    const uint8_t segments = data[1];
    size_t offset = 0x18;
    const uint8_t* app_desc = nullptr;
    for (uint8_t index = 0; index < segments; ++index) {
        if (offset + 8 > count) return false;
        const uint32_t length = data[offset + 4] | (data[offset + 5] << 8) |
                                (data[offset + 6] << 16) | (data[offset + 7] << 24);
        offset += 8;
        if (offset + length > count) return false;
        if (index == 0) app_desc = data.data() + offset;
        offset += length;
    }
    if (!app_desc || app_desc + sizeof(esp_app_desc_t) > data.data() + count) return false;
    esp_app_desc_t descriptor{};
    memcpy(&descriptor, app_desc, sizeof(descriptor));
    return strcmp(descriptor.version, expected_version) == 0 &&
           strcmp(descriptor.project_name, "xiaozhi") == 0;
}

void LogMemory(const char* point) {
    ESP_LOGI(kTag, "OTA_MEM_%s internal=%u largest=%u psram=%u", point,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

bool StageStableUpdateImpl(const esp_app_desc_t* descriptor) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    mkdir(kUpdateDir, 0775);
    LogMemory("BEFORE_MANIFEST");
    ESP_LOGI(kTag, "OTA_MANIFEST_HTTP_REQUEST url=%s", kManifestUrl);
    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);
    http->SetHeader("Accept", "application/json");
    if (!http->Open("GET", kManifestUrl) || http->GetStatusCode() != 200) {
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=http");
        return false;
    }
    const size_t content_length = http->GetBodyLength();
    if (content_length == 0 || content_length > kManifestLimit) {
        http->Close();
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=size");
        return false;
    }
    const std::string manifest = http->ReadAll();
    http->Close();
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
                       CompareStableVersions(descriptor->version, version->valuestring) < 0 &&
                       IsHexDigest(sha->valuestring);
    if (!valid) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=validation");
        return false;
    }
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
    std::array<uint8_t, 32> digest{};
    size_t digest_length = 0;
    success = success && total == expected_size &&
              psa_hash_finish(&sha_operation, digest.data(), digest.size(), &digest_length) == PSA_SUCCESS &&
              digest_length == digest.size();
    if (!success) psa_hash_abort(&sha_operation);
    LogMemory("MIN_DOWNLOAD");
    if (!success || !HexDigestEquals(sha->valuestring, digest) || !ValidateImageHeader(kFirmwareTmp, version->valuestring)) {
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
    cJSON_Delete(staged);
    cJSON_Delete(root);
    if (success) {
        rename(kManifestTmp, kManifestPath);
        ESP_LOGI(kTag, "OTA_STAGE_STAGED version=%s size=%u", version->valuestring, expected_size);
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
    return StageStableUpdateImpl(descriptor);
}

}  // namespace CustomOtaPolicy
