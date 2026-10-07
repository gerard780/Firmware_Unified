"""Verify generated SRAM ceilings and fail-closed post-link resource checks."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check_memory
import prepare_memory_profile


class MemoryProfileTest(unittest.TestCase):
    def test_startup_and_linker_profiles_preserve_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdk"
            (sdk / "boot/B85").mkdir(parents=True)
            (sdk / "project/tlsr_tc32/B85").mkdir(parents=True)
            startup = ("#if (MCU_STARTUP_8258)\n"
                       "#define SRAM_SIZE (SRAM_BASE_ADDR + SRAM_64K)\n"
                       "#elif (MCU_STARTUP_8253)\n#endif\n"
                       ".word SRAM_SIZE\n.word SRAM_SIZE\n")
            source = sdk / "boot/B85/cstartup_825x.S"
            source.write_text(startup)
            (sdk / "project/tlsr_tc32/B85/boot.link").write_text("/* SDK linker */\n")
            for kib in (32, 64):
                output = Path(directory) / str(kib)
                prepare_memory_profile.prepare(sdk, output, kib)
                generated = (output / "cstartup_825x.S").read_text()
                self.assertIn(f"SRAM_BASE_ADDR + {kib * 1024}", generated)
                self.assertIn("#define SRAM_SIZE (OD_TLSR_SRAM_TOP)", generated)
                self.assertEqual(generated.count(".word SRAM_SIZE"), 2)
                self.assertIn("__SRAM_SIZE - 3584", (output / "boot.link").read_text())
            self.assertEqual(source.read_text(), startup)
            source.write_text(startup.replace("SRAM_64K", "SRAM_48K"))
            with self.assertRaises(ValueError):
                prepare_memory_profile.prepare(sdk, Path(directory) / "bad", 32)

    def test_link_audit_counts_address_gaps_and_bounds_flash(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "image.bin"
            image.write_bytes(bytes(192 * 1024))
            symbols = "00848000 A __SRAM_SIZE\n00847200 A _ram_use_end_\n"
            with patch("check_memory.subprocess.check_output", return_value=symbols):
                result = check_memory.check("nm", Path("image.elf"), image, 32)
                self.assertEqual(result["occupied_ram_bytes"], 29184)
                self.assertEqual(result["stack_and_margin_bytes"], 3584)
                self.assertEqual(result["main_stack_minimum_bytes"], 3072)
                image.write_bytes(bytes(192 * 1024 + 1))
                with self.assertRaises(ValueError):
                    check_memory.check("nm", Path("image.elf"), image, 32)
            for symbols in ("00850000 A __SRAM_SIZE\n00847200 A _ram_use_end_\n",
                            "00848000 A __SRAM_SIZE\n00847204 A _ram_use_end_\n"):
                with patch("check_memory.subprocess.check_output", return_value=symbols):
                    with self.assertRaises(ValueError):
                        check_memory.check("nm", Path("image.elf"), image, 32)


if __name__ == "__main__":
    unittest.main()
