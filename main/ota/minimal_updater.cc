#include "minimal_updater.h"

#include "custom_ota_policy.h"
#include "managers/storage_manager.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
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
constexpr size_t kReasonLimit = 64;
constexpr size_t kVersionLimit = 32;
constexpr size_t kUrlLimit = 256;
constexpr size_t kShaLimit = 65;
constexpr size_t kBoardLimit = 32;
constexpr size_t kChipLimit = 16;
constexpr EventBits_t kWifiConnected = BIT0;

void LogMemory(const char* point) {
    ESP_LOGI(kTag, "OTA_MINIMAL_MEM_%s internal=%u largest=%u psram=%u", point,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

bool SetString(nvs_handle_t handle, const char* key, const char* value, size_t limit) {
    return value && strnlen(value, limit + 1) <= limit && nvs_set_str(handle, key, value) == ESP_OK;
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
    if (result != ESP_OK || raw > static_cast<uint8_t>(State::FAILED)) return true;
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
    if (success) success = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return success;
}

bool RunIfRequested() {
    State state = State::IDLE;
    ReadState(&state);
    if (state != State::UPDATE_REQUESTED) return false;

    ESP_LOGI(kTag, "OTA_MINIMAL_MODE_ENTER state=UPDATE_REQUESTED");
    LogMemory("BOOT");
    if (!MarkState(State::STAGING, "staging", nullptr)) {
        ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=state_staging");
        return false;
    }
    if (!ConnectWifi()) {
        MarkState(State::FAILED, "wifi", nullptr);
        ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=wifi");
        return false;
    }
    ESP_LOGI(kTag, "OTA_MINIMAL_WIFI_CONNECTED");
    LogMemory("WIFI");
    if (!StorageManager::GetInstance().InitializeSdCard()) {
        MarkState(State::FAILED, "sd_mount", nullptr);
        ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=sd_mount");
        return false;
    }
    EspNetwork network;
    LogMemory("BEFORE_DOWNLOAD");
    const bool staged = CustomOtaPolicy::StageStableUpdateOnNetwork(&network);
    if (!staged) {
        MarkState(State::FAILED, "stage", nullptr);
        ESP_LOGE(kTag, "OTA_MINIMAL_FAILED reason=stage");
        return false;
    }
    LogMemory("AFTER_STAGE");
    ESP_LOGI(kTag, "OTA_MINIMAL_STACK unused_bytes=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    MarkState(State::STAGED, "staged", nullptr);
    ESP_LOGI(kTag, "OTA_MINIMAL_STAGED stop=1");
    return true;
}

}  // namespace MinimalUpdater
