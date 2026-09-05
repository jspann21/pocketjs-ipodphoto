#!/usr/bin/env python3
"""Static admission checks for the flat and `.ipod` A1099 probe images."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from pack_ipod import decode


def arm_branch(word: int) -> bool:
    return (word & 0x0E000000) == 0x0A000000 and (word >> 28) != 0xF


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    args = parser.parse_args()
    payload = args.file.read_bytes()
    image = decode(payload) if args.file.suffix == ".ipod" else payload
    if len(image) < 32:
        raise SystemExit("FAIL: image does not contain a complete vector table")
    first = struct.unpack_from("<I", image)[0]
    if not arm_branch(first):
        raise SystemExit(f"FAIL: first word 0x{first:08x} is not an ARM B/BL")
    if image[:32].count(b"\x00") == 32:
        raise SystemExit("FAIL: vector table is blank")
    print(f"OK {args.file}")
    print(f"image bytes: {len(image)}")
    print(f"first word: 0x{first:08x}")
    print(f"image sha256: {hashlib.sha256(image).hexdigest()}")
    print(f"container sha256: {hashlib.sha256(payload).hexdigest()}")


if __name__ == "__main__":
    main()
