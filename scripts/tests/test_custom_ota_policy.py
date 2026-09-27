import base64
import hashlib
import json
import re
import tempfile
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


def post_update_policy(version_match: bool, partition_match: bool, selftest_pass: bool,
                       mark_valid_pass: bool, cleanup_pass: bool) -> dict:
    """Small host model of the post-update state machine's safety contract."""
    result = {"state": "PENDING_VERIFY", "marked_valid": False, "cleaned": False}
    if not partition_match:
        result["state"] = "FAILED"
        result["reason"] = "ROLLBACK_DETECTED"
        return result
    if not version_match or not selftest_pass:
        result["state"] = "FAILED"
        result["reason"] = "VERIFY_FAILED"
        return result
    if not mark_valid_pass:
        result["state"] = "FAILED"
        result["reason"] = "MARK_VALID_FAILED"
        return result
    result["marked_valid"] = True
    result["state"] = "VALIDATED"
    if not cleanup_pass:
        result["state"] = "IDLE"
        result["cleanup_pending"] = True
        return result
    result["cleaned"] = True
    result["state"] = "IDLE"
    return result


def stage_file_names(root: Path):
    return {
        "firmware": root / "firmware.bin",
        "firmware_tmp": root / "firmware.tmp",
        "metadata": root / "staged-info.json",
        "metadata_tmp": root / "staged-info.tmp",
    }


def recover_tmp_files(root: Path) -> None:
    files = stage_file_names(root)
    for key in ("firmware_tmp", "metadata_tmp"):
        files[key].unlink(missing_ok=True)


def existing_stage_matches(root: Path, version: str, payload: bytes) -> bool:
    files = stage_file_names(root)
    if not files["firmware"].is_file() or not files["metadata"].is_file():
        return False
    metadata = json.loads(files["metadata"].read_text(encoding="utf-8"))
    digest = hashlib.sha256(files["firmware"].read_bytes()).hexdigest()
    return (metadata.get("version") == version and metadata.get("size") == len(payload)
            and metadata.get("sha256") == hashlib.sha256(payload).hexdigest()
            and files["firmware"].stat().st_size == len(payload) and digest == metadata["sha256"])


def atomic_stage_replace(root: Path, version: str, payload: bytes,
                         fail_remove: bool = False) -> bool:
    files = stage_file_names(root)
    digest = hashlib.sha256(payload).hexdigest()
    files["firmware_tmp"].write_bytes(payload)
    files["metadata_tmp"].write_text(
        json.dumps({"state": "staged", "version": version, "size": len(payload),
                    "sha256": digest}),
        encoding="utf-8")
    if fail_remove:
        return False
    files["firmware"].unlink(missing_ok=True)
    files["firmware_tmp"].replace(files["firmware"])
    files["metadata"].unlink(missing_ok=True)
    files["metadata_tmp"].replace(files["metadata"])
    return True


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

    def test_post_update_lifecycle_contract_and_ordering(self):
        root = Path(__file__).parents[2]
        main = (root / "main" / "main.cc").read_text(encoding="utf-8")
        ota = (root / "main" / "ota.cc").read_text(encoding="utf-8")
        updater = (root / "main" / "ota" / "minimal_updater.cc").read_text(encoding="utf-8")
        policy = (root / "main" / "custom_ota_policy.cc").read_text(encoding="utf-8")
        for marker in (
            "OTA_POST_UPDATE_PENDING", "OTA_POST_UPDATE_SELFTEST",
            "OTA_POST_UPDATE_MARK_VALID", "OTA_POST_UPDATE_STATE VALIDATED",
            "OTA_ROLLBACK_DETECTED", "OTA_POST_UPDATE_VERSION_MISMATCH",
            "OTA_NVS_METADATA_CLEANED",
        ):
            self.assertIn(marker, updater)
        self.assertIn('LogVerifyMemory("BEFORE")', updater)
        self.assertIn('LogVerifyMemory("AFTER")', updater)
        self.assertIn("CleanupStagedFiles", updater)
        self.assertIn("OTA_CLEANUP_FILE", policy)
        self.assertLess(main.index("Application::GetInstance"),
                        main.index("MinimalUpdater::ValidatePendingUpdate"))
        self.assertLess(main.index("MinimalUpdater::ValidatePendingUpdate"), main.index("app.Run"))
        validate = updater[updater.index("bool ValidatePendingUpdate") :]
        self.assertLess(validate.index("esp_ota_mark_app_valid_cancel_rollback"),
                        validate.index("CustomOtaPolicy::CleanupStagedFiles"))
        self.assertLess(validate.index("CustomOtaPolicy::CleanupStagedFiles"),
                        validate.index("ClearValidatedMetadata"))
        guard = ota.index("MinimalUpdater::IsValidationPendingOrFailed")
        mark = ota.index("esp_ota_mark_app_valid_cancel_rollback")
        self.assertLess(guard, mark)

    def test_post_update_failure_injection_policy(self):
        cases = (
            ("partition rollback", dict(version_match=True, partition_match=False,
                                         selftest_pass=True, mark_valid_pass=True,
                                         cleanup_pass=True), "FAILED", False, False),
            ("version mismatch", dict(version_match=False, partition_match=True,
                                       selftest_pass=True, mark_valid_pass=True,
                                       cleanup_pass=True), "FAILED", False, False),
            ("self test failure", dict(version_match=True, partition_match=True,
                                        selftest_pass=False, mark_valid_pass=True,
                                        cleanup_pass=True), "FAILED", False, False),
            ("mark valid failure", dict(version_match=True, partition_match=True,
                                         selftest_pass=True, mark_valid_pass=False,
                                         cleanup_pass=True), "FAILED", False, False),
            ("cleanup failure", dict(version_match=True, partition_match=True,
                                      selftest_pass=True, mark_valid_pass=True,
                                      cleanup_pass=False), "IDLE", True, False),
            ("successful validation", dict(version_match=True, partition_match=True,
                                            selftest_pass=True, mark_valid_pass=True,
                                            cleanup_pass=True), "IDLE", True, True),
        )
        for name, inputs, state, marked_valid, cleaned in cases:
            with self.subTest(name=name):
                result = post_update_policy(**inputs)
                self.assertEqual(result["state"], state)
                self.assertEqual(result["marked_valid"], marked_valid)
                self.assertEqual(result["cleaned"], cleaned)

    def test_stale_staging_state_matrix(self):
        payload = b"new-image"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = stage_file_names(root)

            # A: clean directory, B/H: temporary leftovers are bounded and removed.
            self.assertTrue(atomic_stage_replace(root, "1.1.1", payload))
            self.assertTrue(existing_stage_matches(root, "1.1.1", payload))
            files["firmware_tmp"].write_bytes(b"stale")
            files["metadata_tmp"].write_text("stale", encoding="utf-8")
            recover_tmp_files(root)
            self.assertFalse(files["firmware_tmp"].exists())
            self.assertFalse(files["metadata_tmp"].exists())

            # C: matching final is already present and requires no download.
            self.assertTrue(existing_stage_matches(root, "1.1.1", payload))

            # D/G: invalid or different-version final is replaced only after verification.
            files["firmware"].write_bytes(b"old-image")
            self.assertFalse(existing_stage_matches(root, "1.1.1", payload))
            self.assertTrue(atomic_stage_replace(root, "1.1.1", payload))
            files["metadata"].write_text(
                json.dumps({"version": "1.0.0", "size": len(payload),
                            "sha256": hashlib.sha256(payload).hexdigest()}),
                encoding="utf-8")
            self.assertFalse(existing_stage_matches(root, "1.1.1", payload))
            self.assertTrue(atomic_stage_replace(root, "1.1.1", payload))

            # E/F: orphan final or metadata is repaired by the next commit.
            files["metadata"].unlink()
            self.assertTrue(atomic_stage_replace(root, "1.1.1", payload))
            files["firmware"].unlink()
            self.assertTrue(atomic_stage_replace(root, "1.1.1", payload))

            # A remove/replace failure stops staging before install.
            files["firmware"].unlink()
            files["firmware"].mkdir()
            self.assertFalse(atomic_stage_replace(root, "1.1.2", payload, fail_remove=True))

    def test_stale_recovery_and_updater_reentry_contract(self):
        root = Path(__file__).parents[2]
        policy = (root / "main" / "custom_ota_policy.cc").read_text(encoding="utf-8")
        updater = (root / "main" / "ota" / "minimal_updater.cc").read_text(encoding="utf-8")
        for marker in (
            "OTA_STALE_STAGE_DETECTED", "OTA_STALE_STAGE_CLEANUP",
            "OTA_STAGE_REMOVE", "OTA_STAGE_RENAME", "firmware.tmp->firmware.bin",
            "staged-info.tmp->staged-info.json", "OTA_STAGE_FAILED reason=atomic_commit",
        ):
            self.assertIn(marker, policy)
        self.assertIn("OTA_MINIMAL_REBOOT_AFTER_FAILURE", updater)
        self.assertIn("OTA_MINIMAL_ALREADY_ENTERED", updater)
        self.assertIn('return FailAndReboot("stage")', updater)

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
