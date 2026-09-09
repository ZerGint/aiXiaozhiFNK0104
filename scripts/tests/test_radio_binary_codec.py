import struct
import unittest

from scripts.radio_binary_codec import (
    BinaryFormatError, FIELD_LIMITS, HEADER_SIZE, MAGIC_CATALOG,
    MAGIC_FAVORITES, MAX_RECORD_LENGTH, decode_catalog_record, decode_favorite_uuid,
    decode_catalog_file, decode_favorites_file, decode_header, encode_catalog_record,
    encode_favorite_uuid, encode_header,
)


class BinaryCodecTests(unittest.TestCase):
    def station(self):
        return {field: "" for field in FIELD_LIMITS} | {"bitrate": 128}

    def test_catalog_header_and_empty(self):
        data = encode_header(MAGIC_CATALOG, 0)
        self.assertEqual(len(data), HEADER_SIZE)
        self.assertEqual(decode_header(data, MAGIC_CATALOG), 0)
        self.assertEqual(decode_catalog_file(data), [])

    def test_catalog_round_trip_all_fields_utf8(self):
        station = self.station() | {"stationuuid": "u1", "name": "Радио", "state": "Москва"}
        self.assertEqual(decode_catalog_record(encode_catalog_record(station)), station)

    def test_catalog_empty_and_multiple_records_are_independent(self):
        first, second = self.station() | {"name": "a"}, self.station() | {"name": "b"}
        self.assertEqual(decode_catalog_record(encode_catalog_record(first))["name"], "a")
        self.assertEqual(decode_catalog_record(encode_catalog_record(second))["name"], "b")

    def test_catalog_max_fields_and_overflow(self):
        station = self.station()
        for field, limit in FIELD_LIMITS.items():
            station[field] = "x" * limit
        self.assertLessEqual(len(encode_catalog_record(station)), MAX_RECORD_LENGTH)
        for field, limit in FIELD_LIMITS.items():
            bad = self.station() | {field: "x" * (limit + 1)}
            with self.assertRaises(BinaryFormatError):
                encode_catalog_record(bad)

    def test_catalog_header_validation(self):
        valid = encode_header(MAGIC_CATALOG, 0)
        cases = [valid[:HEADER_SIZE - 1], b"BAD!" + valid[4:], valid[:4] + struct.pack("<H", 2) + valid[6:], valid[:6] + struct.pack("<H", 1) + valid[8:], valid[:8] + struct.pack("<I", 129) + valid[12:]]
        for case in cases:
            with self.assertRaises(BinaryFormatError):
                decode_header(case, MAGIC_CATALOG)

    def test_catalog_record_validation_and_no_partial_success(self):
        record = encode_catalog_record(self.station())
        for bad in (record[:3], record[:-1], struct.pack("<I", 3) + record[4:], struct.pack("<I", MAX_RECORD_LENGTH + 1) + record[4:]):
            with self.assertRaises(BinaryFormatError):
                decode_catalog_record(bad)
        with self.assertRaises(BinaryFormatError):
            decode_catalog_record(record + b"x")

    def test_favorites_header_and_uuids(self):
        self.assertEqual(decode_header(encode_header(MAGIC_FAVORITES, 2), MAGIC_FAVORITES), 2)
        for uuid in ("abc", "тест-uuid"):
            self.assertEqual(decode_favorite_uuid(encode_favorite_uuid(uuid)), uuid)
        data = encode_header(MAGIC_FAVORITES, 2) + encode_favorite_uuid("abc") + encode_favorite_uuid("def")
        self.assertEqual(decode_favorites_file(data), ["abc", "def"])

    def test_favorites_duplicate_representation_is_codec_valid(self):
        self.assertEqual(decode_favorite_uuid(encode_favorite_uuid("same")), "same")
        self.assertEqual(decode_favorite_uuid(encode_favorite_uuid("same")), "same")

    def test_favorites_uuid_limits_and_validation(self):
        self.assertEqual(len(encode_favorite_uuid("x" * 64)), 66)
        for uuid in ("", "x" * 65):
            with self.assertRaises(BinaryFormatError):
                encode_favorite_uuid(uuid)
        for bad in (b"", b"\x01", b"\x00\x00", b"\x41\x00a", b"\x01\x00"):
            with self.assertRaises(BinaryFormatError):
                decode_favorite_uuid(bad)

    def test_favorites_header_count_and_trailing_bytes(self):
        with self.assertRaises(BinaryFormatError):
            decode_header(encode_header(MAGIC_FAVORITES, 0)[:10], MAGIC_FAVORITES)
        with self.assertRaises(BinaryFormatError):
            decode_header(MAGIC_FAVORITES + struct.pack("<HHII", 1, 0, 129, 0), MAGIC_FAVORITES)
        with self.assertRaises(BinaryFormatError):
            decode_favorite_uuid(encode_favorite_uuid("abc") + b"x")


if __name__ == "__main__":
    unittest.main()
