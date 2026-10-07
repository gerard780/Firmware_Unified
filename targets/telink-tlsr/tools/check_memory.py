#!/usr/bin/env python3
"""Audit RAM address occupancy (including copied code/gaps) and the OTA image."""
import argparse
import json
from pathlib import Path
import subprocess


def check(nm: str, elf: Path, image: Path, kib: int) -> dict:
    symbols = {}
    for line in subprocess.check_output([nm, "-n", str(elf)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    top, end = symbols["__SRAM_SIZE"], symbols["_ram_use_end_"]
    if top != 0x840000 + kib * 1024 or top - end < 3584:
        raise ValueError("RAM occupancy violates stack/margin reservation")
    flash = image.stat().st_size
    if flash > 192 * 1024:
        raise ValueError("Finished image exceeds a 192 KiB OTA bank")
    return {"sram_bytes": kib * 1024, "occupied_ram_bytes": end - 0x840000,
            "main_stack_minimum_bytes": 3072, "reserve_minimum_bytes": 512, "stack_and_margin_bytes": top - end,
            "irq_stack_in_occupied_ram_bytes": 384, "finished_image_bytes": flash,
            "hardware_stack_high_water": "unmeasured"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--sram-kib", type=int, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = check(args.nm, args.elf, args.image, args.sram_kib)
    args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
