#include "resource_manager.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

#define TAG "ResourceManager"

void ResourceManager::PrintSystemMemoryInfo() {
    size_t free_internal = GetFreeInternalHeapSize();
    size_t free_psram = GetFreePsramSize();
    ESP_LOGI(TAG, "Memory Info -> Internal Free: %u KB, PSRAM Free: %u KB",
             static_cast<unsigned int>(free_internal / 1024),
             static_cast<unsigned int>(free_psram / 1024));
}

size_t ResourceManager::GetFreePsramSize() const {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t ResourceManager::GetFreeInternalHeapSize() const {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
