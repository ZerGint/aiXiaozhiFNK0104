#include "generated_music_job_storage.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <unistd.h>

namespace {

constexpr char kMagic[] = "GMJB";
constexpr size_t kHeaderSize = 16;
constexpr size_t kMaxFileSize = 2048;

void Put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}

void Put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (8 * i)));
}

void Put64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(value >> (8 * i)));
}

bool Get16(const uint8_t* data, size_t size, size_t& pos, uint16_t& value) {
    if (pos > size || size - pos < 2) return false;
    value = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
    return true;
}

bool Get32(const uint8_t* data, size_t size, size_t& pos, uint32_t& value) {
    if (pos > size || size - pos < 4) return false;
    value = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
            (static_cast<uint32_t>(data[pos + 2]) << 16) |
            (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool Get64(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    if (pos > size || size - pos < 8) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
    pos += 8;
    return true;
}

bool PutString(std::string& out, const std::string& value) {
    if (value.size() > GeneratedMusicJobStorage::kMaxStringLength || value.size() > UINT16_MAX) return false;
    Put16(out, static_cast<uint16_t>(value.size()));
    out.append(value);
    return true;
}

bool GetString(const uint8_t* data, size_t size, size_t& pos, std::string& value) {
    uint16_t length = 0;
    if (!Get16(data, size, pos, length) || length > GeneratedMusicJobStorage::kMaxStringLength ||
        pos > size || size - pos < length) return false;
    value.assign(reinterpret_cast<const char*>(data + pos), length);
    pos += length;
    return true;
}

bool Write(FILE* file, const std::string& data) {
    return data.empty() || fwrite(data.data(), 1, data.size(), file) == data.size();
}

bool Encode(const GeneratedMusicJobRecord& record, std::string& out) {
    std::string payload;
    if (!PutString(payload, record.job_id) || !PutString(payload, record.title) ||
        !PutString(payload, record.status) || !PutString(payload, record.filename) ||
        !PutString(payload, record.download_status) || !PutString(payload, record.local_filename) ||
        !PutString(payload, record.error)) return false;
    Put64(payload, record.size);
    Put32(payload, record.duration_seconds);
    Put64(payload, record.downloaded_size);
    Put32(payload, record.download_attempts);
    Put64(payload, static_cast<uint64_t>(record.next_download_at));
    payload.push_back(record.library_source ? 1 : 0);
    payload.push_back(record.user_library_source ? 1 : 0);
    if (payload.size() > kMaxFileSize - kHeaderSize) return false;
    out.assign(kMagic, 4);
    Put16(out, GeneratedMusicJobStorage::kVersion);
    Put16(out, 0);
    Put32(out, static_cast<uint32_t>(payload.size()));
    Put32(out, 0);
    out += payload;
    return true;
}

bool Decode(const uint8_t* data, size_t size, GeneratedMusicJobRecord& record) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, kMagic, 4) != 0) return false;
    size_t pos = 4;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
    if (!Get16(data, size, pos, version) || !Get16(data, size, pos, flags) ||
        !Get32(data, size, pos, payload_size) || !Get32(data, size, pos, reserved) ||
        (version != 1 && version != GeneratedMusicJobStorage::kVersion) || flags != 0 || reserved != 0 ||
        payload_size != size - kHeaderSize) return false;
    if (!GetString(data, size, pos, record.job_id) || !GetString(data, size, pos, record.title) ||
        !GetString(data, size, pos, record.status) || !GetString(data, size, pos, record.filename) ||
        !GetString(data, size, pos, record.download_status) ||
        !GetString(data, size, pos, record.local_filename) || !GetString(data, size, pos, record.error) ||
        !Get64(data, size, pos, record.size)) return false;
    uint32_t duration = 0;
    if (!Get32(data, size, pos, duration) || !Get64(data, size, pos, record.downloaded_size) ||
        !Get32(data, size, pos, record.download_attempts)) return false;
    uint64_t next_download = 0;
    if (!Get64(data, size, pos, next_download)) return false;
    if (pos < size) {
        const size_t remaining = size - pos;
        if (version == 1) {
            if (remaining != 1 || data[pos] > 1) return false;
            record.user_library_source = data[pos++] != 0;
        } else {
            if (remaining != 2 || data[pos] > 1 || data[pos + 1] > 1) return false;
            record.library_source = data[pos++] != 0;
            record.user_library_source = data[pos++] != 0;
        }
    }
    if (pos != size) return false;
    record.duration_seconds = duration;
    record.next_download_at = static_cast<int64_t>(next_download);
    return !record.job_id.empty() && !record.status.empty();
}

}  // namespace

bool GeneratedMusicJobStorage::Load(const char* path, GeneratedMusicJobRecord& record, bool& exists,
                                    std::string& error) {
    record = {};
    exists = false;
    error.clear();
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        if (errno == ENOENT) return true;
        error = "open_jobs_failed";
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        error = "seek_jobs_failed";
        return false;
    }
    const long length = ftell(file);
    if (length < 0 || static_cast<size_t>(length) > kMaxFileSize || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        error = "invalid_jobs_size";
        return false;
    }
    std::string encoded(static_cast<size_t>(length), '\0');
    if (!encoded.empty() && fread(encoded.data(), 1, encoded.size(), file) != encoded.size()) {
        fclose(file);
        error = "read_jobs_failed";
        return false;
    }
    const bool closed = fclose(file) == 0;
    if (!closed || !Decode(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), record)) {
        error = "corrupt_jobs";
        return false;
    }
    exists = true;
    return true;
}

bool GeneratedMusicJobStorage::SaveAtomic(const char* target_path, const char* temp_path,
                                          const GeneratedMusicJobRecord& record, std::string& error) {
    error.clear();
    std::string encoded;
    if (!Encode(record, encoded)) {
        error = "jobs_record_too_large";
        return false;
    }
    FILE* file = fopen(temp_path, "wb");
    if (file == nullptr) {
        error = "open_jobs_temp_failed";
        return false;
    }
    const bool written = Write(file, encoded);
    const bool flushed = written && fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !closed) {
        unlink(temp_path);
        error = "write_jobs_failed";
        return false;
    }
    if (rename(temp_path, target_path) != 0) {
        if (unlink(target_path) != 0 && errno != ENOENT) {
            unlink(temp_path);
            error = "replace_jobs_failed";
            return false;
        }
        if (rename(temp_path, target_path) != 0) {
            unlink(temp_path);
            error = "rename_jobs_failed";
            return false;
        }
    }
    return true;
}
