import unittest

from scripts.firmware_version import Comparison, compare_versions, format_version, parse_version


class FirmwareVersionTests(unittest.TestCase):
    def test_format_and_parse(self):
        self.assertEqual(format_version(parse_version("1.1.3_beta_ota_1")), "1.1.3_beta_ota_1")
        self.assertEqual(format_version(parse_version("1.1.0")), "1.1.0")

    def test_ordering(self):
        self.assertEqual(compare_versions("1.1.3_beta_ota_1", "1.1.3_beta_ota_2"), Comparison.LESS)
        self.assertEqual(compare_versions("1.1.3_beta_ota_2", "1.1.3"), Comparison.LESS)
        self.assertEqual(compare_versions("1.1.3", "1.2.0"), Comparison.LESS)

    def test_cross_channel_is_incomparable(self):
        self.assertEqual(
            compare_versions("1.1.3_beta_ota_2", "1.1.3_beta_aec_3"),
            Comparison.INCOMPARABLE,
        )

    def test_invalid_version(self):
        with self.assertRaises(ValueError):
            parse_version("1.1.beta")


if __name__ == "__main__":
    unittest.main()
