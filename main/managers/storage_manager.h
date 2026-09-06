#ifndef _STORAGE_MANAGER_H_
#define _STORAGE_MANAGER_H_

#include <string>
#include <vector>
#include <cstdint>

class StorageManager {
public:
    static StorageManager& GetInstance() {
        static StorageManager instance;
        return instance;
    }

    bool InitializeSdCard();
    bool IsSdCardMounted() const { return sd_mounted_; }

    std::vector<std::string> ListDirectory(const std::string& path, const std::string& extension = "");

    bool SaveSettingString(const char* key, const std::string& value);
    std::string GetSettingString(const char* key, const std::string& default_val = "");

    bool SaveSettingInt(const char* key, int32_t value);
    int32_t GetSettingInt(const char* key, int32_t default_val = 0);

private:
    StorageManager() = default;
    bool sd_mounted_ = false;
};

#endif // _STORAGE_MANAGER_H_
