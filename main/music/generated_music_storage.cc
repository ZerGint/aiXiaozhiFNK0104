#include "generated_music_storage.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr char kMagic[] = "GMDB";
constexpr uint16_t kVersion = 2;
constexpr size_t kHeaderSize = 16;

void Put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}

void Put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (8 * i)));
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

bool PutString(std::string& out, const std::string& value, size_t limit) {
    if (value.size() > limit || value.size() > UINT16_MAX) return false;
    Put16(out, static_cast<uint16_t>(value.size()));
    out.append(value);
    return true;
}

bool GetString(const uint8_t* data, size_t size, size_t& pos, size_t limit,
               std::string& value) {
    uint16_t length = 0;
    if (!Get16(data, size, pos, length) || length > limit || pos > size || size - pos < length)
        return false;
    value.assign(reinterpret_cast<const char*>(data + pos), length);
    pos += length;
    return true;
}

bool EncodeHeader(uint32_t count, std::string& out) {
    if (count > GeneratedMusicStorage::kMaxRecords) return false;
    out.assign(kMagic, 4);
    Put16(out, kVersion);
    Put16(out, 0);
    Put32(out, count);
    Put32(out, 0);
    return true;
}

bool DecodeHeader(const uint8_t* data, size_t size, uint32_t& count) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, kMagic, 4) != 0) return false;
    size_t pos = 4;
    uint16_t version = 0;
    uint16_t flags = 0;
    uint32_t reserved = 0;
    return Get16(data, size, pos, version) && Get16(data, size, pos, flags) &&
           Get32(data, size, pos, count) && Get32(data, size, pos, reserved) &&
           version == kVersion && flags == 0 && reserved == 0 &&
           count <= GeneratedMusicStorage::kMaxRecords;
}

bool EncodeRecord(const GeneratedMusicRecord& record, std::string& out) {
    std::string payload;
    if (!PutString(payload, record.filename, GeneratedMusicStorage::kMaxFilenameLength)) return false;
    if (payload.size() + 4 > GeneratedMusicStorage::kMaxRecordLength) return false;
    out.clear();
    Put32(out, static_cast<uint32_t>(payload.size() + 4));
    out += payload;
    return true;
}

bool DecodeRecord(const uint8_t* data, size_t size, GeneratedMusicRecord& record) {
    if (data == nullptr || size < 4 || size > GeneratedMusicStorage::kMaxRecordLength) return false;
    size_t pos = 0;
    uint32_t total = 0;
    if (!Get32(data, size, pos, total) || total != size) return false;
    return GetString(data, size, pos, GeneratedMusicStorage::kMaxFilenameLength, record.filename) &&
           !record.filename.empty() && pos == size;
}

bool Exact(FILE* file, void* data, size_t size) {
    return size == 0 || fread(data, 1, size, file) == size;
}

bool Write(FILE* file, const std::string& data) {
    return data.empty() || fwrite(data.data(), 1, data.size(), file) == data.size();
}

bool ReadRecord(FILE* file, std::string& encoded) {
    uint8_t length_bytes[4] = {};
    if (!Exact(file, length_bytes, sizeof(length_bytes))) return false;
    const uint32_t length = static_cast<uint32_t>(length_bytes[0]) |
                            (static_cast<uint32_t>(length_bytes[1]) << 8) |
                            (static_cast<uint32_t>(length_bytes[2]) << 16) |
                            (static_cast<uint32_t>(length_bytes[3]) << 24);
    if (length < 4 || length > GeneratedMusicStorage::kMaxRecordLength) return false;
    encoded.assign(length, '\0');
    std::memcpy(encoded.data(), length_bytes, sizeof(length_bytes));
    return Exact(file, encoded.data() + 4, length - 4);
}

bool AtEnd(FILE* file) {
    uint8_t byte = 0;
    return fread(&byte, 1, 1, file) == 0;
}
}  // namespace

bool GeneratedMusicStorage::Load(const char* path, std::vector<GeneratedMusicRecord>& records,
                                 std::string& error) {
    records.clear();
    error.clear();
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        if (errno == ENOENT) return true;
        error = "open_generated_index_failed";
        return false;
    }
    uint8_t header[kHeaderSize] = {};
    uint32_t count = 0;
    if (!Exact(file, header, sizeof(header)) || !DecodeHeader(header, sizeof(header), count)) {
        fclose(file);
        error = "corrupt_generated_index_header";
        return false;
    }
    records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string encoded;
        GeneratedMusicRecord record;
        if (!ReadRecord(file, encoded) ||
            !DecodeRecord(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), record) ||
            record.filename.empty()) {
            fclose(file);
            records.clear();
            error = "corrupt_generated_index_record";
            return false;
        }
        records.push_back(std::move(record));
    }
    if (!AtEnd(file) || fclose(file) != 0) {
        records.clear();
        error = "corrupt_generated_index_tail";
        return false;
    }
    return true;
}

bool GeneratedMusicStorage::SaveAtomic(const char* target_path, const char* temp_path,
                                       const std::vector<GeneratedMusicRecord>& records,
                                       std::string& error) {
    error.clear();
    if (records.size() > kMaxRecords) {
        error = "generated_index_capacity_exceeded";
        return false;
    }
    FILE* file = fopen(temp_path, "wb");
    if (file == nullptr) {
        error = "open_generated_index_temp_failed";
        return false;
    }
    std::string encoded;
    if (!EncodeHeader(static_cast<uint32_t>(records.size()), encoded) || !Write(file, encoded)) {
        fclose(file);
        unlink(temp_path);
        error = "write_generated_index_header_failed";
        return false;
    }
    for (const auto& record : records) {
        if (!EncodeRecord(record, encoded) || !Write(file, encoded)) {
            fclose(file);
            unlink(temp_path);
            error = "write_generated_index_record_failed";
            return false;
        }
    }
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!flushed || !closed) {
        unlink(temp_path);
        error = "close_generated_index_failed";
        return false;
    }
    if (unlink(target_path) != 0 && errno != ENOENT) {
        unlink(temp_path);
        error = "remove_generated_index_failed";
        return false;
    }
    if (rename(temp_path, target_path) != 0) {
        unlink(temp_path);
        error = "rename_generated_index_failed";
        return false;
    }
    return true;
}
