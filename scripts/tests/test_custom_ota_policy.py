import base64
import json
import re
import unittest
from pathlib import Path

VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
FIXTURE = Path(__file__).parent / "fixtures" / "github_contents_stable.json"
GITHUB_RESPONSE_LIMIT = 16 * 1024
MANIFEST_LIMIT = 8 * 1024


def is_strict_stable_version(value):
    return bool(VERSION_RE.fullmatch(value))


def compare_triplets(left, right):
    a = tuple(int(part) for part in left.split("."))
    b = tuple(int(part) for part in right.split("."))
    return (a > b) - (a < b)


def discovery_result(current, remote, test_mode):
    if not is_strict_stable_version(remote):
        return False
    return test_mode or compare_triplets(remote, current) > 0


def decode_github_contents_response(response):
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
        for value in ("1.1.0_beta_ota_1", "v1.1.0", "1.1", "1.1.0+build", ""):
            self.assertFalse(is_strict_stable_version(value))

    def test_production_and_test_discovery_ordering(self):
        self.assertFalse(discovery_result("1.1.0", "1.1.0", False))
        self.assertTrue(discovery_result("1.1.0", "1.1.1", False))
        self.assertFalse(discovery_result("1.2.0", "1.1.0", False))
        self.assertTrue(discovery_result("1.1.0", "1.1.0", True))
        self.assertTrue(discovery_result("1.2.0", "1.1.0", True))
        self.assertTrue(discovery_result("1.1.0_beta_ota_1", "1.1.0", True))

    def test_discovery_is_only_user_driven(self):
        root = Path(__file__).parents[2]
        application = (root / "main" / "application.cc").read_text(encoding="utf-8")
        display = (root / "main" / "display" / "lcd_display.cc").read_text(encoding="utf-8")
        policy = (root / "main" / "custom_ota_policy.cc").read_text(encoding="utf-8")
        self.assertNotIn("CheckForStableUpdate()", application)
        self.assertIn("StartOtaCheck", display)
        self.assertIn("CheckForStableUpdate()", display)
        self.assertIn("api.github.com/repos/ZerGint/FNK0104s_xiaozhi_update/contents/ota/stable.json", policy)
        self.assertIn("application/vnd.github+json", policy)
        self.assertIn("X-GitHub-Api-Version", policy)
        self.assertNotIn("raw.githubusercontent.com/ZerGint/FNK0104s_xiaozhi_update/main/ota/stable.json", policy)

    def test_primary_state_machine_and_minimal_updater_contract(self):
        root = Path(__file__).parents[2]
        header = (root / "main" / "ota" / "minimal_updater.h").read_text(encoding="utf-8")
        updater = (root / "main" / "ota" / "minimal_updater.cc").read_text(encoding="utf-8")
        main = (root / "main" / "main.cc").read_text(encoding="utf-8")
        for state in ("STABLE = 0", "UPDATE = 1", "INSTALLED = 2"):
            self.assertIn(state, header)
        for old in ("PENDING_VERIFY", "VALIDATED", "STAGING", "STAGED", "INSTALL_REQUESTED", "INSTALLING"):
            self.assertNotIn("State::" + old, updater)
        self.assertIn("OTA_UPDATE_DIR_CLEAR_START", policy_text(root))
        self.assertIn("ClearUpdateDirectory", updater)
        self.assertIn("esp_ota_set_boot_partition", policy_text(root))
        self.assertIn("OTA_ROLLBACK_DETECTED", updater)
        self.assertNotIn("xTaskCreate", updater)
        self.assertLess(main.index("Application::GetInstance"), main.index("MinimalUpdater::ValidatePendingUpdate"))

    def test_post_update_policy_model(self):
        self.assertEqual("STABLE", lifecycle(False, False, False))
        self.assertEqual("STABLE", lifecycle(True, True, False))
        self.assertEqual("STABLE", lifecycle(True, False, True))
        self.assertEqual("STABLE", lifecycle(True, True, True))

    def test_check_manifest_then_update_request_contract(self):
        root = Path(__file__).parents[2]
        display = (root / "main" / "display" / "lcd_display.cc").read_text(encoding="utf-8")
        policy = policy_text(root)
        self.assertIn("OTA_UI_CHECK_TAP", display)
        self.assertIn("ota_check_in_progress_", display)
        self.assertIn("OTA_UPDATE_REQUESTED", policy)
        self.assertIn("WriteUpdateRequest", policy)
        self.assertNotIn("StageStableUpdateOnNetwork", display)
        self.assertNotIn("esp_restart", display.split("StartOtaCheck", 1)[1].split("SetOtaUpdateAvailable", 1)[0])

    def test_github_contents_fixture_decodes_manifest(self):
        manifest = decode_github_contents_response(FIXTURE.read_bytes())
        self.assertEqual(manifest["firmware"]["version"], "1.1.0")

    def test_github_contents_wrapper_rejects_invalid_inputs(self):
        fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
        cases = []
        wrong = dict(fixture); wrong["encoding"] = "utf-8"; cases.append(wrong)
        missing = dict(fixture); del missing["content"]; cases.append(missing)
        invalid = dict(fixture); invalid["content"] = "%%%"; cases.append(invalid)
        for case in cases:
            with self.assertRaises((ValueError, KeyError, json.JSONDecodeError)):
                decode_github_contents_response(json.dumps(case).encode())
        oversized = {"content": "A" * (GITHUB_RESPONSE_LIMIT + 1), "encoding": "base64"}
        with self.assertRaises(ValueError):
            decode_github_contents_response(json.dumps(oversized).encode())


def policy_text(root):
    return (root / "main" / "custom_ota_policy.cc").read_text(encoding="utf-8")


def lifecycle(partition_ok, version_ok, mark_valid_ok):
    if not partition_ok or not version_ok or not mark_valid_ok:
        return "STABLE"
    return "STABLE"


if __name__ == "__main__":
    unittest.main()
