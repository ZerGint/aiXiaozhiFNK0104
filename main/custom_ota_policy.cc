#include "custom_ota_policy.h"

#include "ota/minimal_updater.h"

#include "board.h"
#include "system_info.h"

#include <cJSON.h>
#include <errno.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <utility>

namespace CustomOtaPolicy {
bool IsValidRemoteStableVersion(const char* version);
int CompareStableVersions(const char* left, const char* right);
}  // namespace CustomOtaPolicy

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

bool RemoveStagingPath(const char* path, const char* name, bool allow_missing = true) {
    errno = 0;
    if (unlink(path) == 0) {
        ESP_LOGI(kTag, "OTA_STAGE_REMOVE path=%s result=0 errno=0", name);
        return true;
    }
    const int error = errno;
    const bool missing = error == ENOENT;
    ESP_LOGW(kTag, "OTA_STAGE_REMOVE path=%s result=-1 errno=%d error=%s", name, error,
             strerror(error));
    return allow_missing && missing;
}

bool RenameStagingPath(const char* from, const char* to, const char* name) {
    errno = 0;
    if (rename(from, to) == 0) {
        ESP_LOGI(kTag, "OTA_STAGE_RENAME path=%s result=0 errno=0", name);
        return true;
    }
    const int error = errno;
    ESP_LOGE(kTag, "OTA_STAGE_RENAME path=%s result=-1 errno=%d error=%s", name, error,
             strerror(error));
    return false;
}

bool RecoverTemporaryStagingFiles() {
    struct TemporaryFile {
        const char* path;
        const char* name;
    };
    constexpr TemporaryFile files[] = {
        {kFirmwareTmp, "firmware.tmp"},
        {kStagedInfoTmp, "staged-info.tmp"},
        {kManifestTmp, "manifest.tmp"},
    };
    bool success = true;
    for (const auto& file : files) {
        struct stat info {};
        if (stat(file.path, &info) != 0) continue;
        ESP_LOGW(kTag, "OTA_STALE_STAGE_DETECTED file=%s kind=tmp", file.name);
        success = RemoveStagingPath(file.path, file.name) && success;
    }
    if (success) ESP_LOGI(kTag, "OTA_STALE_STAGE_CLEANUP result=pass");
    return success;
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
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kIoBufferSize]);
    if (!buffer) {
        fclose(file);
        psa_hash_abort(&operation);
        return false;
    }
    while (success) {
        const size_t count = fread(buffer.get(), 1, kIoBufferSize, file);
        if (count > 0) {
            success = psa_hash_update(&operation, buffer.get(), count) == PSA_SUCCESS;
        }
        if (count < kIoBufferSize) {
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
    ESP_LOGI(kTag, "OTA_MINIMAL_MEM_%s internal=%u largest=%u psram=%u", point,
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
    std::unique_ptr<char[]> chunk(new (std::nothrow) char[2048]);
    if (!chunk) {
        http->Close();
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=alloc");
        return false;
    }
    int bytes_read = 0;
    while ((bytes_read = http->Read(chunk.get(), 2048)) > 0) {
        if (github_response.size() + static_cast<size_t>(bytes_read) > kGithubResponseLimit) {
            bytes_read = -1;
            break;
        }
        github_response.append(chunk.get(), static_cast<size_t>(bytes_read));
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

struct StableManifestFields {
    cJSON* root = nullptr;
    cJSON* version = nullptr;
    cJSON* url = nullptr;
    cJSON* size = nullptr;
    cJSON* sha256 = nullptr;
    cJSON* board = nullptr;
    cJSON* chip = nullptr;
};

bool ParseStableManifest(const std::string& manifest, const char* current_version,
                         bool enforce_current_version, StableManifestFields* fields) {
    if (!fields) return false;
    *fields = {};
    fields->root = cJSON_ParseWithLength(manifest.data(), manifest.size());
    cJSON* firmware = fields->root ? cJSON_GetObjectItem(fields->root, "firmware") : nullptr;
    fields->version = firmware ? cJSON_GetObjectItem(firmware, "version") : nullptr;
    fields->url = firmware ? cJSON_GetObjectItem(firmware, "url") : nullptr;
    fields->size = firmware ? cJSON_GetObjectItem(firmware, "size") : nullptr;
    fields->sha256 = firmware ? cJSON_GetObjectItem(firmware, "sha256") : nullptr;
    fields->board = firmware ? cJSON_GetObjectItem(firmware, "board") : nullptr;
    fields->chip = firmware ? cJSON_GetObjectItem(firmware, "chip") : nullptr;
    cJSON* schema = fields->root ? cJSON_GetObjectItem(fields->root, "schema") : nullptr;
    cJSON* channel = fields->root ? cJSON_GetObjectItem(fields->root, "channel") : nullptr;
    const bool current_version_ok = !enforce_current_version ||
                                    (current_version && cJSON_IsString(fields->version) &&
                                     CustomOtaPolicy::CompareStableVersions(current_version,
                                                                            fields->version->valuestring) < 0);
    const bool valid = fields->root && cJSON_IsNumber(schema) && schema->valueint == 1 &&
                       cJSON_IsString(channel) && strcmp(channel->valuestring, "stable") == 0 &&
                       cJSON_IsString(fields->version) &&
                       CustomOtaPolicy::IsValidRemoteStableVersion(fields->version->valuestring) &&
                       cJSON_IsNumber(fields->size) && fields->size->valuedouble > 0.0 &&
                       fields->size->valuedouble == static_cast<double>(fields->size->valueint) &&
                       cJSON_IsString(fields->sha256) && IsHexDigest(fields->sha256->valuestring) &&
                       cJSON_IsString(fields->url) && strncmp(fields->url->valuestring, "https://", 8) == 0 &&
                       cJSON_IsString(fields->board) && strcmp(fields->board->valuestring, "freenove-fnk0104s") == 0 &&
                       cJSON_IsString(fields->chip) && strcmp(fields->chip->valuestring, "esp32s3") == 0 &&
                       current_version_ok;
    if (!valid) {
        cJSON_Delete(fields->root);
        *fields = {};
        return false;
    }
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

bool CleanupStagedFiles() {
    struct CleanupFile {
        const char* path;
        const char* name;
    };
    constexpr CleanupFile files[] = {
        {kFirmwarePath, "firmware.bin"},       {kStagedInfoPath, "staged-info.json"},
        {kFirmwareTmp, "firmware.tmp"},        {kStagedInfoTmp, "staged-info.tmp"},
        {kManifestPath, "manifest.json"},      {kManifestTmp, "manifest.tmp"},
    };

    bool success = true;
    for (const auto& file : files) {
        const bool removed = unlink(file.path) == 0 || errno == ENOENT;
        ESP_LOGI(kTag, "OTA_CLEANUP_FILE %s status=%s", file.name, removed ? "pass" : "fail");
        success = success && removed;
    }
    return success;
}

bool InstallStagedUpdate() {
    struct stat staged_info {};
    struct stat firmware_info {};
    if (stat(kStagedInfoPath, &staged_info) != 0 || stat(kFirmwarePath, &firmware_info) != 0) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=staged_file_missing");
        return false;
    }
    if (firmware_info.st_size <= 0 || static_cast<size_t>(firmware_info.st_size) > UINT32_MAX) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=staged_size");
        return false;
    }

    FILE* metadata_file = fopen(kStagedInfoPath, "rb");
    if (!metadata_file || staged_info.st_size <= 0 || staged_info.st_size > 1024) {
        if (metadata_file) fclose(metadata_file);
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=metadata_size");
        return false;
    }
    std::string metadata(static_cast<size_t>(staged_info.st_size), '\0');
    const size_t metadata_read = fread(metadata.data(), 1, metadata.size(), metadata_file);
    fclose(metadata_file);
    if (metadata_read != metadata.size()) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=metadata_read");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(metadata.data(), metadata.size());
    cJSON* schema = root ? cJSON_GetObjectItem(root, "schema") : nullptr;
    cJSON* state = root ? cJSON_GetObjectItem(root, "state") : nullptr;
    cJSON* version = root ? cJSON_GetObjectItem(root, "version") : nullptr;
    cJSON* image_version = root ? cJSON_GetObjectItem(root, "image_version") : nullptr;
    cJSON* size = root ? cJSON_GetObjectItem(root, "size") : nullptr;
    cJSON* sha = root ? cJSON_GetObjectItem(root, "sha256") : nullptr;
    cJSON* board = root ? cJSON_GetObjectItem(root, "board") : nullptr;
    cJSON* chip = root ? cJSON_GetObjectItem(root, "chip") : nullptr;
    cJSON* project = root ? cJSON_GetObjectItem(root, "image_project") : nullptr;
    const bool metadata_valid = root && cJSON_IsNumber(schema) && schema->valueint == 1 &&
                                cJSON_IsString(state) && strcmp(state->valuestring, "staged") == 0 &&
                                cJSON_IsString(version) && IsValidRemoteStableVersion(version->valuestring) &&
                                cJSON_IsString(image_version) &&
                                strcmp(image_version->valuestring, version->valuestring) == 0 &&
                                cJSON_IsNumber(size) && size->valuedouble > 0.0 &&
                                size->valuedouble == static_cast<double>(size->valueint) &&
                                size->valueint == firmware_info.st_size && cJSON_IsString(sha) &&
                                IsHexDigest(sha->valuestring) && cJSON_IsString(board) &&
                                strcmp(board->valuestring, "freenove-fnk0104s") == 0 &&
                                cJSON_IsString(chip) && strcmp(chip->valuestring, "esp32s3") == 0 &&
                                cJSON_IsString(project) && strcmp(project->valuestring, "xiaozhi") == 0;
    if (!metadata_valid) {
        if (root) cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=metadata_validation");
        return false;
    }
    const std::string staged_version = version->valuestring;
    const std::string staged_sha = sha->valuestring;
    const uint32_t staged_size = static_cast<uint32_t>(size->valueint);
    cJSON_Delete(root);

    std::array<uint8_t, 32> digest{};
    if (!ComputeFileSha256(kFirmwarePath, &digest) || !HexDigestEquals(staged_sha.c_str(), digest)) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=sha256");
        return false;
    }
    ESP_LOGI(kTag, "OTA_INSTALL_SHA256_VERIFY status=pass");
    if (!ValidateImageHeader(kFirmwarePath, staged_version.c_str())) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=image_header");
        return false;
    }
    ESP_LOGI(kTag, "OTA_INSTALL_IMAGE_VERIFY status=pass version=%s chip=esp32s3 project=xiaozhi",
             staged_version.c_str());

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* target = running ? esp_ota_get_next_update_partition(running) : nullptr;
    if (!running || !boot || !target || target == running || target->type != ESP_PARTITION_TYPE_APP ||
        target->size < staged_size) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=partition_safety");
        return false;
    }
    ESP_LOGI(kTag, "OTA_INSTALL_PARTITIONS running=%s boot=%s target=%s target_address=0x%lx target_size=%lu",
             running->label, boot->label, target->label, static_cast<unsigned long>(target->address),
             static_cast<unsigned long>(target->size));
    ESP_LOGI(kTag, "OTA_INSTALL_TARGET_INACTIVE yes");

    FILE* firmware = fopen(kFirmwarePath, "rb");
    if (!firmware) {
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=firmware_open");
        return false;
    }
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kIoBufferSize]);
    if (!buffer) {
        fclose(firmware);
        ESP_LOGW(kTag, "OTA_INSTALL_REJECTED reason=buffer_alloc");
        return false;
    }
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, staged_size, &handle);
    if (err != ESP_OK) {
        fclose(firmware);
        ESP_LOGE(kTag, "OTA_INSTALL_FAILED reason=ota_begin err=%s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "OTA_INSTALL_OTA_BEGIN status=pass");
    size_t written = 0;
    int last_percent = -1;
    while (written < staged_size) {
        const size_t want = std::min(kIoBufferSize, static_cast<size_t>(staged_size) - written);
        const size_t count = fread(buffer.get(), 1, want, firmware);
        if (count != want || esp_ota_write(handle, buffer.get(), count) != ESP_OK) {
            fclose(firmware);
            esp_ota_abort(handle);
            ESP_LOGE(kTag, "OTA_INSTALL_FAILED reason=ota_write written=%u expected=%u",
                     static_cast<unsigned>(written), static_cast<unsigned>(staged_size));
            return false;
        }
        written += count;
        const int percent = static_cast<int>(written * 100 / staged_size);
        if (percent / 5 != last_percent / 5) {
            last_percent = percent;
            ESP_LOGI(kTag, "OTA_INSTALL_PROGRESS written=%u total=%u percent=%d",
                     static_cast<unsigned>(written), static_cast<unsigned>(staged_size), percent);
        }
    }
    fclose(firmware);
    if (written != staged_size || esp_ota_end(handle) != ESP_OK) {
        ESP_LOGE(kTag, "OTA_INSTALL_FAILED reason=ota_end written=%u expected=%u",
                 static_cast<unsigned>(written), static_cast<unsigned>(staged_size));
        return false;
    }
    ESP_LOGI(kTag, "OTA_INSTALL_OTA_END status=pass bytes=%u", static_cast<unsigned>(written));
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OTA_INSTALL_FAILED reason=set_boot err=%s", esp_err_to_name(err));
        return false;
    }
    const esp_partition_t* boot_after = esp_ota_get_boot_partition();
    if (!boot_after || boot_after->address != target->address) {
        ESP_LOGE(kTag, "OTA_INSTALL_FAILED reason=boot_verify");
        return false;
    }
    ESP_LOGI(kTag, "OTA_BOOT_PARTITION_SET from=%s to=%s", boot->label, boot_after->label);
    ESP_LOGI(kTag, "OTA_INSTALL_SUCCESS version=%s", staged_version.c_str());
    return true;
}

bool StageStableUpdateImpl(NetworkInterface* network, const esp_app_desc_t* descriptor,
                           bool enforce_current_version) {
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

    StableManifestFields fields;
    if (!ParseStableManifest(manifest, descriptor ? descriptor->version : nullptr,
                             enforce_current_version, &fields)) {
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=validation");
        return false;
    }
    cJSON* root = fields.root;
    cJSON* version = fields.version;
    cJSON* url = fields.url;
    cJSON* size_item = fields.size;
    cJSON* sha = fields.sha256;
    cJSON* board = fields.board;
    cJSON* chip = fields.chip;
    ESP_LOGI(kTag, "OTA_MANIFEST_VALID version=%s size=%u", version->valuestring,
             static_cast<unsigned>(size_item->valuedouble));
    const size_t expected_size = static_cast<size_t>(size_item->valuedouble);
    size_t largest_app_partition = 0;
    esp_partition_iterator_t partition_it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                               ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (partition_it) {
        const esp_partition_t* candidate = esp_partition_get(partition_it);
        largest_app_partition = std::max(largest_app_partition, static_cast<size_t>(candidate->size));
        partition_it = esp_partition_next(partition_it);
    }
    if (largest_app_partition == 0 || expected_size == 0 || expected_size > largest_app_partition) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_MANIFEST_REJECTED reason=partition_size");
        return false;
    }

    if (!RecoverTemporaryStagingFiles()) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=stale_tmp_cleanup");
        return false;
    }

    struct stat staged_info {};
    struct stat firmware_info {};
    const bool has_staged_info = stat(kStagedInfoPath, &staged_info) == 0;
    const bool has_firmware = stat(kFirmwarePath, &firmware_info) == 0;
    bool existing_stage_matches = false;
    const char* stale_reason = nullptr;
    if (has_staged_info && has_firmware) {
        FILE* staged_file = fopen(kStagedInfoPath, "rb");
        char staged_data[512] = {};
        const size_t staged_count = staged_file ? fread(staged_data, 1, sizeof(staged_data) - 1, staged_file) : 0;
        if (staged_file) fclose(staged_file);
        cJSON* staged_root = cJSON_ParseWithLength(staged_data, staged_count);
        cJSON* staged_version = staged_root ? cJSON_GetObjectItem(staged_root, "version") : nullptr;
        cJSON* staged_size = staged_root ? cJSON_GetObjectItem(staged_root, "size") : nullptr;
        cJSON* staged_sha = staged_root ? cJSON_GetObjectItem(staged_root, "sha256") : nullptr;
        const bool metadata_matches = staged_root && cJSON_IsString(staged_version) &&
                                      cJSON_IsNumber(staged_size) &&
                                      staged_size->valuedouble == static_cast<double>(expected_size) &&
                                      cJSON_IsString(staged_sha) &&
                                      strcmp(staged_version->valuestring, version->valuestring) == 0 &&
                                      strcasecmp(staged_sha->valuestring, sha->valuestring) == 0 &&
                                      static_cast<size_t>(firmware_info.st_size) == expected_size;
        std::array<uint8_t, 32> existing_digest{};
        const bool file_matches = metadata_matches &&
                                   ComputeFileSha256(kFirmwarePath, &existing_digest) &&
                                   HexDigestEquals(sha->valuestring, existing_digest);
        existing_stage_matches = file_matches;
        stale_reason = file_matches ? nullptr : (metadata_matches ? "sha256_mismatch" : "metadata_mismatch");
        if (file_matches) {
            ESP_LOGI(kTag, "OTA_STAGE_ALREADY_PRESENT version=%s size=%u", version->valuestring, expected_size);
            cJSON_Delete(staged_root);
            cJSON_Delete(root);
            return true;
        }
        if (staged_root) cJSON_Delete(staged_root);
    } else if (has_staged_info || has_firmware) {
        stale_reason = has_staged_info ? "metadata_without_firmware" : "firmware_without_metadata";
    }
    const bool stale_stage_detected = (has_staged_info || has_firmware) && !existing_stage_matches;
    if (stale_stage_detected) {
        ESP_LOGW(kTag, "OTA_STALE_STAGE_DETECTED firmware=%d metadata=%d reason=%s", has_firmware,
                 has_staged_info, stale_reason ? stale_reason : "unknown");
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
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kIoBufferSize]);
    if (!buffer) {
        if (output) fclose(output);
        psa_hash_abort(&sha_operation);
        cJSON_Delete(root);
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=alloc");
        return false;
    }
    size_t total = 0;
    int last_percent = -1;
    while (success) {
        const int count = firmware_http->Read(buffer.get(), kIoBufferSize);
        if (count < 0) { success = false; break; }
        if (count == 0) break;
        success = fwrite(buffer.get(), 1, count, output) == static_cast<size_t>(count) &&
                  psa_hash_update(&sha_operation, reinterpret_cast<uint8_t*>(buffer.get()), count) == PSA_SUCCESS;
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
    LogMemory("BEFORE_VERIFY");
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
    LogMemory("MIN_VERIFY");
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
    if (success) {
        // FAT/VFS does not guarantee rename-over-existing.  Remove only after
        // the new temporary image has passed all verification above.
        success = RemoveStagingPath(kFirmwarePath, "firmware.bin") &&
                  RenameStagingPath(kFirmwareTmp, kFirmwarePath, "firmware.tmp->firmware.bin");
    }
    if (success) {
        struct stat committed_info {};
        errno = 0;
        const int stat_result = stat(kFirmwarePath, &committed_info);
        const int stat_error = errno;
        success = stat_result == 0 &&
                  static_cast<size_t>(committed_info.st_size) == expected_size;
        if (!success) {
            ESP_LOGE(kTag, "OTA_STAGE_COMMIT_VERIFY path=firmware.bin result=%d errno=%d error=%s",
                     stat_result, stat_error, strerror(stat_error));
        }
    }
    if (success) {
        success = RemoveStagingPath(kStagedInfoPath, "staged-info.json") &&
                  RenameStagingPath(kStagedInfoTmp, kStagedInfoPath,
                                    "staged-info.tmp->staged-info.json");
        if (success && stale_stage_detected) {
            ESP_LOGI(kTag, "OTA_STALE_STAGE_CLEANUP result=pass");
        }
    }
    if (staged_text) free(staged_text);
    const std::string committed_version = version->valuestring;
    const std::string committed_sha = sha->valuestring;
    cJSON_Delete(staged);
    cJSON_Delete(root);
    if (success) {
        if (!RemoveStagingPath(kManifestPath, "manifest.json") ||
            !RenameStagingPath(kManifestTmp, kManifestPath, "manifest.tmp->manifest.json")) {
            ESP_LOGE(kTag, "OTA_STAGE_FAILED reason=manifest_commit");
            return false;
        }
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
    } else {
        ESP_LOGW(kTag, "OTA_STAGE_FAILED reason=atomic_commit");
    }
    return success;
}

bool StageStableUpdate() {
    const auto* descriptor = esp_app_get_description();
    if (!ShouldCheckForUpdates(descriptor->version)) {
        ESP_LOGI(kTag, "OTA_POLICY_SKIP_NON_STABLE_VERSION current=%s", descriptor->version);
        return false;
    }
    return StageStableUpdateImpl(Board::GetInstance().GetNetwork(), descriptor, true);
}

bool StageStableUpdateOnNetwork(NetworkInterface* network) {
    const auto* descriptor = esp_app_get_description();
    return network && descriptor && StageStableUpdateImpl(network, descriptor, false);
}

bool CheckForStableUpdate() {
    g_stable_update_metadata = {};
    const auto* descriptor = esp_app_get_description();
#if CONFIG_CUSTOM_OTA_ACCEPT_ANY_STABLE_FOR_TEST
    ESP_LOGW(kTag, "OTA_TEST_MODE_ENABLED");
#else
    if (!ShouldCheckForUpdates(descriptor->version)) {
        ESP_LOGI(kTag, "OTA_POLICY_SKIP_NON_STABLE_VERSION current=%s", descriptor->version);
        return false;
    }
#endif
    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    ESP_LOGI(kTag, "OTA_DISCOVERY_START");
    ESP_LOGI(kTag, "OTA_MANIFEST_HTTP_REQUEST url=%s", kManifestUrl);
    std::string manifest;
    if (!FetchGithubManifest(network, &manifest)) return false;
    StableManifestFields fields;
    if (!ParseStableManifest(manifest, descriptor->version, false, &fields)) {
        ESP_LOGW(kTag, "OTA_DISCOVERY_REJECTED reason=version");
        return false;
    }
    cJSON* version = fields.version;
    cJSON* size = fields.size;
    cJSON* sha = fields.sha256;
    cJSON* url = fields.url;
    cJSON* board = fields.board;
    cJSON* chip = fields.chip;
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
    strncpy(g_stable_update_metadata.board, board->valuestring,
            sizeof(g_stable_update_metadata.board) - 1);
    strncpy(g_stable_update_metadata.chip, chip->valuestring,
            sizeof(g_stable_update_metadata.chip) - 1);
    ESP_LOGI(kTag, "OTA_VERSION_PARSE status=pass version=%s", g_stable_update_metadata.version);
#if CONFIG_CUSTOM_OTA_ACCEPT_ANY_STABLE_FOR_TEST
    g_stable_update_metadata.update_available = true;
    ESP_LOGI(kTag, "OTA_TEST_ACCEPT_ANY_STABLE version=%s", g_stable_update_metadata.version);
#else
    g_stable_update_metadata.update_available =
        CompareStableVersions(descriptor->version, g_stable_update_metadata.version) < 0;
#endif
    ESP_LOGI(kTag, "OTA_UPDATE_AVAILABLE value=%d version=%s", g_stable_update_metadata.update_available,
             g_stable_update_metadata.version);
    cJSON_Delete(fields.root);
    return g_stable_update_metadata.update_available;
}

bool IsUpdateAvailable() {
    return g_stable_update_metadata.update_available;
}

const StableUpdateMetadata& GetStableUpdateMetadata() {
    return g_stable_update_metadata;
}

bool RequestFirmwareInstall() {
    if (!g_stable_update_metadata.update_available) {
        ESP_LOGW(kTag, "OTA_UPDATE_REQUEST_REJECTED reason=no_update");
        return false;
    }
    if (!MinimalUpdater::WriteUpdateRequest(g_stable_update_metadata)) {
        ESP_LOGW(kTag, "OTA_UPDATE_REQUEST_REJECTED reason=nvs");
        return false;
    }
    ESP_LOGI(kTag, "OTA_UPDATE_REQUESTED version=%s", g_stable_update_metadata.version);
    esp_restart();
    return true;
}

bool TriggerDevUpdateRequestOnce() {
#if CONFIG_CUSTOM_OTA_DEV_REQUEST_UPDATE
    bool consumed = false;
    if (!MinimalUpdater::ReadDevRequestConsumed(&consumed)) {
        ESP_LOGE(kTag, "DEV_OTA_REQUEST_TRIGGER failed=read_guard");
        return false;
    }
    if (consumed) {
        ESP_LOGI(kTag, "DEV_OTA_REQUEST_TRIGGER skipped=consumed");
        return false;
    }

    g_stable_update_metadata = {};
    g_stable_update_metadata.update_available = true;
    strncpy(g_stable_update_metadata.version, "1.1.0",
            sizeof(g_stable_update_metadata.version) - 1);
    g_stable_update_metadata.size = 3809616;
    constexpr char kDevSha256[] =
        "a42c59fa6408a0a4b85bd1291cdee7d12dc1dd16c995e66db81417537e3df045";
    static_assert(sizeof(kDevSha256) <= sizeof(g_stable_update_metadata.sha256));
    memcpy(g_stable_update_metadata.sha256, kDevSha256, sizeof(kDevSha256));
    strncpy(g_stable_update_metadata.url,
            "https://raw.githubusercontent.com/ZerGint/FNK0104s_xiaozhi_update/main/firmware/stable/1.1.0/fnk0104s-firmware.bin",
            sizeof(g_stable_update_metadata.url) - 1);
    strncpy(g_stable_update_metadata.board, "freenove-fnk0104s",
            sizeof(g_stable_update_metadata.board) - 1);
    strncpy(g_stable_update_metadata.chip, "esp32s3",
            sizeof(g_stable_update_metadata.chip) - 1);

    if (!MinimalUpdater::MarkDevRequestConsumed()) {
        ESP_LOGE(kTag, "DEV_OTA_REQUEST_TRIGGER failed=write_guard");
        return false;
    }
    ESP_LOGI(kTag, "DEV_OTA_REQUEST_TRIGGER version=%s", g_stable_update_metadata.version);
    ESP_LOGI(kTag, "DEV_OTA_REQUEST_METADATA size=%u sha256=%s",
             static_cast<unsigned>(g_stable_update_metadata.size),
             g_stable_update_metadata.sha256);
    if (!RequestFirmwareInstall()) {
        ESP_LOGE(kTag, "DEV_OTA_REQUEST_TRIGGER failed=request");
        return false;
    }
#endif
    return false;
}

}  // namespace CustomOtaPolicy
