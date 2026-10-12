#include "partition_switcher.h"

#include "minimal_updater.h"

#include <esp_app_desc.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_partition.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {
constexpr char kTag[] = "OtaPartition";
std::mutex g_switch_mutex;
std::atomic<bool> g_switch_busy{false};

void SetReason(OtaPartitionSwitcher::PartitionInfo* info, const char* reason) {
    if (!info) return;
    std::strncpy(info->reason, reason ? reason : "unknown", sizeof(info->reason) - 1);
    info->reason[sizeof(info->reason) - 1] = '\0';
}

const esp_partition_t* FindSlot(int index) {
    const auto subtype = index == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                    : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

bool VerifyImage(const esp_partition_t* partition) {
    if (!partition) return false;
    esp_partition_pos_t position = {
        .offset = partition->address,
        .size = partition->size,
    };
    esp_image_metadata_t metadata = {};
    return esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata) == ESP_OK;
}

bool IsSelectableState(esp_ota_img_states_t state, esp_err_t state_result) {
    if (state_result != ESP_OK) {
        // Older IDF targets may not expose the per-partition state.  Only
        // treat the documented absence of that metadata as non-blocking;
        // other read errors must keep the slot disabled.
        return state_result == ESP_ERR_NOT_FOUND || state_result == ESP_ERR_NOT_SUPPORTED;
    }
    // With rollback disabled ESP-IDF reports UNDEFINED for a normal image.
    // The complete image verification above is the authority in that case.
    return state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_NEW ||
           state == ESP_OTA_IMG_UNDEFINED;
}

void InspectSlot(int index, const esp_partition_t* running, const esp_partition_t* boot,
                 OtaPartitionSwitcher::PartitionInfo* info) {
    *info = {};
    info->index = index;
    info->state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* partition = FindSlot(index);
    if (!partition) {
        SetReason(info, "partition_missing");
        return;
    }
    info->present = true;
    std::strncpy(info->label, partition->label, sizeof(info->label) - 1);
    info->running = running && running->address == partition->address;
    info->boot = boot && boot->address == partition->address;

    esp_app_desc_t description = {};
    if (esp_ota_get_partition_description(partition, &description) == ESP_OK) {
        std::strncpy(info->version, description.version, sizeof(info->version) - 1);
    } else {
        SetReason(info, "app_descriptor_missing");
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t state_result = esp_ota_get_state_partition(partition, &state);
    if (state_result == ESP_OK) info->state = state;
    info->image_valid = VerifyImage(partition);
    if (!info->image_valid) {
        SetReason(info, "image_integrity_failed");
        return;
    }
    if (info->running) {
        SetReason(info, "running");
        return;
    }
    if (info->boot) {
        SetReason(info, "already_boot");
        return;
    }
    if (!IsSelectableState(state, state_result)) {
        if (state == ESP_OTA_IMG_INVALID) SetReason(info, "state_invalid");
        else if (state == ESP_OTA_IMG_ABORTED) SetReason(info, "state_aborted");
        else if (state == ESP_OTA_IMG_PENDING_VERIFY) SetReason(info, "state_pending_verify");
        else SetReason(info, "state_not_selectable");
        return;
    }
    info->selectable = true;
    SetReason(info, state_result == ESP_OK ? "ready" : "ready_state_unavailable");
}

bool BuildSnapshot(OtaPartitionSwitcher::Snapshot* snapshot) {
    if (!snapshot) return false;
    *snapshot = {};
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    snapshot->rollback_enabled = true;
#endif
    MinimalUpdater::State updater_state = MinimalUpdater::State::STABLE;
    MinimalUpdater::ReadState(&updater_state);
    snapshot->custom_update_pending = updater_state != MinimalUpdater::State::STABLE;
    if (snapshot->custom_update_pending) {
        std::strncpy(snapshot->global_reason, "custom_update_pending",
                     sizeof(snapshot->global_reason) - 1);
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    InspectSlot(0, running, boot, &snapshot->slots[0]);
    InspectSlot(1, running, boot, &snapshot->slots[1]);
    if (snapshot->custom_update_pending) {
        for (auto& slot : snapshot->slots) {
            if (slot.selectable) {
                slot.selectable = false;
                SetReason(&slot, snapshot->global_reason);
            }
        }
    }
    ESP_LOGI(kTag, "OTA_PARTITIONS running=%s boot=%s ota0=%s/%s ota1=%s/%s rollback=%d",
             running ? running->label : "none", boot ? boot->label : "none",
             snapshot->slots[0].version[0] ? snapshot->slots[0].version : "empty",
             snapshot->slots[0].reason, snapshot->slots[1].version[0] ? snapshot->slots[1].version : "empty",
             snapshot->slots[1].reason, snapshot->rollback_enabled ? 1 : 0);
    return true;
}
}  // namespace

namespace OtaPartitionSwitcher {

bool GetSnapshot(Snapshot* snapshot) {
    std::lock_guard<std::mutex> lock(g_switch_mutex);
    return BuildSnapshot(snapshot);
}

bool SwitchTo(int index, std::string& error) {
    error.clear();
    if (index < 0 || index > 1) {
        error = "invalid_slot";
        return false;
    }
    bool expected = false;
    if (!g_switch_busy.compare_exchange_strong(expected, true)) {
        error = "switch_in_progress";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_switch_mutex);
    Snapshot snapshot;
    const bool inspected = BuildSnapshot(&snapshot);
    if (!inspected || snapshot.custom_update_pending || !snapshot.slots[index].selectable) {
        error = !inspected ? "partition_inspection_failed"
                           : (snapshot.custom_update_pending ? snapshot.global_reason
                                                             : snapshot.slots[index].reason);
        g_switch_busy.store(false);
        return false;
    }
    const esp_partition_t* target = FindSlot(index);
    if (!target) {
        error = "partition_missing";
        g_switch_busy.store(false);
        return false;
    }
    const esp_err_t result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        error = esp_err_to_name(result);
        ESP_LOGE(kTag, "OTA_PARTITION_SWITCH_FAILED slot=%d error=%s", index, error.c_str());
        g_switch_busy.store(false);
        return false;
    }
    const esp_partition_t* boot_after = esp_ota_get_boot_partition();
    if (!boot_after || boot_after->address != target->address) {
        error = "boot_partition_verification_failed";
        g_switch_busy.store(false);
        return false;
    }
    ESP_LOGI(kTag, "OTA_PARTITION_SWITCH_READY slot=%d target=%s version=%s reboot=required", index,
             target->label, snapshot.slots[index].version);
    g_switch_busy.store(false);
    return true;
}

}  // namespace OtaPartitionSwitcher
