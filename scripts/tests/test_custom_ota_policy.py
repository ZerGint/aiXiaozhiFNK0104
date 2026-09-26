import re
import unittest
from pathlib import Path


VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")


def is_strict_stable_version(value: str) -> bool:
    return bool(VERSION_RE.fullmatch(value))


class CustomOtaPolicyTests(unittest.TestCase):
    def test_stable_versions_are_numeric_triplets_only(self):
        self.assertTrue(is_strict_stable_version("1.1.0"))
        self.assertTrue(is_strict_stable_version("0.0.0"))
        for value in ("1.1.0_beta_ota_1", "v1.1.0", "1.1", "1.1.0+build", ""):
            self.assertFalse(is_strict_stable_version(value))

    def test_policy_has_no_install_path(self):
        source = (Path(__file__).parents[2] / "main" / "custom_ota_policy.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("OTA_POLICY_SKIP_NON_STABLE_VERSION", source)
        self.assertIn("OTA_MANIFEST_HTTP_REQUEST", source)
        self.assertIn("OTA_STAGE_STAGED", source)
        self.assertNotIn("esp_ota_begin", source)
        self.assertNotIn("esp_ota_write", source)
        self.assertNotIn("esp_ota_end", source)
        self.assertNotIn("esp_ota_set_boot_partition", source)


if __name__ == "__main__":
    unittest.main()
