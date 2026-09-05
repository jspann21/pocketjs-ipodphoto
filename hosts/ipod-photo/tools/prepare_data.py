#!/usr/bin/env python3
"""Prepare a new, offline POCKETJS directory without overwriting saved data."""
import argparse
from pathlib import Path
import struct
import zlib

from build_embedded_pocket import verify_container, fnv1a64
from build_lineage_slots import record, SOURCE_ACTIVE, SOURCE_EMBEDDED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new offline output directory")
    parser.add_argument("--recovery", type=Path, required=True)
    parser.add_argument("--launcher", type=Path, required=True)
    args = parser.parse_args()
    recovery = args.recovery.read_bytes()
    launcher = args.launcher.read_bytes()
    verify_container(recovery)
    verify_container(launcher)
    package_hash = fnv1a64(recovery[:-8])
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output / "POCKETJS"
    (root / "APPS").mkdir(parents=True)
    for name in ("ACTIVE.PKT", "LASTGOOD.PKT", "APPS/HOME.PKT"):
        (root / name).write_bytes(recovery)
    (root / "LAUNCHER.PKT").write_bytes(launcher)
    for slot in range(2):
        (root / f"STATE{slot}.BIN").write_bytes(record(
            1 - slot, SOURCE_ACTIVE, package_hash,
            SOURCE_EMBEDDED, package_hash, slot == 0))
        # Empty PFS2 v1 image: the CRC covers the 24-byte header, with its
        # own field zeroed. Remaining preallocated sectors start at zero.
        bank = bytearray(136 * 512)
        struct.pack_into("<6I", bank, 0, 0x32534650, 1, 1 - slot, 0, 24, 0)
        struct.pack_into("<I", bank, 20, zlib.crc32(bank[:24]))
        (root / f"FSBANK{slot}.BIN").write_bytes(bank)
    print(root)


if __name__ == "__main__":
    main()
