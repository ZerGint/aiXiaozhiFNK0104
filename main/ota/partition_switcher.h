#pragma once

#include <esp_ota_ops.h>

#include <cstdint>
#include <string>

namespace OtaPartitionSwitcher {

struct PartitionInfo {
    int index = -1;
    bool present = false;
    bool image_valid = false;
    bool selectable = false;
    bool running = false;
    bool boot = false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    char label[16] = {};
    char version[33] = {};
    char reason[64] = {};
};

struct Snapshot {
    PartitionInfo slots[2];
    bool rollback_enabled = false;
    bool custom_update_pending = false;
    char global_reason[64] = {};
};

// Reads both fixed OTA slots, verifies the complete flash image and reports
// why a slot cannot be selected. This function does not change boot metadata.
bool GetSnapshot(Snapshot* snapshot);

// Revalidates the selected slot, atomically changes the ESP-IDF boot target,
// and returns without rebooting. The caller must reboot only after true.
bool SwitchTo(int index, std::string& error);

}  // namespace OtaPartitionSwitcher
