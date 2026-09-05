#!/usr/bin/env python3
"""Provision bounded app-storage files on an existing PocketJS disk volume."""
import argparse
import json
import os
from pathlib import Path
import struct
import shutil
import zlib


def provision(volume: Path) -> dict:
    root = volume.resolve() / "POCKETJS"
    if not root.is_dir() or not (root / "LAUNCHER.PKT").is_file():
        raise ValueError("expected an existing PocketJS volume with POCKETJS/LAUNCHER.PKT")
    with (root / "LAUNCHER.PKT").open("rb") as source:
        if source.read(4) != b"PCKT":
            raise ValueError("launcher package signature is invalid")
    files = {f"APP{slot:02d}{bank}.BIN": 4 * 1024 * 1024
             for slot in range(8) for bank in "AB"}
    files.update({"APPIDX0.BIN": 2048, "APPIDX1.BIN": 2048})
    # Refuse existing truncated files before creating anything. Existing app
    # packages/catalogs are retained byte-for-byte when setup is run again.
    for name, size in files.items():
        path = root / name
        if path.exists() and (not path.is_file() or path.stat().st_size != size):
            raise ValueError(f"unexpected existing file size: {path}")
    indexes = [root / f"APPIDX{i}.BIN" for i in range(2)]
    fresh = not any(p.exists() for p in indexes)
    if not fresh and not all(p.exists() for p in indexes):
        raise ValueError("incomplete app catalog pair; preserve it for recovery")
    if not fresh:
        valid = False
        for path in indexes:
            image = path.read_bytes()
            fields = struct.unpack_from("<8I", image)
            length = fields[3]
            valid |= (fields[0:2] == (0x50534a50, 1) and
                      fields[5:8] == (0x58444941, 1, 0x4d4d4f43) and length == 1256 and
                      struct.unpack_from("<I", image, 508)[0] == zlib.crc32(image[:508]) and
                      fields[4] == zlib.crc32(image[512:512 + length]) and
                      struct.unpack_from("<II", image, 512) == (0x3141504d, 1))
        if not valid:
            raise ValueError("no valid existing app catalog; preserve files for recovery")
    needed = sum(size for name, size in files.items() if not (root / name).exists())
    if shutil.disk_usage(root).free < needed + 1024 * 1024:
        raise OSError("not enough free space for the app store")
    created = []
    for name, size in files.items():
        path = root / name
        if path.exists():
            continue
        with path.open("xb") as target:
            # Explicit writes allocate FAT clusters without sparse extents.
            block = bytes(min(size, 65536))
            for offset in range(0, size, len(block)):
                target.write(block[:min(len(block), size - offset)])
            target.flush()
            os.fsync(target.fileno())
        created.append(name)
    if fresh:
        payload = struct.pack("<II", 0x3141504d, 1) + bytes(8 * 156)
        header = bytearray(512)
        struct.pack_into("<8I", header, 0, 0x50534a50, 1, 1, len(payload),
                         zlib.crc32(payload), 0x58444941, 1, 0x4d4d4f43)
        struct.pack_into("<I", header, 508, zlib.crc32(header[:508]))
        image = bytes(header) + payload + bytes(2048 - 512 - len(payload))
        with indexes[0].open("r+b") as target:
            target.write(image)
            target.flush()
            os.fsync(target.fileno())
        if indexes[0].read_bytes() != image:
            raise OSError("catalog readback mismatch")
    return {"status": "ready", "root": str(root), "slots": 8,
            "package_limit": 4 * 1024 * 1024 - 512, "created": created}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--volume", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(provision(args.volume), indent=2))
