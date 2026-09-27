import base64
import json
import re
import unittest
from pathlib import Path


VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
FIXTURE = Path(__file__).parent / "fixtures" / "github_contents_stable.json"
GITHUB_RESPONSE_LIMIT = 16 * 1024
MANIFEST_LIMIT = 8 * 1024


def is_strict_stable_version(value: str) -> bool:
    return bool(VERSION_RE.fullmatch(value))


def compare_triplets(left: str, right: str) -> int:
    left_parts = tuple(int(part) for part in left.split("."))
    right_parts = tuple(int(part) for part in right.split("."))
    return (left_parts > right_parts) - (left_parts < right_parts)


def discovery_result(current: str, remote: str, test_mode: bool) -> bool:
    if not is_strict_stable_version(remote):
        return False
    return test_mode or compare_triplets(remote, current) > 0


def decode_github_contents_response(response: bytes) -> dict:
    if len(response) > GITHUB_RESPONSE_LIMIT:
        raise ValueError("outer response too large")
    wrapper = json.loads(response)
    if wrapper.get("encoding") != "base64" or not isinstance(wrapper.get("content"), str):
        raise ValueError("invalid GitHub contents wrapper")
    encoded = "".join(wrapper["content"].split())
    decoded = base64.b64decode(encoded, validate=True)
    if not decoded or len(decoded) > MANIFEST_LIMIT:
        raise ValueError("decoded manifest too large")
    return json.loads(decoded)


class CustomOtaPolicyTests(unittest.TestCase):
    def test_stable_versions_are_numeric_triplets_only(self):
        self.assertTrue(is_strict_stable_version("1.1.0"))
        self.assertTrue(is_strict_stable_version("0.0.0"))
        for value in ("1.1.0_beta_ota_1", "v1.1.0", "1.1", "1.1.0+build", ""):
            self.assertFalse(is_strict_stable_version(value))

    def test_production_and_test_discovery_ordering(self):
        self.assertFalse(discovery_result("1.1.0", "1.1.0", False))
        self.assertTrue(discovery_result("1.1.0", "1.1.1", False))
        self.assertFalse(discovery_result("1.2.0", "1.1.0", False))
        self.assertTrue(discovery_result("1.1.0", "1.1.0", True))
        self.assertTrue(discovery_result("1.2.0", "1.1.0", True))
        self.assertTrue(discovery_result("1.1.0_beta_ota_1", "1.1.0", True))

    def test_policy_has_separate_staged_install_path(self):
        root = Path(__file__).parents[2]
        source = (root / "main" / "custom_ota_policy.cc").read_text(encoding="utf-8")
        application = (root / "main" / "application.cc").read_text(encoding="utf-8")
        main = (root / "main" / "main.cc").read_text(encoding="utf-8")
        updater = (root / "main" / "ota" / "minimal_updater.cc").read_text(encoding="utf-8")
        self.assertIn("OTA_POLICY_SKIP_NON_STABLE_VERSION", source)
        self.assertIn("OTA_MANIFEST_HTTP_REQUEST", source)
        self.assertIn("OTA_STAGE_STAGED", source)
        self.assertIn("api.github.com/repos/ZerGint/FNK0104s_xiaozhi_update/contents/ota/stable.json", source)
        self.assertIn("application/vnd.github+json", source)
        self.assertIn("X-GitHub-Api-Version", source)
        self.assertNotIn("raw.githubusercontent.com/ZerGint/FNK0104s_xiaozhi_update/main/ota/stable.json", source)
        self.assertIn("esp_ota_begin", source)
        self.assertIn("esp_ota_write", source)
        self.assertIn("esp_ota_end", source)
        self.assertIn("esp_ota_set_boot_partition", source)
        self.assertIn("CheckForStableUpdate", application)
        self.assertNotIn('"custom_ota"', application)
        self.assertNotIn("StageStableUpdate", application)
        self.assertLess(main.index("MinimalUpdater::RunIfRequested"), main.index("Application::GetInstance"))
        self.assertIn('constexpr char kNamespace[] = "ota_sd"', updater)
        self.assertIn('LogMemory("BEFORE_DOWNLOAD")', updater + source)
        self.assertIn('LogMemory("MIN_VERIFY")', source)
        self.assertIn("StageStableUpdateOnNetwork", updater)
        self.assertNotIn("xTaskCreate", updater)
        self.assertIn("CustomOtaPolicy::InstallStagedUpdate", updater)
        self.assertIn("PENDING_VERIFY", updater)

    def test_github_contents_fixture_decodes_manifest(self):
        manifest = decode_github_contents_response(FIXTURE.read_bytes())
        self.assertEqual(manifest["firmware"]["version"], "1.1.0")

    def test_github_contents_wrapper_rejects_invalid_inputs(self):
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cases = []

        wrong_encoding = dict(fixture)
        wrong_encoding["encoding"] = "utf-8"
        cases.append(wrong_encoding)

        missing_content = dict(fixture)
        del missing_content["content"]
        cases.append(missing_content)

        invalid_base64 = dict(fixture)
        invalid_base64["content"] = "%%%"
        cases.append(invalid_base64)

        oversized = {"content": "A" * (GITHUB_RESPONSE_LIMIT + 1), "encoding": "base64"}
        for case in cases:
            with self.assertRaises((ValueError, KeyError, json.JSONDecodeError)):
                decode_github_contents_response(json.dumps(case).encode())
        with self.assertRaises(ValueError):
            decode_github_contents_response(json.dumps(oversized).encode())


if __name__ == "__main__":
    unittest.main()
