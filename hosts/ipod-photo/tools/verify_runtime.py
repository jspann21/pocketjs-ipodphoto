#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
from pathlib import Path


def symbols(path: Path, nm: str) -> dict[str, int]:
    output = subprocess.check_output([nm, "-n", str(path)], text=True)
    result: dict[str, int] = {}
    for line in output.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3:
            try:
                result[fields[2]] = int(fields[0], 16)
            except ValueError:
                pass
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--ipod", type=Path, required=True)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--backend", choices=("rust", "stub"), default="rust")
    parser.add_argument("--require-power-telemetry", action="store_true")
    parser.add_argument("--require-quickjs", action="store_true")
    args = parser.parse_args()

    binary = args.bin.read_bytes()
    wrapped = args.ipod.read_bytes()
    if len(wrapped) != len(binary) + 8 or wrapped[4:8] != b"ipco":
        raise SystemExit("invalid ipco wrapper")
    stored = struct.unpack_from(">I", wrapped, 0)[0]
    calculated = (3 + sum(binary)) & 0xFFFFFFFF
    if stored != calculated:
        raise SystemExit("ipco checksum mismatch")
    if binary[0x20:0x28] != b"Rockbox\x01":
        raise SystemExit("handoff marker missing")

    required = {
        "_start", "reset_handler", "vector_irq", "irq_dispatch",
        "timer_irq_init", "pjs_heap_init", "pjs_core_init",
        "pjs_core_backend_marker", "pjs_core_step",
        "pjs_core_render_damage", "pjs_core_needs_render",
        "lcd_present_damage", "kernel_main",
        "PJS_CORE_BACKEND_RUST" if args.backend == "rust" else "PJS_CORE_BACKEND_STUB",
    }
    if args.require_power_telemetry:
        required.update({"power_telemetry_init", "power_telemetry_sample"})
    if args.require_quickjs:
        required.update({
            "qjs_runtime_boot", "qjs_runtime_frame", "JS_NewRuntime2", "JS_Eval",
            "pjs_package_open_ipod_photo", "pjs_ui_create_node", "pjs_ui_set_prop",
            "pjs_embedded_package", "pjs_embedded_package_length",
        })
    found = symbols(args.elf, args.nm)
    missing = sorted(required - found.keys())
    if missing:
        raise SystemExit(f"missing runtime symbols: {', '.join(missing)}")
    if found["_start"] != 0:
        raise SystemExit("entry is not offset zero")

    relocations = subprocess.check_output([args.readelf, "-r", str(args.elf)], text=True)
    if "There are no relocations" not in relocations:
        raise SystemExit("final runtime ELF still contains relocations")

    undefined_output = subprocess.check_output(
        [args.nm, "-u", str(args.elf)], text=True
    ).strip()
    strong_undefined = [
        line for line in undefined_output.splitlines()
        if line.split() and line.split()[0] == "U"
    ]
    if strong_undefined:
        raise SystemExit(
            "strong undefined symbols remain:\n" + "\n".join(strong_undefined)
        )

    print("Runtime image: OK")
    print(f"  binary bytes: {len(binary)}")
    print(f"  binary SHA-256: {hashlib.sha256(binary).hexdigest()}")
    print(f"  ipod SHA-256: {hashlib.sha256(wrapped).hexdigest()}")
    print(f"  IRQ vector: 0x{found['vector_irq']:08x}")
    print(f"  PocketJS core init: 0x{found['pjs_core_init']:08x}")
    print(f"  core backend: {args.backend}")
    print(f"  power telemetry: {'required' if args.require_power_telemetry else 'optional'}")
    print(f"  QuickJS runtime: {'required' if args.require_quickjs else 'optional'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
