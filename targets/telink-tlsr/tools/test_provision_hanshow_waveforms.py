"""Synthetic offline provisioner tests; no manufacturer waveform bytes stored."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import provision_hanshow_waveforms as tool


class ProvisionTest(unittest.TestCase):
    def test_complete_hash_verified_pair_and_no_extension_emitted(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            extension = bytes.fromhex("090006070f0a2f25222b21")
            records = [bytes([component + 1]) * 535 + extension for component in range(2)]
            hashes = tuple(hashlib.sha256(record).hexdigest() for record in records)
            for component, record in enumerate(records):
                (root / f"pass{component}.bin").write_bytes(record)
            manifest = root / "input.json"
            manifest.write_text(json.dumps({"profiles": [{"panel": "154", "band": 6,
                "temperature_c": 24.5, "records": ["pass0.bin", "pass1.bin"]}]}))
            with patch.dict(tool.PROFILES, {("154", "154", 6): (1035, 2400, 2500, extension.hex(), hashes)}):
                profiles = tool.load_profiles(manifest)
            source = tool.render_provider(profiles)
            self.assertEqual(source.count("0x01"), 535)
            self.assertEqual(source.count("0x02"), 535)
            self.assertIn(".panel_id = 1035u", source)
            tool.write_private_output(root / "private", profiles)
            self.assertEqual((root / "private").stat().st_mode & 0o777, 0o700)
            self.assertEqual((root / "private/hanshow_waveforms.c").stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                tool.write_private_output(root / "private", profiles)
            (root / "pass0.bin").write_bytes(records[1])
            with self.assertRaisesRegex(ValueError, "fingerprint"):
                tool.load_profiles(manifest)

    def test_explicit_family_retains_both_component_extensions_and_backup_stride(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            extension = bytes.fromhex("130006070f0a2f25222e2160020202650400000000")
            records = [bytes([component + 3]) * 535 + extension for component in range(2)]
            hashes = tuple(hashlib.sha256(record).hexdigest() for record in records)
            capture = bytearray(512 * 1024)
            for component, record in enumerate(records):
                address = 0x78188 + (10 + component) * 556
                capture[address:address + 556] = record
                (root / f"pass{component}.bin").write_bytes(record)
            (root / "backup.bin").write_bytes(capture)
            manifest = root / "input.json"
            spec = {"panel": "266", "waveform_family": "0enspq1yq", "band": 5,
                    "temperature_c": 24.5, "backup": "backup.bin"}
            manifest.write_text(json.dumps({"profiles": [spec]}))
            with patch.dict(tool.PROFILES, {("266", "0enspq1yq", 5):
                    (1036, 2001, 2500, extension.hex(), hashes)}):
                profiles = tool.load_profiles(manifest)
                self.assertEqual(profiles[0]["records"], records)
                self.assertEqual(profiles[0]["waveform_family"], "0enspq1yq")
                self.assertEqual(profiles[0]["source_sha256"], hashlib.sha256(capture).hexdigest())
                spec.pop("backup")
                spec["records"] = ["pass0.bin", "pass1.bin"]
                manifest.write_text(json.dumps({"profiles": [spec]}))
                self.assertEqual(tool.load_profiles(manifest)[0]["records"], records)
                (root / "pass1.bin").write_bytes(records[1][:-1])
                with self.assertRaisesRegex(ValueError, "complete records"):
                    tool.load_profiles(manifest)
                corrupt = records[1][:-1] + b"\x01"
                (root / "pass1.bin").write_bytes(corrupt)
                with self.assertRaisesRegex(ValueError, "command extension"):
                    tool.load_profiles(manifest)
            source = tool.render_provider(profiles)
            self.assertIn(".family = HANSHOW_QN_FAMILY_0ENSPQ1YQ", source)
            self.assertIn(".initializer = { &qn266_init0, &qn266_init1 }", source)
            self.assertEqual(source.count(".booster = { 0x0f, 0x0a, 0x2f, 0x25, 0x22, 0x2e, 0x21 }"), 2)
            self.assertEqual(source.count(".timing = { 0x02, 0x02 }"), 2)
            self.assertEqual(source.count(".gate_start = { 0x00, 0x00, 0x00, 0x00 }"), 2)

    def test_default_family_and_explicit_family_refuse_wrong_records(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            manifest = root / "input.json"
            spec = {"panel": "266", "band": 6, "temperature_c": 24.5,
                    "records": ["pass0.bin", "pass1.bin"]}
            legacy_extension = bytes.fromhex("090006070f0a2f25222e21")
            records = [bytes([component + 1]) * 535 + legacy_extension for component in range(2)]
            hashes = tuple(hashlib.sha256(record).hexdigest() for record in records)
            for component, record in enumerate(records):
                (root / f"pass{component}.bin").write_bytes(record)
            manifest.write_text(json.dumps({"profiles": [spec]}))
            with patch.dict(tool.PROFILES, {("266", "0cnspq1tl", 6):
                    (1036, 2400, 2500, legacy_extension.hex(), hashes)}):
                profiles = tool.load_profiles(manifest)
                self.assertEqual(profiles[0]["waveform_family"], "0cnspq1tl")
                self.assertNotIn(".initializer =", tool.render_provider(profiles))
                spec["waveform_family"] = "0enspq1yq"
                manifest.write_text(json.dumps({"profiles": [spec]}))
                with self.assertRaisesRegex(ValueError, "supported profiles"):
                    tool.load_profiles(manifest)
                spec["band"] = 5
                manifest.write_text(json.dumps({"profiles": [spec]}))
                with self.assertRaisesRegex(ValueError, "complete records"):
                    tool.load_profiles(manifest)
                spec["panel"] = "154"
                manifest.write_text(json.dumps({"profiles": [spec]}))
                with self.assertRaisesRegex(ValueError, "supported profiles"):
                    tool.load_profiles(manifest)
                spec["panel"] = "266"
                spec["temperature_c"] = 25.01
                manifest.write_text(json.dumps({"profiles": [spec]}))
                with self.assertRaisesRegex(ValueError, "temperature"):
                    tool.load_profiles(manifest)

    def test_unknown_profile_and_temperature_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            manifest = Path(name) / "input.json"
            for panel, band, temperature in (("266", 7, 26), ("154", 6, 26), ("154", 6, "NaN")):
                manifest.write_text(json.dumps({"profiles": [{"panel": panel, "band": band,
                    "temperature_c": temperature, "records": ["missing0", "missing1"]}]}))
                with self.assertRaises(ValueError):
                    tool.load_profiles(manifest)

    def test_default_has_no_waveforms_and_repo_output_rejected(self):
        source = tool.render_provider([])
        self.assertIn("return NULL;", source)
        self.assertNotIn("static const uint8_t", source)
        with self.assertRaisesRegex(ValueError, "outside"):
            tool.write_private_output(tool.REPOSITORY / "private", [])


if __name__ == "__main__":
    unittest.main()
