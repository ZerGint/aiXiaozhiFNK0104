#pragma once

#include "internet_radio_player.h"

#include <cstdint>
#include <string>

namespace RadioBinaryCodec {
constexpr uint32_t kMaxRecords = 128;
constexpr uint32_t kMaxRecordLength = 2052;
constexpr uint16_t kMaxUuidLength = 64;

bool EncodeCatalogHeader(uint32_t count, std::string& out);
bool DecodeCatalogHeader(const uint8_t* data, size_t size, uint32_t& count);
bool EncodeFavoritesHeader(uint32_t count, std::string& out);
bool DecodeFavoritesHeader(const uint8_t* data, size_t size, uint32_t& count);
bool EncodeCatalogRecord(const RadioStationInfo& station, std::string& out);
bool DecodeCatalogRecord(const uint8_t* data, size_t size, RadioStationInfo& station);
bool EncodeFavoriteUuid(const std::string& uuid, std::string& out);
bool DecodeFavoriteUuid(const uint8_t* data, size_t size, std::string& uuid);
}  // namespace RadioBinaryCodec
