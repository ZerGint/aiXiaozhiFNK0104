#pragma once

#include <cstdint>
#include <string>

struct GeneratedMusicJobRecord {
    std::string job_id;
    std::string title;
    std::string status;
    std::string filename;
    std::string download_status;
    std::string local_filename;
    std::string error;
    uint64_t size = 0;
    uint32_t duration_seconds = 0;
    uint64_t downloaded_size = 0;
    uint32_t download_attempts = 0;
    int64_t next_download_at = 0;
    bool library_source = false;
    bool user_library_source = false;
};

class GeneratedMusicJobStorage {
public:
    static constexpr uint16_t kVersion = 2;
    static constexpr size_t kMaxStringLength = 256;

    // A missing file is a valid empty state and returns exists=false.
    static bool Load(const char* path, GeneratedMusicJobRecord& record, bool& exists,
                     std::string& error);
    static bool SaveAtomic(const char* target_path, const char* temp_path,
                           const GeneratedMusicJobRecord& record, std::string& error);
};
