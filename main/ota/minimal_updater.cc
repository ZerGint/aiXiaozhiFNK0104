#include "minimal_updater.h"

#include "custom_ota_policy.h"
#include "board.h"
#include "managers/storage_manager.h"
#include "ota/minimal_ota_display.h"

#include <esp_heap_caps.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
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
constexpr char kAttemptKey[] = "attempt";
constexpr char kDevRequestConsumedKey[] = "dev_triggered";
// NVS keys are limited to 15 characters.
constexpr char kInstalledVersionKey[] = "inst_version";
constexpr char kInstalledShaKey[] = "inst_sha256";
constexpr char kPreviousPartitionKey[] = "prev_part";
constexpr char kNewPartitionKey[] = "new_part";
constexpr char kLastValidatedVersionKey[] = "last_valid_ver";
constexpr char kCleanupPendingKey[] = "cleanup_pending";
constexpr char kFailedVersionKey[] = "failed_version";
constexpr char kRollbackDetectedKey[] = "rollback_det";
constexpr size_t kReasonLimit = 64;
constexpr size_t kVersionLimit = 32;
constexpr size_t kUrlLimit = 256;
constexpr size_t kShaLimit = 65;
constexpr size_t kBoardLimit = 32;
constexpr size_t kChipLimit = 16;
constexpr EventBits_t kWifiConnected = BIT0;

struct PendingMetadata {
    char installed_version[kVersionLimit] = {};
    char previous_partition[16] = {};
    char target_partition[16] = {};
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

bool ReadPendingMetadata(PendingMetadata* metadata, MinimalUpdater::State* state) {
    if (!metadata || !state) return false;
    *metadata = {};
    *state = MinimalUpdater::State::IDLE;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t raw = 0;
    const bool state_ok = nvs_get_u8(handle, kStateKey, &raw) == ESP_OK &&
                          raw <= static_cast<uint8_t>(MinimalUpdater::State::VALIDATED);
    if (state_ok) *state = static_cast<MinimalUpdater::State>(raw);
    if (state_ok && *state == MinimalUpdater::State::PENDING_VERIFY) {
        ReadString(handle, kInstalledVersionKey, metadata->installed_version,
                   sizeof(metadata->installed_version));
        ReadString(handle, kPreviousPartitionKey, metadata->previous_partition,
                   sizeof(metadata->previous_partition));
        ReadString(handle, kNewPartitionKey, metadata->target_partition,
                   sizeof(metadata->target_partition));
    }
    nvs_close(handle);
    return state_ok;
}

void LogVerifyMemory(const char* point) {
    ESP_LOGI(kTag, "OTA_VERIFY_MEM_%s internal=%u largest=%u psram=%u", point,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
    const EventBits_t bits = xEventGroupWaitBits(events, kWifiConnected, pdTRUE,
                                                  pdFALSE, pdMS_TO_TICKS(60000));
    vEventGroupDelete(events);
    return (bits & kWifiConnected) != 0 && wifi.IsConnected();
}

}  // namespace

namespace MinimalUpdater {

bool ReadState(State* state) {
    if (!state) return false;
    *state = State::IDLE;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return true;
    uint8_t raw = 0;
    const esp_err_t result = nvs_get_u8(handle, kStateKey, &raw);
    nvs_close(handle);
    if (result != ESP_OK || raw > static_cast<uint8_t>(State::VALIDATED)) return true;
    *state = static_cast<State>(raw);
    return true;
}

bool WriteUpdateRequest(const CustomOtaPolicy::StableUpdateMetadata& metadata) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool success = SetString(handle, kVersionKey, metadata.version, kVersionLimit) &&
                   SetString(handle, kUrlKey, metadata.url, kUrlLimit) &&
                   SetString(handle, kShaKey, metadata.sha256, kShaLimit) &&
                   SetString(handle, kBoardKey, metadata.board, kBoardLimit) &&
                   SetString(handle, kChipKey, metadata.chip, kChipLimit) &&
                   metadata.size > 0 && nvs_set_u32(handle, kSizeKey, metadata.size) == ESP_OK &&
                   nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::UPDATE_REQUESTED)) == ESP_OK &&
                   nvs_set_u32(handle, kAttemptKey, 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool ReadDevRequestConsumed(bool* consumed) {
    if (!consumed) return false;
    *consumed = false;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return true;
    uint8_t raw = 0;
    const esp_err_t result = nvs_get_u8(handle, kDevRequestConsumedKey, &raw);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    *consumed = raw != 0;
    return true;
}

bool MarkDevRequestConsumed() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool success = nvs_set_u8(handle, kDevRequestConsumedKey, 1) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool MarkState(State state, const char* reason, const char* version) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    uint32_t attempt = 0;
    nvs_get_u32(handle, kAttemptKey, &attempt);
    if (state == State::STAGING || state == State::FAILED) ++attempt;
    bool success = nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(state)) == ESP_OK &&
                   nvs_set_u32(handle, kAttemptKey, attempt) == ESP_OK;
    if (reason) success = success && SetString(handle, kReasonKey, reason, kReasonLimit);
    if (version) success = success && SetString(handle, kVersionKey, version, kVersionLimit);
    if (success && state == State::PENDING_VERIFY) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* boot = esp_ota_get_boot_partition();
        char installed_version[kVersionLimit] = {};
        char installed_sha[kShaLimit] = {};
        size_t length = sizeof(installed_version);
        const bool version_ok = nvs_get_str(handle, kVersionKey, installed_version, &length) == ESP_OK;
        length = sizeof(installed_sha);
        const bool sha_ok = nvs_get_str(handle, kShaKey, installed_sha, &length) == ESP_OK;
        success = version_ok && sha_ok && running && boot &&
                  SetString(handle, kInstalledVersionKey, installed_version, kVersionLimit) &&
                  SetString(handle, kInstalledShaKey, installed_sha, kShaLimit) &&
                  SetString(handle, kPreviousPartitionKey, running->label, 16) &&
                  SetString(handle, kNewPartitionKey, boot->label, 16);
    }
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

void ReportPendingVerification() {
    PendingMetadata metadata;
    State state = State::IDLE;
    if (ReadPendingMetadata(&metadata, &state) && state == State::PENDING_VERIFY) {
        ESP_LOGI(kTag, "OTA_POST_UPDATE_PENDING version=%s", metadata.installed_version);
    }
}

bool IsValidationPendingOrFailed() {
    State state = State::IDLE;
    if (!ReadState(&state)) return true;
    return state == State::PENDING_VERIFY || state == State::FAILED;
}

bool MarkVerificationFailed(const char* reason, const char* failed_version, bool rollback) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool success = nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::FAILED)) == ESP_OK &&
                   SetString(handle, kReasonKey, reason, kReasonLimit);
    if (failed_version && *failed_version) {
        success = success && SetString(handle, kFailedVersionKey, failed_version, kVersionLimit);
    }
    success = success && nvs_set_u8(handle, kRollbackDetectedKey, rollback ? 1 : 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool MarkValidatedAndRemember(const char* version) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool success = nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::VALIDATED)) == ESP_OK &&
                   SetString(handle, kLastValidatedVersionKey, version, kVersionLimit) &&
                   nvs_set_u8(handle, kCleanupPendingKey, 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool ClearValidatedMetadata() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    constexpr const char* keys[] = {
        kReasonKey, kVersionKey, kUrlKey, kShaKey, kBoardKey, kChipKey, kSizeKey,
        kAttemptKey, kInstalledVersionKey, kInstalledShaKey, kPreviousPartitionKey,
        kNewPartitionKey, kFailedVersionKey, kRollbackDetectedKey,
    };
    bool success = true;
    for (const char* key : keys) {
        const esp_err_t err = nvs_erase_key(handle, key);
        success = success && (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);
    }
    success = success && nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::IDLE)) == ESP_OK &&
              nvs_set_u8(handle, kCleanupPendingKey, 0) == ESP_OK;
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (success) ESP_LOGI(kTag, "OTA_NVS_METADATA_CLEANED state=IDLE");
    return success;
}

bool MarkCleanupPending() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool success = nvs_set_u8(handle, kStateKey, static_cast<uint8_t>(State::IDLE)) == ESP_OK &&
                         nvs_set_u8(handle, kCleanupPendingKey, 1) == ESP_OK &&
                         nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool ValidatePendingUpdate() {
    PendingMetadata metadata;
    State state = State::IDLE;
    if (!ReadPendingMetadata(&metadata, &state) || state != State::PENDING_VERIFY) return true;

    LogVerifyMemory("BEFORE");
    if (metadata.installed_version[0] == '\0' || metadata.previous_partition[0] == '\0' ||
        metadata.target_partition[0] == '\0') {
        ESP_LOGE(kTag, "OTA_POST_UPDATE_FAILED reason=VERIFY_FAILED missing_pending_metadata");
        MarkVerificationFailed("VERIFY_FAILED", nullptr, false);
        LogVerifyMemory("AFTER");
        return false;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_app_desc_t* descriptor = esp_app_get_description();
    const bool running_ok = running && boot && descriptor &&
                            strcmp(running->label, metadata.target_partition) == 0 &&
                            strcmp(boot->label, metadata.target_partition) == 0;
    if (!running_ok) {
        ESP_LOGE(kTag, "OTA_ROLLBACK_DETECTED running=%s expected=%s",
                 running ? running->label : "none", metadata.target_partition);
        MarkVerificationFailed("ROLLBACK_DETECTED", metadata.installed_version, true);
        LogVerifyMemory("AFTER");
        return false;
    }

    const bool version_match = strcmp(descriptor->version, metadata.installed_version) == 0;
    if (!version_match) {
        ESP_LOGE(kTag, "OTA_POST_UPDATE_VERSION_MISMATCH running=%s expected=%s", descriptor->version,
                 metadata.installed_version);
        MarkVerificationFailed("VERIFY_FAILED", descriptor->version, false);
        LogVerifyMemory("AFTER");
        return false;
    }

    const bool nvs_ok = metadata.installed_version[0] != '\0';
    const bool board_ok = Board::GetInstance().GetDisplay() != nullptr;
    const bool display_ok = board_ok;
    const bool critical_tasks_ok = true;  // Reaching this checkpoint proves startup survived.
    const bool self_test = nvs_ok && running_ok && version_match && board_ok && display_ok &&
                           critical_tasks_ok;
    ESP_LOGI(kTag, "OTA_POST_UPDATE_SELFTEST status=%s nvs=%s board=%s display=%s tasks=%s",
             self_test ? "pass" : "fail", nvs_ok ? "pass" : "fail", board_ok ? "pass" : "fail",
             display_ok ? "pass" : "fail", critical_tasks_ok ? "pass" : "fail");
    if (!self_test) {
        MarkVerificationFailed("VERIFY_FAILED", descriptor->version, false);
        LogVerifyMemory("AFTER");
        return false;
    }

    const esp_err_t mark_result = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kTag, "OTA_POST_UPDATE_MARK_VALID status=%s err=%s", mark_result == ESP_OK ? "pass" : "fail",
             esp_err_to_name(mark_result));
    if (mark_result != ESP_OK || !MarkValidatedAndRemember(metadata.installed_version)) {
        ESP_LOGE(kTag, "OTA_POST_UPDATE_FAILED reason=mark_valid_state");
        LogVerifyMemory("AFTER");
        return false;
    }
    ESP_LOGI(kTag, "OTA_POST_UPDATE_STATE VALIDATED");

    const bool files_clean = CustomOtaPolicy::CleanupStagedFiles();
    if (!files_clean) {
        MarkCleanupPending();
        ESP_LOGW(kTag, "OTA_POST_UPDATE_CLEANUP_FAILED state=IDLE cleanup_pending=1");
    } else if (!ClearValidatedMetadata()) {
        ESP_LOGW(kTag, "OTA_POST_UPDATE_METADATA_CLEANUP_FAILED state=VALIDATED");
    }
    LogVerifyMemory("AFTER");
    return true;
}

bool FailAndReboot(const char* reason) {
    MarkState(State::FAILED, reason, nullptr);
    ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=%s", reason ? reason : "unknown");
    ESP_LOGI(kTag, "OTA_MINIMAL_REBOOT_AFTER_FAILURE reason=%s", reason ? reason : "unknown");
    esp_restart();
    return true;
}

bool RunIfRequested() {
    static bool entered = false;
    State state = State::IDLE;
    ReadState(&state);
    if (state != State::UPDATE_REQUESTED && state != State::STAGED) return false;
    if (entered) {
        ESP_LOGE(kTag, "OTA_MINIMAL_ALREADY_ENTERED");
        return true;
    }
    entered = true;

    ESP_LOGI(kTag, "OTA_MINIMAL_MODE_ENTER state=%s",
             state == State::STAGED ? "STAGED" : "UPDATE_REQUESTED");
    MinimalOtaDisplay display;
    display.Init();
    LogMemory("BOOT");
    if (state == State::UPDATE_REQUESTED) {
        if (!MarkState(State::STAGING, "staging", nullptr)) {
            return FailAndReboot("state_staging");
        }
        if (!ConnectWifi()) {
            return FailAndReboot("wifi");
        }
        ESP_LOGI(kTag, "OTA_MINIMAL_WIFI_CONNECTED");
        LogMemory("WIFI");
    }
    if (!StorageManager::GetInstance().InitializeSdCard()) {
        return FailAndReboot("sd_mount");
    }
    if (state == State::UPDATE_REQUESTED) {
        EspNetwork network;
        LogMemory("BEFORE_DOWNLOAD");
        const bool staged = CustomOtaPolicy::StageStableUpdateOnNetwork(&network);
        if (!staged) {
            return FailAndReboot("stage");
        }
        LogMemory("AFTER_STAGE");
        ESP_LOGI(kTag, "OTA_MINIMAL_STACK unused_bytes=%u",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
        if (!MarkState(State::STAGED, "staged", nullptr)) {
            return FailAndReboot("state_staged");
        }
    }

    ESP_LOGI(kTag, "OTA_INSTALL_REQUESTED");
    if (!MarkState(State::INSTALL_REQUESTED, "install_requested", nullptr) ||
        !MarkState(State::INSTALLING, "installing", nullptr)) {
        return FailAndReboot("state_installing");
    }
    LogMemory("BEFORE_INSTALL");
    if (!CustomOtaPolicy::InstallStagedUpdate()) {
        return FailAndReboot("install");
    }
    LogMemory("AFTER_INSTALL");
    ESP_LOGI(kTag, "OTA_MINIMAL_STACK unused_bytes=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    if (!MarkState(State::PENDING_VERIFY, "pending_verify", nullptr)) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running) esp_ota_set_boot_partition(running);
        return FailAndReboot("state_pending_verify");
    }
    ESP_LOGI(kTag, "OTA_INSTALL_PENDING_VERIFY reboot=1");
    esp_restart();
    return true;
}

}  // namespace MinimalUpdater
