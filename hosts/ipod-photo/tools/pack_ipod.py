#!/usr/bin/env python3
"""Wrap a flat A1099 image in the standard 8-byte `ipco` transport header."""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import tempfile
from pathlib import Path

MODEL = b"ipco"
CHECKSUM_SEED = 3


def checksum(image: bytes) -> int:
    return (CHECKSUM_SEED + sum(image)) & 0xFFFFFFFF


def encode(image: bytes) -> bytes:
    if len(image) < 32:
        raise ValueError("firmware image is too small")
    if len(image) > 32 * 1024 * 1024:
        raise ValueError("firmware image exceeds the A1099 SDRAM ceiling")
    if len(image) & 3:
        raise ValueError("firmware image length must be 4-byte aligned")
    return struct.pack(">I4s", checksum(image), MODEL) + image


def decode(payload: bytes) -> bytes:
    if len(payload) < 8:
        raise ValueError(".ipod payload is truncated")
    declared, model = struct.unpack(">I4s", payload[:8])
    if model != MODEL:
        raise ValueError(f"model is {model!r}, expected {MODEL!r}")
    image = payload[8:]
    if len(image) & 3:
        raise ValueError("firmware image length is not 4-byte aligned")
    actual = checksum(image)
    if actual != declared:
        raise ValueError(f"checksum is 0x{declared:08x}, expected 0x{actual:08x}")
    return image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    image = args.image.read_bytes()
    payload = encode(image)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{args.output.name}.tmp-", dir=args.output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        verified_payload = temporary.read_bytes()
        if verified_payload != payload or decode(verified_payload) != image:
            raise SystemExit("temporary output failed read-back verification")
        os.replace(temporary, args.output)
        try:
            directory = os.open(args.output.parent, os.O_RDONLY)
        except OSError:
            directory = -1
        if directory >= 0:
            try:
                os.fsync(directory)
            except OSError:
                pass
            finally:
                os.close(directory)
    finally:
        if temporary.exists():
            temporary.unlink()
    verified = decode(args.output.read_bytes())
    if verified != image:
        raise SystemExit("published output failed read-back verification")
    print(f"wrote {args.output} ({len(image)} image bytes, checksum 0x{checksum(image):08x})")
    print(f"sha256 {hashlib.sha256(payload).hexdigest()}")


if __name__ == "__main__":
    main()
