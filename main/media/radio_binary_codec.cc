#include "radio_binary_codec.h"

#include <array>
#include <cstring>

namespace RadioBinaryCodec {
namespace {
constexpr size_t kHeaderSize = 16;
constexpr std::array<uint16_t, 9> kLimits = {64, 256, 512, 32, 128, 16, 128, 64, 512};
constexpr char kCatalogMagic[] = "RDBC";
constexpr char kFavoritesMagic[] = "RFAV";

void Put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}
void Put32(std::string& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (i * 8)));
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
bool Header(const char* magic, uint32_t count, std::string& out) {
    if (count > kMaxRecords) return false;
    out.assign(magic, 4);
    Put16(out, 1); Put16(out, 0); Put32(out, count); Put32(out, 0);
    return true;
}
bool ParseHeader(const char* magic, const uint8_t* data, size_t size, uint32_t& count) {
    if (data == nullptr || size < kHeaderSize || std::memcmp(data, magic, 4) != 0) return false;
    size_t pos = 4; uint16_t version, flags; uint32_t reserved;
    if (!Get16(data, size, pos, version) || !Get16(data, size, pos, flags) ||
        !Get32(data, size, pos, count) || !Get32(data, size, pos, reserved)) return false;
    return version == 1 && flags == 0 && reserved == 0 && count <= kMaxRecords;
}
bool PutString(std::string& out, const std::string& value, uint16_t limit) {
    if (value.size() > limit) return false;
    Put16(out, static_cast<uint16_t>(value.size())); out.append(value); return true;
}
bool GetString(const uint8_t* data, size_t size, size_t& pos, uint16_t limit, std::string& value) {
    uint16_t length;
    if (!Get16(data, size, pos, length) || length > limit || pos > size || size - pos < length) return false;
    value.assign(reinterpret_cast<const char*>(data + pos), length); pos += length; return true;
}
}  // namespace

bool EncodeCatalogHeader(uint32_t count, std::string& out) { return Header(kCatalogMagic, count, out); }
bool DecodeCatalogHeader(const uint8_t* data, size_t size, uint32_t& count) { return ParseHeader(kCatalogMagic, data, size, count); }
bool EncodeFavoritesHeader(uint32_t count, std::string& out) { return Header(kFavoritesMagic, count, out); }
bool DecodeFavoritesHeader(const uint8_t* data, size_t size, uint32_t& count) { return ParseHeader(kFavoritesMagic, data, size, count); }

bool EncodeCatalogRecord(const RadioStationInfo& station, std::string& out) {
    std::string payload;
    const std::array<const std::string*, 4> first = {&station.stationuuid, &station.name, &station.url_resolved, &station.codec};
    for (size_t i = 0; i < first.size(); ++i) if (!PutString(payload, *first[i], kLimits[i])) return false;
    Put32(payload, station.bitrate);
    const std::array<const std::string*, 5> last = {&station.country, &station.countrycode, &station.state, &station.language, &station.tags};
    for (size_t i = 0; i < last.size(); ++i) if (!PutString(payload, *last[i], kLimits[i + 4])) return false;
    if (payload.size() > kMaxRecordLength - 4) return false;
    out.clear(); Put32(out, static_cast<uint32_t>(payload.size() + 4)); out += payload; return true;
}

bool DecodeCatalogRecord(const uint8_t* data, size_t size, RadioStationInfo& station) {
    if (data == nullptr || size < 4 || size > kMaxRecordLength) return false;
    size_t pos = 0; uint32_t total;
    if (!Get32(data, size, pos, total) || total != size || total < 4) return false;
    const std::array<std::string*, 4> first = {&station.stationuuid, &station.name, &station.url_resolved, &station.codec};
    for (size_t i = 0; i < first.size(); ++i) if (!GetString(data, size, pos, kLimits[i], *first[i])) return false;
    if (!Get32(data, size, pos, station.bitrate)) return false;
    const std::array<std::string*, 5> last = {&station.country, &station.countrycode, &station.state, &station.language, &station.tags};
    for (size_t i = 0; i < last.size(); ++i) if (!GetString(data, size, pos, kLimits[i + 4], *last[i])) return false;
    return pos == size;
}

bool EncodeFavoriteUuid(const std::string& uuid, std::string& out) {
    if (uuid.empty() || uuid.size() > kMaxUuidLength) return false;
    out.clear(); Put16(out, static_cast<uint16_t>(uuid.size())); out += uuid; return true;
}
bool DecodeFavoriteUuid(const uint8_t* data, size_t size, std::string& uuid) {
    size_t pos = 0; uint16_t length;
    if (data == nullptr || !Get16(data, size, pos, length) || length == 0 || length > kMaxUuidLength || size - pos != length) return false;
    uuid.assign(reinterpret_cast<const char*>(data + pos), length); return true;
}
}  // namespace RadioBinaryCodec
