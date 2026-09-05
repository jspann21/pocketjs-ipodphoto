#!/usr/bin/env python3
"""Atomically stage/restore the PocketJS image on a mounted iPod data volume.

This tool never opens a raw disk or firmware partition. It only replaces the
bootloader's ordinary ``rockbox.ipod`` file after creating and verifying an
exact backup on the mounted FAT32 volume.

The path supplied with ``--mount`` may be either the iPod volume root or its
``.rockbox`` directory. Interrupted transactions are recovered only when every
available hash proves that the active image is still the recorded original.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path
from typing import Any

from pack_ipod import decode

TARGET_NAME = "rockbox.ipod"
BACKUP_NAME = "rockbox.ipod.pocketjs-backup"
STATE_NAME = ".pocketjs-a1099-handoff.json"
STATE_TEMP_NAME = f"{STATE_NAME}.new"
LEGACY_STATE_TEMP_NAME = f".{STATE_NAME}.new"  # Phase-0 v1 used a double dot.


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def sync_directory(path: Path) -> None:
    """Best-effort directory sync.

    Windows does not generally permit opening a directory with ``os.open``;
    FAT removables also vary in their support. File contents are always fsynced
    before replacement, while a directory sync is attempted where available.
    """

    try:
        descriptor = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def transaction_directory(path: Path) -> Path:
    """Accept either a volume root or the volume's .rockbox directory."""

    direct_markers = (path / TARGET_NAME, path / BACKUP_NAME, path / STATE_NAME)
    nested = path / ".rockbox"
    nested_markers = (
        nested / TARGET_NAME,
        nested / BACKUP_NAME,
        nested / STATE_NAME,
    )

    if any(marker.exists() for marker in direct_markers):
        return path
    if any(marker.exists() for marker in nested_markers):
        return nested
    return path


def temporary_state_paths(directory: Path) -> tuple[Path, ...]:
    return (
        directory / STATE_TEMP_NAME,
        directory / LEGACY_STATE_TEMP_NAME,
    )


def remove_if_exists(path: Path) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def load_state(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read transaction state {path}: {error}") from error
    if not isinstance(value, dict):
        raise SystemExit(f"transaction state is not a JSON object: {path}")
    return value


def atomic_write_json(destination: Path, value: dict[str, Any]) -> None:
    """Write JSON through a writable, fsynced temporary file and replace."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.tmp-", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as writer:
            json.dump(value, writer, indent=2)
            writer.write("\n")
            writer.flush()
            # Windows requires a writable file descriptor for fsync.
            os.fsync(writer.fileno())
        os.replace(temporary, destination)
        sync_directory(destination.parent)
    finally:
        remove_if_exists(temporary)


def atomic_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.tmp-", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with source.open("rb") as reader, os.fdopen(descriptor, "wb") as writer:
            shutil.copyfileobj(reader, writer, length=1024 * 1024)
            writer.flush()
            os.fsync(writer.fileno())
        try:
            os.chmod(temporary, 0o644)
        except OSError:
            # Permission bits are not meaningful on some FAT mounts.
            pass
        if sha256_file(temporary) != sha256_file(source):
            raise RuntimeError(f"read-back verification failed for {destination}")
        os.replace(temporary, destination)
        sync_directory(destination.parent)
    finally:
        remove_if_exists(temporary)


def validate_probe(path: Path) -> dict[str, object]:
    payload = path.read_bytes()
    image = decode(payload)
    if image[0x20:0x28] != b"Rockbox\x01":
        raise ValueError("probe lacks the bootloader compatibility tag at offset 0x20")
    return {
        "path": str(path),
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "imageBytes": len(image),
        "imageSha256": hashlib.sha256(image).hexdigest(),
    }


def clean_transaction_files(directory: Path, *, remove_backup: bool) -> None:
    if remove_backup:
        remove_if_exists(directory / BACKUP_NAME)
    remove_if_exists(directory / STATE_NAME)
    for temporary in temporary_state_paths(directory):
        remove_if_exists(temporary)
    sync_directory(directory)


def recover_before_install(directory: Path, probe_sha256: str) -> bool:
    """Recover a provably safe interrupted install.

    Returns ``True`` when the requested probe is already installed. Anything
    ambiguous is rejected rather than guessed at.
    """

    target = directory / TARGET_NAME
    backup = directory / BACKUP_NAME
    state_path = directory / STATE_NAME
    stale_temps = [path for path in temporary_state_paths(directory) if path.exists()]

    if not backup.exists() and not state_path.exists() and not stale_temps:
        return False
    if not target.is_file():
        raise SystemExit(
            "an interrupted transaction exists but rockbox.ipod is missing; "
            "do not remove the backup—run status and inspect the volume"
        )

    target_hash = sha256_file(target)
    backup_hash = sha256_file(backup) if backup.is_file() else None

    if state_path.is_file():
        state = load_state(state_path)
        original_hash = state.get("original", {}).get("sha256")
        recorded_probe_hash = state.get("probe", {}).get("sha256")
        if not isinstance(original_hash, str) or not isinstance(recorded_probe_hash, str):
            raise SystemExit("transaction state lacks valid original/probe hashes")
        if backup_hash != original_hash:
            raise SystemExit(
                "existing backup does not match the transaction's recorded original; "
                "refusing automatic recovery"
            )
        if target_hash == recorded_probe_hash:
            if recorded_probe_hash != probe_sha256:
                raise SystemExit(
                    "a different probe is already installed; restore it before staging another"
                )
            print("requested probe is already installed and verified")
            return True
        if target_hash == original_hash:
            print("recovering an interrupted install; active image is still the original")
            clean_transaction_files(directory, remove_backup=True)
            return False
        raise SystemExit(
            "active rockbox.ipod matches neither the recorded original nor probe; "
            "refusing automatic recovery"
        )

    # The Phase-0 Windows fsync bug failed after creating the exact backup but
    # before publishing transaction state or replacing the active image.
    if backup_hash is not None and target_hash == backup_hash:
        print("recovering an interrupted pre-install; original image is unchanged")
        clean_transaction_files(directory, remove_backup=True)
        return False

    raise SystemExit(
        "incomplete backup/state files exist and their hashes do not prove a safe "
        "pre-install state; refusing automatic recovery"
    )


def install(mount: Path, probe: Path) -> None:
    directory = transaction_directory(mount)
    target = directory / TARGET_NAME
    backup = directory / BACKUP_NAME
    state_path = directory / STATE_NAME

    if not directory.is_dir():
        raise SystemExit(f"mount/directory does not exist: {directory}")
    if not target.is_file() or target.is_symlink():
        raise SystemExit(
            f"expected an existing regular {target}; pass either the iPod drive root "
            "or its .rockbox directory"
        )

    probe_fact = validate_probe(probe)
    if recover_before_install(directory, str(probe_fact["sha256"])):
        return

    original_payload = target.read_bytes()
    try:
        original_image = decode(original_payload)
    except ValueError as error:
        raise SystemExit(f"existing {target} is not a valid ipco image: {error}") from error
    original_fact = {
        "bytes": len(original_payload),
        "sha256": hashlib.sha256(original_payload).hexdigest(),
        "imageBytes": len(original_image),
        "imageSha256": hashlib.sha256(original_image).hexdigest(),
    }

    atomic_copy(target, backup)
    if sha256_file(backup) != original_fact["sha256"]:
        raise SystemExit("backup verification failed")

    state = {
        "version": 2,
        "directory": str(directory),
        "target": TARGET_NAME,
        "backup": BACKUP_NAME,
        "original": original_fact,
        "probe": probe_fact,
    }
    atomic_write_json(state_path, state)

    try:
        atomic_copy(probe, target)
        if sha256_file(target) != probe_fact["sha256"]:
            raise RuntimeError("probe read-back verification failed")
    except Exception:
        # Roll back completely if staging fails. Preserve ambiguous evidence only
        # if the rollback itself fails.
        atomic_copy(backup, target)
        if sha256_file(target) == original_fact["sha256"]:
            clean_transaction_files(directory, remove_backup=True)
        raise

    print(f"staged {probe} as {target}")
    print(f"original backup: {backup}")
    print(f"state: {state_path}")


def restore(mount: Path) -> None:
    directory = transaction_directory(mount)
    target = directory / TARGET_NAME
    backup = directory / BACKUP_NAME
    state_path = directory / STATE_NAME

    if backup.is_file() and not state_path.is_file() and target.is_file():
        # Recover the exact pre-install failure produced by Phase-0 v1.
        if sha256_file(backup) == sha256_file(target):
            clean_transaction_files(directory, remove_backup=True)
            print("cleaned interrupted pre-install; original rockbox.ipod was unchanged")
            return

    if not backup.is_file() or not state_path.is_file():
        raise SystemExit("backup/state pair is missing")
    if not target.is_file() or target.is_symlink():
        raise SystemExit(
            "active rockbox.ipod is missing or is not a regular file; "
            "refusing to overwrite it"
        )
    state = load_state(state_path)
    expected = state.get("original", {}).get("sha256")
    expected_probe = state.get("probe", {}).get("sha256")
    actual = sha256_file(backup)
    if not isinstance(expected, str) or not isinstance(expected_probe, str):
        raise SystemExit("transaction state lacks valid original/probe hashes")
    if actual != expected:
        raise SystemExit("backup hash does not match the recorded original")
    target_hash = sha256_file(target)
    if target_hash == expected:
        clean_transaction_files(directory, remove_backup=True)
        print("active rockbox.ipod was already restored; cleaned transaction files")
        return
    if target_hash != expected_probe:
        raise SystemExit(
            "active rockbox.ipod matches neither the recorded original nor probe; "
            "refusing to overwrite an unexpected file"
        )
    atomic_copy(backup, target)
    if sha256_file(target) != expected:
        raise SystemExit("restored target failed read-back verification")
    clean_transaction_files(directory, remove_backup=True)
    print(f"restored {target}")


def status(mount: Path) -> None:
    directory = transaction_directory(mount)
    target = directory / TARGET_NAME
    backup = directory / BACKUP_NAME
    state_path = directory / STATE_NAME
    stale_temps = [str(path) for path in temporary_state_paths(directory) if path.exists()]
    result = {
        "requestedPath": str(mount),
        "directory": str(directory),
        "targetExists": target.is_file(),
        "targetSha256": sha256_file(target) if target.is_file() else None,
        "backupExists": backup.is_file(),
        "backupSha256": sha256_file(backup) if backup.is_file() else None,
        "state": load_state(state_path) if state_path.is_file() else None,
        "staleStateFiles": stale_temps,
    }
    print(json.dumps(result, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    install_parser = subparsers.add_parser("install")
    install_parser.add_argument("--mount", required=True, type=Path)
    install_parser.add_argument("--probe", required=True, type=Path)
    restore_parser = subparsers.add_parser("restore")
    restore_parser.add_argument("--mount", required=True, type=Path)
    status_parser = subparsers.add_parser("status")
    status_parser.add_argument("--mount", required=True, type=Path)
    args = parser.parse_args()

    mount = args.mount.resolve()
    if args.command == "install":
        install(mount, args.probe.resolve())
    elif args.command == "restore":
        restore(mount)
    else:
        status(mount)


if __name__ == "__main__":
    main()
