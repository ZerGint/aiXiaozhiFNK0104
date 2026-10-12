#include "storage_manager.h"
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <string>
#include <unistd.h>

#define TAG "StorageManager"
#define NVS_NAMESPACE "sys_config"

#ifndef SDMMC_CLK_PIN
#define SDMMC_CLK_PIN GPIO_NUM_38
#endif
#ifndef SDMMC_CMD_PIN
#define SDMMC_CMD_PIN GPIO_NUM_40
#endif
#ifndef SDMMC_D0_PIN
#define SDMMC_D0_PIN GPIO_NUM_39
#endif

bool StorageManager::InitializeSdCard() {
    if (sd_mounted_) return true;
    ESP_LOGI(TAG, "Initializing SD Card (SDMMC 1-bit mode: CLK=%d, CMD=%d, D0=%d)...",
             (int)SDMMC_CLK_PIN, (int)SDMMC_CMD_PIN, (int)SDMMC_D0_PIN);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SDMMC_CLK_PIN;
    slot_config.cmd = SDMMC_CMD_PIN;
    slot_config.d0 = SDMMC_D0_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK) {
        sd_mounted_ = true;
        ESP_LOGI(TAG, "SD Card mounted successfully at /sdcard!");
        if (!EnsureRequiredDirectories()) {
            ESP_LOGW(TAG, "SD_REQUIRED_DIRECTORIES incomplete; continuing with safe degradation");
        }
    } else {
        ESP_LOGE(TAG, "Failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        sd_mounted_ = false;
    }
    return sd_mounted_;
}

bool StorageManager::EnsureDirectory(const char* path) {
    if (!sd_mounted_ || path == nullptr || path[0] != '/') return false;
    struct stat existing = {};
    if (stat(path, &existing) == 0) {
        if (!S_ISDIR(existing.st_mode)) {
            ESP_LOGE(TAG, "SD_PATH_NOT_DIRECTORY path=%s", path);
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        ESP_LOGE(TAG, "SD_PATH_STAT_FAILED path=%s errno=%d", path, errno);
        return false;
    }

    std::string current;
    const std::string requested(path);
    for (size_t i = 0; i < requested.size(); ++i) {
        if (requested[i] != '/' && (i == 0 || requested[i - 1] == '/')) {
            current.push_back('/');
        }
        if (requested[i] != '/') current.push_back(requested[i]);
        if (i + 1 == requested.size() || requested[i + 1] == '/') {
            if (current.empty() || current == "/") continue;
            struct stat component = {};
            if (stat(current.c_str(), &component) == 0) {
                if (!S_ISDIR(component.st_mode)) {
                    ESP_LOGE(TAG, "SD_PATH_NOT_DIRECTORY path=%s", current.c_str());
                    return false;
                }
                continue;
            }
            if (errno != ENOENT ||
                (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST)) {
                ESP_LOGE(TAG, "SD_DIRECTORY_CREATE_FAILED path=%s errno=%d", current.c_str(), errno);
                return false;
            }
            ESP_LOGI(TAG, "SD_DIRECTORY_READY path=%s", current.c_str());
        }
    }
    return true;
}

bool StorageManager::EnsureRequiredDirectories() {
    if (!sd_mounted_) return false;
    static constexpr const char* kDirectories[] = {
        "/sdcard/music", "/sdcard/generated_music", "/sdcard/update"};
    bool success = true;
    for (const char* path : kDirectories) success = EnsureDirectory(path) && success;
    ESP_LOGI(TAG, "SD_REQUIRED_DIRECTORIES result=%s count=%u", success ? "ready" : "partial",
             static_cast<unsigned>(sizeof(kDirectories) / sizeof(kDirectories[0])));
    return success;
}

std::vector<std::string> StorageManager::ListDirectory(const std::string& path, const std::string& extension) {
    std::vector<std::string> result;
    if (!sd_mounted_) return result;

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        if (errno != ENOENT) ESP_LOGW(TAG, "Failed to open directory: %s errno=%d", path.c_str(), errno);
        return result;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (extension.empty()) {
            result.push_back(name);
        } else {
            if (name.length() >= extension.length()) {
                std::string ext = name.substr(name.length() - extension.length());
                std::string target_ext = extension;
                for (auto& c : ext) c = tolower((unsigned char)c);
                for (auto& c : target_ext) c = tolower((unsigned char)c);
                if (ext == target_ext) {
                    result.push_back(name);
                }
            }
        }
    }
    closedir(dir);
    return result;
}

bool StorageManager::SaveSettingString(const char* key, const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    nvs_commit(handle);
    nvs_close(handle);
    return (err == ESP_OK);
}

std::string StorageManager::GetSettingString(const char* key, const std::string& default_val) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return default_val;
    size_t required_size = 0;
    if (nvs_get_str(handle, key, nullptr, &required_size) != ESP_OK || required_size == 0) {
        nvs_close(handle);
        return default_val;
    }
    std::vector<char> buf(required_size);
    nvs_get_str(handle, key, buf.data(), &required_size);
    nvs_close(handle);
    return std::string(buf.data());
}

bool StorageManager::SaveSettingInt(const char* key, int32_t value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_i32(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
    return (err == ESP_OK);
}

int32_t StorageManager::GetSettingInt(const char* key, int32_t default_val) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return default_val;
    int32_t val = default_val;
    nvs_get_i32(handle, key, &val);
    nvs_close(handle);
    return val;
}
