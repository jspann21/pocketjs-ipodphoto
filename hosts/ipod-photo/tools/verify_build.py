#!/usr/bin/env python3
"""Fail closed on the link, boot, and transport invariants of the A1099 probe."""

from __future__ import annotations

import argparse
import struct
import subprocess
from pathlib import Path

from pack_ipod import decode

SDRAM_BYTES = 32 * 1024 * 1024
HANDOFF_RESET = 0x100
DIRECT_RESET = 0x20


def run(*command: str) -> str:
    completed = subprocess.run(command, check=True, text=True, capture_output=True, timeout=15)
    return completed.stdout + completed.stderr


def symbols_from_nm(output: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            symbols[fields[-1]] = address
    return symbols


def branch_target(address: int, word: int) -> int:
    if (word & 0x0E000000) != 0x0A000000 or (word >> 28) == 0xF:
        raise ValueError(f"0x{word:08x} is not an ARM B/BL")
    displacement = (word & 0x00FFFFFF) << 2
    if displacement & 0x02000000:
        displacement -= 0x04000000
    return (address + 8 + displacement) & 0xFFFFFFFF


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--ipod", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--require-handoff-signature", action="store_true")
    parser.add_argument("--kernel-symbol", default="kernel_main")
    parser.add_argument("--max-image-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()

    image = args.bin.read_bytes()
    wrapped = args.ipod.read_bytes()
    if decode(wrapped) != image:
        raise SystemExit("FAIL: .ipod body differs from the flat image")
    if len(image) < 32:
        raise SystemExit("FAIL: vector table is truncated")
    if args.max_image_bytes <= 0:
        raise SystemExit("FAIL: maximum image size must be positive")
    if len(image) > args.max_image_bytes:
        raise SystemExit(
            f"FAIL: image is {len(image)} bytes; maximum is {args.max_image_bytes}"
        )
    if len(image) & 3:
        raise SystemExit("FAIL: flat image is not word aligned")

    handoff_tag = image[0x20:0x28]
    if args.require_handoff_signature:
        if handoff_tag != b"Rockbox\x01":
            raise SystemExit("FAIL: bootloader handoff signature is missing at offset 0x20")
    elif handoff_tag == b"Rockbox\x01":
        raise SystemExit("FAIL: direct image unexpectedly contains the handoff signature")

    relocations = run(args.readelf, "-r", str(args.elf))
    if "There are no relocations in this file." not in relocations:
        raise SystemExit("FAIL: final ELF still contains relocations")
    undefined_output = run(args.nm, "-u", str(args.elf)).strip()
    strong_undefined = [
        line for line in undefined_output.splitlines()
        if line.split() and line.split()[0] == "U"
    ]
    if strong_undefined:
        raise SystemExit(
            "FAIL: strong undefined symbols remain:\n" + "\n".join(strong_undefined)
        )

    nm_output = run(args.nm, "-n", str(args.elf))
    symbols = symbols_from_nm(nm_output)
    required = (
        "_start",
        "reset_handler",
        "cache_clean_wait",
        "cache_disable",
        "remap_stub_start",
        "remap_target",
        "remap_stub_end",
        "post_remap",
        args.kernel_symbol,
        "__image_end",
        "__bss_start",
        "__bss_end",
        "__stack_top",
        "__heap_start",
        "__ram_end",
    )
    missing = [symbol for symbol in required if symbol not in symbols]
    if missing:
        raise SystemExit(f"FAIL: ELF symbol table is missing {', '.join(missing)}")

    if symbols["_start"] != 0:
        raise SystemExit("FAIL: image entry is not at offset zero")
    expected_reset = HANDOFF_RESET if args.require_handoff_signature else DIRECT_RESET
    if symbols["reset_handler"] != expected_reset:
        raise SystemExit(
            f"FAIL: reset handler is 0x{symbols['reset_handler']:x}, expected 0x{expected_reset:x}"
        )
    if symbols["__image_end"] != len(image):
        raise SystemExit(
            f"FAIL: __image_end is 0x{symbols['__image_end']:x}, flat length is 0x{len(image):x}"
        )
    if symbols["__bss_start"] < symbols["__image_end"]:
        raise SystemExit("FAIL: BSS overlaps stored image bytes")
    if not (symbols["__bss_start"] <= symbols["__bss_end"] <= symbols["__stack_top"]):
        raise SystemExit("FAIL: BSS/stack symbols are not monotonically ordered")
    if not (symbols["__stack_top"] <= symbols["__heap_start"] < symbols["__ram_end"]):
        raise SystemExit("FAIL: stack/heap/RAM symbols are not monotonically ordered")
    if symbols["__ram_end"] != SDRAM_BYTES:
        raise SystemExit("FAIL: linker RAM ceiling is not 32 MiB")
    if not (
        symbols["reset_handler"]
        < symbols["cache_clean_wait"]
        < symbols["cache_disable"]
        < symbols["remap_stub_start"]
    ):
        raise SystemExit("FAIL: cache-clean/disable/remap bootstrap order is invalid")

    stub_start = symbols["remap_stub_start"]
    stub_end = symbols["remap_stub_end"]
    target_word = symbols["remap_target"]
    post_remap = symbols["post_remap"]
    stub_length = stub_end - stub_start
    if stub_length <= 0 or stub_length > 64 or (stub_length & 3):
        raise SystemExit(f"FAIL: remap stub length is invalid: {stub_length}")
    if not (stub_start <= target_word <= stub_end - 4):
        raise SystemExit("FAIL: remap target literal lies outside the copied IRAM stub")
    encoded_target = struct.unpack_from("<I", image, target_word)[0]
    if encoded_target != post_remap:
        raise SystemExit(
            f"FAIL: copied remap target is 0x{encoded_target:08x}, expected 0x{post_remap:08x}"
        )

    words = struct.unpack_from("<8I", image)
    vector_symbols = (
        "reset_handler",
        "vector_undef",
        "vector_swi",
        "vector_prefetch_abort",
        "vector_data_abort",
        "vector_reserved",
        "vector_irq",
        "vector_fiq",
    )
    for index, (word, symbol) in enumerate(zip(words, vector_symbols)):
        if symbol not in symbols:
            raise SystemExit(f"FAIL: ELF symbol table is missing {symbol}")
        try:
            target = branch_target(index * 4, word)
        except ValueError as error:
            raise SystemExit(f"FAIL: vector {index}: {error}") from error
        if target != symbols[symbol]:
            raise SystemExit(
                f"FAIL: vector {index} targets 0x{target:08x}, {symbol} is 0x{symbols[symbol]:08x}"
            )

    link_map = args.map.read_text()
    for symbol in required:
        if symbol not in link_map:
            raise SystemExit(f"FAIL: link map is missing {symbol}")

    print("build verifier: OK")
    print(f"vectors: {[f'0x{word:08x}' for word in words]}")
    print(f"reset: 0x{symbols['reset_handler']:x}")
    print(
        f"cache: wait 0x{symbols['cache_clean_wait']:x}, "
        f"disable 0x{symbols['cache_disable']:x}"
    )
    print(f"remap stub: 0x{stub_start:x}..0x{stub_end:x} ({stub_length} bytes)")
    print(f"flat bytes: {len(image)}")
    print(f"ipod bytes: {len(wrapped)}")


if __name__ == "__main__":
    main()
