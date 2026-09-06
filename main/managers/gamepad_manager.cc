#include "gamepad_manager.h"
#include <esp_log.h>

#define TAG "GamepadManager"

void GamepadManager::StartScanAndConnect() {
    ESP_LOGI(TAG, "Starting Bluetooth LE scan for Xbox Controller...");
    // Future BLE HID Host implementation
}

void GamepadManager::StopScan() {
    ESP_LOGI(TAG, "Stopping Bluetooth LE scan.");
}
