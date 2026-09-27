#include "minimal_updater.h"

#include "board.h"
#include "managers/storage_manager.h"
#include "ota/minimal_ota_display.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <network_interface.h>
#include <wifi_manager.h>
#include <esp_network.h>

#include <cstring>

namespace {

constexpr char kTag[] = "MinimalUpdater";
constexpr char kNamespace[] = "ota_sd";
constexpr char kStateKey[] = "state";
constexpr char kReasonKey[] = "reason";
constexpr char kVersionKey[] = "version";
constexpr char kUrlKey[] = "url";
constexpr char kShaKey[] = "sha256";
constexpr char kBoardKey[] = "board";
constexpr char kChipKey[] = "chip";
constexpr char kSizeKey[] = "size";
constexpr char kInstalledVersionKey[] = "inst_version";
constexpr char kInstalledShaKey[] = "inst_sha256";
constexpr char kPreviousPartitionKey[] = "prev_part";
constexpr char kNewPartitionKey[] = "new_part";
constexpr char kRollbackDetectedKey[] = "rollback_det";
constexpr size_t kVersionLimit = 32;
constexpr size_t kUrlLimit = 256;
constexpr size_t kShaLimit = 65;
constexpr size_t kBoardLimit = 32;
constexpr size_t kChipLimit = 16;
constexpr size_t kReasonLimit = 64;
constexpr EventBits_t kWifiConnected = BIT0;

struct InstalledMetadata {
    char version[kVersionLimit] = {};
    char previous[16] = {};
    char target[16] = {};
};

void LogMemory(const char* point) {
    ESP_LOGI(kTag, "OTA_MINIMAL_MEM_%s internal=%u largest=%u psram=%u", point,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

bool SetString(nvs_handle_t handle, const char* key, const char* value, size_t limit) {
    return value && strnlen(value, limit + 1) <= limit && nvs_set_str(handle, key, value) == ESP_OK;
}

bool ReadString(nvs_handle_t handle, const char* key, char* value, size_t capacity) {
    if (!value || capacity == 0) return false;
    value[0] = '\0';
    size_t length = capacity;
    return nvs_get_str(handle, key, value, &length) == ESP_OK;
}

bool ConnectWifi() {
    auto& wifi = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.station_scan_min_interval_seconds = 2;
    config.station_scan_max_interval_seconds = 10;
    config.show_ota_config = false;
    config.show_sleep_config = false;
    auto events = xEventGroupCreate();
    if (!events || !wifi.Initialize(config)) {
        if (events) vEventGroupDelete(events);
        return false;
    }
    wifi.SetEventCallback([events](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected) xEventGroupSetBits(events, kWifiConnected);
    });
    wifi.StartStation();
    const EventBits_t bits = xEventGroupWaitBits(events, kWifiConnected, pdTRUE, pdFALSE,
                                                  pdMS_TO_TICKS(60000));
    vEventGroupDelete(events);
    return (bits & kWifiConnected) != 0 && wifi.IsConnected();
}

bool ReadInstalledMetadata(InstalledMetadata* metadata, MinimalUpdater::State* state) {
    if (!metadata || !state) return false;
    *metadata = {};
    *state = MinimalUpdater::State::STABLE;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t raw = 0;
    const bool state_ok = nvs_get_u8(handle, kStateKey, &raw) == ESP_OK;
    if (state_ok) {
        if (raw <= static_cast<uint8_t>(MinimalUpdater::State::INSTALLED)) {
            *state = static_cast<MinimalUpdater::State>(raw);
        } else if (raw == 7) {
            // Migrate the previous PENDING_VERIFY value to INSTALLED.
            *state = MinimalUpdater::State::INSTALLED;
        }
    }
    if (*state == MinimalUpdater::State::INSTALLED) {
        ReadString(handle, kInstalledVersionKey, metadata->version, sizeof(metadata->version));
        ReadString(handle, kPreviousPartitionKey, metadata->previous, sizeof(metadata->previous));
        ReadString(handle, kNewPartitionKey, metadata->target, sizeof(metadata->target));
    }
    nvs_close(handle);
    return state_ok;
}

bool MarkInstalled(const char* previous, const char* target) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    char version[kVersionLimit] = {};
    char sha[kShaLimit] = {};
    ReadString(handle, kVersionKey, version, sizeof(version));
    ReadString(handle, kShaKey, sha, sizeof(sha));
    bool success = version[0] && nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(MinimalUpdater::State::INSTALLED)) == ESP_OK &&
                   SetString(handle, kInstalledVersionKey, version, kVersionLimit) &&
                   SetString(handle, kInstalledShaKey, sha, kShaLimit) &&
                   SetString(handle, kPreviousPartitionKey, previous, sizeof(((InstalledMetadata*)0)->previous)) &&
                   SetString(handle, kNewPartitionKey, target, sizeof(((InstalledMetadata*)0)->target)) &&
                   nvs_set_u8(handle, kRollbackDetectedKey, 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool ClearTransientMetadata() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    constexpr const char* keys[] = {kReasonKey, kVersionKey, kUrlKey, kShaKey, kBoardKey,
                                    kChipKey, kSizeKey, kInstalledVersionKey,
                                    kInstalledShaKey, kPreviousPartitionKey, kNewPartitionKey};
    bool success = true;
    for (const char* key : keys) {
        const esp_err_t err = nvs_erase_key(handle, key);
        success = success && (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);
    }
    success = success && nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(MinimalUpdater::State::STABLE)) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (success) ESP_LOGI(kTag, "OTA_NVS_METADATA_CLEANED state=STABLE");
    return success;
}

bool FailAndReboot(const char* reason) {
    MinimalUpdater::MarkState(MinimalUpdater::State::STABLE, reason, nullptr);
    ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=%s", reason ? reason : "unknown");
    ESP_LOGI(kTag, "OTA_MINIMAL_REBOOT_AFTER_FAILURE reason=%s", reason ? reason : "unknown");
    esp_restart();
    return true;
}

}  // namespace

namespace MinimalUpdater {

bool ReadState(State* state) {
    if (!state) return false;
    *state = State::STABLE;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return true;
    uint8_t raw = 0;
    const esp_err_t result = nvs_get_u8(handle, kStateKey, &raw);
    nvs_close(handle);
    if (result == ESP_OK && raw <= static_cast<uint8_t>(State::INSTALLED)) {
        *state = static_cast<State>(raw);
    } else if (result == ESP_OK && raw == 7) {
        *state = State::INSTALLED;
    }
    return true;
}

bool WriteUpdateRequest(const CustomOtaPolicy::StableUpdateMetadata& metadata) {
    if (!metadata.update_available || !CustomOtaPolicy::IsStrictStableVersion(metadata.version) ||
        metadata.size == 0 || !metadata.url[0] || !metadata.sha256[0] || !metadata.board[0] ||
        !metadata.chip[0]) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool success = SetString(handle, kVersionKey, metadata.version, kVersionLimit) &&
                   SetString(handle, kUrlKey, metadata.url, kUrlLimit) &&
                   SetString(handle, kShaKey, metadata.sha256, kShaLimit) &&
                   SetString(handle, kBoardKey, metadata.board, kBoardLimit) &&
                   SetString(handle, kChipKey, metadata.chip, kChipLimit) &&
                   nvs_set_u32(handle, kSizeKey, metadata.size) == ESP_OK &&
                   nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::UPDATE)) == ESP_OK &&
                   nvs_set_u8(handle, kRollbackDetectedKey, 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool MarkState(State state, const char* reason, const char* version) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool success = nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(state)) == ESP_OK;
    if (reason) success = success && SetString(handle, kReasonKey, reason, kReasonLimit);
    if (version) success = success && SetString(handle, kVersionKey, version, kVersionLimit);
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

void ReportPendingVerification() {
    InstalledMetadata metadata;
    State state = State::STABLE;
    if (ReadInstalledMetadata(&metadata, &state) && state == State::INSTALLED) {
        ESP_LOGI(kTag, "OTA_INSTALLED_PENDING version=%s target=%s", metadata.version,
                 metadata.target);
    }
}

bool IsValidationPendingOrFailed() {
    State state = State::STABLE;
    if (!ReadState(&state)) return true;
    return state == State::INSTALLED;
}

bool ValidatePendingUpdate() {
    InstalledMetadata metadata;
    State state = State::STABLE;
    if (!ReadInstalledMetadata(&metadata, &state) || state != State::INSTALLED) return true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_app_desc_t* descriptor = esp_app_get_description();
    const bool partition_ok = running && boot && metadata.target[0] &&
                              strcmp(running->label, metadata.target) == 0 &&
                              strcmp(boot->label, metadata.target) == 0;
    if (!partition_ok) {
        ESP_LOGE(kTag, "OTA_ROLLBACK_DETECTED running=%s expected=%s",
                 running ? running->label : "none", metadata.target);
        MarkState(State::STABLE, "ROLLBACK_DETECTED", metadata.version);
        return false;
    }
    if (!descriptor || strcmp(descriptor->version, metadata.version) != 0) {
        ESP_LOGE(kTag, "OTA_POST_UPDATE_VERSION_MISMATCH running=%s expected=%s",
                 descriptor ? descriptor->version : "none", metadata.version);
        MarkState(State::STABLE, "VERIFY_FAILED", descriptor ? descriptor->version : nullptr);
        return false;
    }
    const esp_err_t mark_result = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kTag, "OTA_POST_UPDATE_MARK_VALID status=%s err=%s",
             mark_result == ESP_OK ? "pass" : "fail", esp_err_to_name(mark_result));
    if (mark_result != ESP_OK) {
        MarkState(State::STABLE, "MARK_VALID_FAILED", metadata.version);
        return false;
    }
    ESP_LOGI(kTag, "OTA_POST_UPDATE_STATE STABLE");
    ClearTransientMetadata();
    return true;
}

bool RunIfRequested() {
    static bool entered = false;
    State state = State::STABLE;
    ReadState(&state);
    if (state != State::UPDATE) return false;
    if (entered) return true;
    entered = true;

    ESP_LOGI(kTag, "OTA_MINIMAL_MODE_ENTER state=UPDATE");
    MinimalOtaDisplay display;
    display.Init();
    LogMemory("BOOT");
    if (!ConnectWifi()) return FailAndReboot("wifi");
    ESP_LOGI(kTag, "OTA_MINIMAL_WIFI_CONNECTED");
    if (!StorageManager::GetInstance().InitializeSdCard()) return FailAndReboot("sd_mount");
    if (!CustomOtaPolicy::ClearUpdateDirectory()) return FailAndReboot("clear_update_dir");
    EspNetwork network;
    LogMemory("BEFORE_DOWNLOAD");
    if (!CustomOtaPolicy::StageStableUpdateOnNetwork(&network)) return FailAndReboot("stage");
    LogMemory("AFTER_STAGE");

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = running ? esp_ota_get_next_update_partition(running) : nullptr;
    char previous[16] = {};
    char target_label[16] = {};
    if (running) strncpy(previous, running->label, sizeof(previous) - 1);
    if (target) strncpy(target_label, target->label, sizeof(target_label) - 1);
    if (!target || !target_label[0]) return FailAndReboot("target_partition");
    if (!MarkInstalled(previous, target_label)) {
        return FailAndReboot("state_installed");
    }
    if (!CustomOtaPolicy::InstallStagedUpdate()) return FailAndReboot("install");
    if (!CustomOtaPolicy::ClearUpdateDirectory()) return FailAndReboot("clear_after_install");
    ESP_LOGI(kTag, "OTA_NVS_STATE INSTALLED target=%s", target_label);
    ESP_LOGI(kTag, "OTA_INSTALL_PENDING_VERIFY reboot=1");
    esp_restart();
    return true;
}

}  // namespace MinimalUpdater
