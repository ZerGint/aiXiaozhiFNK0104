"""Standalone v1 codecs for the planned binary radio stores."""

import struct

MAGIC_CATALOG = b"RDBC"
MAGIC_FAVORITES = b"RFAV"
VERSION = 1
MAX_RECORDS = 128
MAX_UUID = 64
FIELD_LIMITS = {
    "stationuuid": 64,
    "name": 256,
    "url_resolved": 512,
    "codec": 32,
    "country": 128,
    "countrycode": 16,
    "state": 128,
    "language": 64,
    "tags": 512,
}
HEADER_SIZE = 16
MAX_RECORD_LENGTH = 2052


class BinaryFormatError(ValueError):
    pass


def encode_header(magic: bytes, count: int) -> bytes:
    if magic not in (MAGIC_CATALOG, MAGIC_FAVORITES):
        raise BinaryFormatError("invalid magic")
    if not 0 <= count <= MAX_RECORDS:
        raise BinaryFormatError("invalid record count")
    return magic + struct.pack("<HHII", VERSION, 0, count, 0)


def decode_header(data: bytes, magic: bytes) -> int:
    if len(data) < HEADER_SIZE:
        raise BinaryFormatError("truncated header")
    if data[:4] != magic:
        raise BinaryFormatError("bad magic")
    version, flags, count, reserved = struct.unpack_from("<HHII", data, 4)
    if version != VERSION:
        raise BinaryFormatError("unsupported version")
    if flags != 0 or reserved != 0:
        raise BinaryFormatError("invalid flags")
    if count > MAX_RECORDS:
        raise BinaryFormatError("invalid record count")
    return count


def _encode_string(value: str, limit: int) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > limit:
        raise BinaryFormatError("field too long")
    return struct.pack("<H", len(raw)) + raw


def _decode_string(data: bytes, offset: int, end: int, limit: int):
    if offset + 2 > end:
        raise BinaryFormatError("truncated field length")
    length = struct.unpack_from("<H", data, offset)[0]
    offset += 2
    if length > limit or length > end - offset:
        raise BinaryFormatError("invalid field length")
    raw = data[offset:offset + length]
    return raw.decode("utf-8"), offset + length


def encode_catalog_record(station: dict) -> bytes:
    payload = bytearray()
    for field in ("stationuuid", "name", "url_resolved", "codec"):
        payload += _encode_string(station.get(field, ""), FIELD_LIMITS[field])
    payload += struct.pack("<I", station.get("bitrate", 0))
    for field in ("country", "countrycode", "state", "language", "tags"):
        payload += _encode_string(station.get(field, ""), FIELD_LIMITS[field])
    total = 4 + len(payload)
    if total > MAX_RECORD_LENGTH:
        raise BinaryFormatError("record too long")
    return struct.pack("<I", total) + payload


def decode_catalog_record(data: bytes) -> dict:
    if len(data) < 4:
        raise BinaryFormatError("truncated record")
    total = struct.unpack_from("<I", data)[0]
    if total < 4 or total > MAX_RECORD_LENGTH or total != len(data):
        raise BinaryFormatError("invalid record length")
    offset, end = 4, total
    station = {}
    for field in ("stationuuid", "name", "url_resolved", "codec"):
        station[field], offset = _decode_string(data, offset, end, FIELD_LIMITS[field])
    if offset + 4 > end:
        raise BinaryFormatError("truncated bitrate")
    station["bitrate"] = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    for field in ("country", "countrycode", "state", "language", "tags"):
        station[field], offset = _decode_string(data, offset, end, FIELD_LIMITS[field])
    if offset != end:
        raise BinaryFormatError("unexpected record bytes")
    return station


def encode_favorite_uuid(uuid: str) -> bytes:
    raw = uuid.encode("utf-8")
    if not raw or len(raw) > MAX_UUID:
        raise BinaryFormatError("invalid UUID")
    return struct.pack("<H", len(raw)) + raw


def decode_favorite_uuid(data: bytes) -> str:
    if len(data) < 2:
        raise BinaryFormatError("truncated UUID length")
    length = struct.unpack_from("<H", data)[0]
    if not 0 < length <= MAX_UUID or length + 2 != len(data):
        raise BinaryFormatError("invalid UUID record")
    try:
        return data[2:].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise BinaryFormatError("invalid UUID UTF-8") from exc


def decode_catalog_file(data: bytes):
    count = decode_header(data[:HEADER_SIZE], MAGIC_CATALOG)
    offset, records = HEADER_SIZE, []
    for _ in range(count):
        if offset + 4 > len(data):
            raise BinaryFormatError("truncated record")
        total = struct.unpack_from("<I", data, offset)[0]
        if total > len(data) - offset:
            raise BinaryFormatError("truncated record")
        records.append(decode_catalog_record(data[offset:offset + total]))
        offset += total
    if offset != len(data):
        raise BinaryFormatError("unexpected trailing bytes")
    return records


def decode_favorites_file(data: bytes):
    count = decode_header(data[:HEADER_SIZE], MAGIC_FAVORITES)
    offset, records = HEADER_SIZE, []
    for _ in range(count):
        if offset + 2 > len(data):
            raise BinaryFormatError("truncated UUID length")
        length = struct.unpack_from("<H", data, offset)[0]
        end = offset + 2 + length
        if end > len(data):
            raise BinaryFormatError("truncated UUID record")
        records.append(decode_favorite_uuid(data[offset:end]))
        offset = end
    if offset != len(data):
        raise BinaryFormatError("unexpected trailing bytes")
    return records
