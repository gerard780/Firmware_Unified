#!/usr/bin/env python3
"""Generate bounded startup/linker files from the pinned SDK, without editing it."""
import argparse
from pathlib import Path


def prepare(sdk: Path, output: Path, kib: int) -> None:
    if kib not in (32, 64):
        raise ValueError("SRAM profile must be 32 or 64 KiB")
    startup = (sdk / "boot/B85/cstartup_825x.S").read_text()
    marker = "#if (MCU_STARTUP_8258)"
    if startup.count(marker) != 1:
        raise ValueError("Unrecognized SDK startup: SRAM selector changed")
    branch_end = startup.index("#elif (MCU_STARTUP_8253)")
    if startup[startup.index(marker):branch_end].count("(SRAM_BASE_ADDR + SRAM_64K)") != 1:
        raise ValueError("Unrecognized SDK startup: 8258 SRAM expression changed")
    startup = startup.replace(marker, f"#if (MCU_STARTUP_8258)\n"
                              f"#define OD_TLSR_SRAM_TOP (SRAM_BASE_ADDR + {kib * 1024})", 1)
    startup = startup.replace("(SRAM_BASE_ADDR + SRAM_64K)", "(OD_TLSR_SRAM_TOP)", 1)
    linker = (sdk / "project/tlsr_tc32/B85/boot.link").read_text()
    linker += (f'\nASSERT(__SRAM_SIZE == 0x840000 + {kib * 1024}, "Wrong SRAM stack top");\n'
               'ASSERT(_ram_use_end_ <= __SRAM_SIZE - 3584, "Need 3 KiB stack + 512 byte margin");\n')
    output.mkdir(parents=True, exist_ok=True)
    (output / "cstartup_825x.S").write_text(startup)
    (output / "boot.link").write_text(linker)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sram-kib", required=True, type=int)
    args = parser.parse_args()
    prepare(args.sdk, args.output, args.sram_kib)
