#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GeneratedMusicRecord {
    std::string filename;
};

class GeneratedMusicStorage {
public:
    static constexpr uint32_t kMaxRecords = 50;
    static constexpr uint32_t kMaxRecordLength = 166;
    static constexpr size_t kMaxFilenameLength = 160;

    static bool Load(const char* path, std::vector<GeneratedMusicRecord>& records,
                     std::string& error);
    static bool SaveAtomic(const char* target_path, const char* temp_path,
                           const std::vector<GeneratedMusicRecord>& records,
                           std::string& error);
};
