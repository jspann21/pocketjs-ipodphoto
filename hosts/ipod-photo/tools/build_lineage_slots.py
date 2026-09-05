#!/usr/bin/env python3
"""Build fixed lifecycle-lineage seed slots for the runtime."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

SECTOR_BYTES = 512
MAGIC = b"PJSLIFE2"
COMMITTED = 0x434F4D54
PHASE_ACTIVE = 1
SOURCE_ACTIVE = 2
SOURCE_EMBEDDED = 5


def record(
    generation: int,
    active_source: int,
    active_hash: int,
    last_good_source: int,
    last_good_hash: int,
    committed: bool,
) -> bytes:
    output = bytearray(SECTOR_BYTES)
    output[:8] = MAGIC
    struct.pack_into("<IIII", output, 8, 2, generation,
                     COMMITTED if committed else 0, PHASE_ACTIVE)
    struct.pack_into("<III", output, 24, active_source,
                     active_hash & 0xFFFFFFFF, active_hash >> 32)
    struct.pack_into("<III", output, 36, last_good_source,
                     last_good_hash & 0xFFFFFFFF, last_good_hash >> 32)
    struct.pack_into("<I", output, SECTOR_BYTES - 4,
                     zlib.crc32(output[:-4]) & 0xFFFFFFFF)
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--active-hash", required=True,
                        type=lambda value: int(value, 0))
    parser.add_argument("--active-source", default=SOURCE_ACTIVE, type=int)
    parser.add_argument("--last-good-source", default=SOURCE_EMBEDDED,
                        type=int)
    parser.add_argument("--last-good-hash", type=lambda value: int(value, 0))
    args = parser.parse_args()
    last_good_hash = (args.active_hash if args.last_good_hash is None
                      else args.last_good_hash)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "STATE0.BIN").write_bytes(
        record(1, args.active_source, args.active_hash,
               args.last_good_source, last_good_hash, True))
    (args.output / "STATE1.BIN").write_bytes(
        record(0, args.active_source, args.active_hash,
               args.last_good_source, last_good_hash, False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
