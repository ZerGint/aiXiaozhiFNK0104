#include "app_manager.h"
#include <esp_log.h>

#define TAG "AppManager"

void AppManager::Initialize() {
    ESP_LOGI(TAG, "Initializing Application Manager...");
    // Future launcher / app registration
}

bool AppManager::SwitchToApp(AppId id) {
    if (current_app_id_ == id) return true;

    ESP_LOGI(TAG, "Switching App from %d to %d", static_cast<int>(current_app_id_), static_cast<int>(id));
    if (current_app_) {
        current_app_->OnStop();
        current_app_.reset();
    }

    current_app_id_ = id;
    return true;
}

void AppManager::DispatchTouchEvent(int x, int y, bool pressed) {
    if (current_app_) {
        current_app_->OnTouchEvent(x, y, pressed);
    }
}

void AppManager::DispatchEvent(const SystemEvent& event) {
    if (current_app_) {
        current_app_->OnEvent(event);
    }
}

void AppManager::OnUpdate() {
    if (current_app_) {
        current_app_->OnUpdate();
    }
}
