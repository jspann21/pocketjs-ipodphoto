#!/usr/bin/env python3
"""Host workflow for the PocketJS iPod Photo PJSU USB service.

JSON is presentation-only. The serial wire is the 20-byte binary frame from
``include/usb_protocol.h``; package/image bytes are sent raw in DATA frames.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
import math
import os
import re
import struct
import time
from pathlib import Path
from typing import Any, Dict, Iterable, Optional

try:
    from ipod_usb_protocol import (
        CHAINLOAD, CHAINLOAD_REPLY, Frame, IMAGE_BEGIN, IMAGE_DATA, IMAGE_END,
        IMAGE_REPLY, LOG, PROMPT, ProtocolError, RESULT, RUN, SerialTransport,
        STOP, TransportError, UPLOAD_BEGIN, UPLOAD_DATA, UPLOAD_END, UPLOAD_REPLY,
        crc32, discover_ports, status_payload, u32, STATE_NAMES, DEFAULT_BAUD,
    )
    from pack_ipod import decode as decode_ipod
except ImportError:  # pragma: no cover - permits package imports
    from .ipod_usb_protocol import (  # type: ignore
        CHAINLOAD, CHAINLOAD_REPLY, Frame, IMAGE_BEGIN, IMAGE_DATA, IMAGE_END,
        IMAGE_REPLY, LOG, PROMPT, ProtocolError, RESULT, RUN, SerialTransport,
        STOP, TransportError, UPLOAD_BEGIN, UPLOAD_DATA, UPLOAD_END, UPLOAD_REPLY,
        crc32, discover_ports, status_payload, u32, STATE_NAMES, DEFAULT_BAUD,
    )
    from .pack_ipod import decode as decode_ipod  # type: ignore


class PromptRequired(RuntimeError):
    pass


APPS_LIST, APP_INSTALL, APP_REMOVE, APP_LOAD = 0x27, 0x28, 0x29, 0x2a


def app_name(value: str) -> bytes:
    name = value.upper().removesuffix(".PKT")
    if not re.fullmatch(r"[A-Z0-9_-]{1,8}", name):
        raise ValueError("app name must contain 1–8 letters, digits, underscores or hyphens")
    return name.ljust(8).encode("ascii") + b"PKT"


def _hex(value: int) -> str:
    return f"0x{value:08x}"


def _state(value: int) -> str:
    return STATE_NAMES.get(value, f"state_{value}")


class IpodRunner:
    def __init__(self, args: argparse.Namespace, transport: Optional[SerialTransport] = None) -> None:  # type: ignore[name-defined]
        self.args = args
        self.transport = transport
        self.answers = dict(item.split("=", 1) for item in (args.answer or []) if "=" in item)
        self.batch_log = None
        self.last_cycle_result: Optional[Dict[str, Any]] = None

    def emit(self, record: Dict[str, Any]) -> None:
        if self.batch_log is not None:
            self.batch_log.write(json.dumps(record, separators=(",", ":"), ensure_ascii=True) + "\n")
            self.batch_log.flush()
        if self.args.json:
            print(json.dumps(record, separators=(",", ":"), ensure_ascii=True), flush=True)
            return
        kind = record.get("type", "message")
        if kind == "log":
            print(f"[{record.get('level', 'info')}] {record.get('message', '')}", flush=True)
        elif kind == "prompt":
            print(f"ACTION REQUIRED: {record.get('message', '')}", flush=True)
        elif kind == "result":
            print(f"RESULT {record.get('status', 'unknown')}: {record}", flush=True)
        elif kind == "upload":
            print(f"upload {record['bytes']}/{record['size']} ({record['percent']:.1f}%)", end="\r", flush=True)
        else:
            print(json.dumps(record, indent=2, sort_keys=True), flush=True)

    def on_frame(self, frame: Frame) -> None:  # type: ignore[name-defined]
        if frame.msg_type == LOG:
            if not frame.payload:
                raise ProtocolError("LOG event is missing level")
            self.emit({"type": "log", "level": frame.payload[0], "message": frame.payload[1:].decode("utf-8", "replace")})
        elif frame.msg_type == PROMPT:
            if not frame.payload:
                raise ProtocolError("PROMPT event is missing kind")
            event = {"type": "prompt", "kind": frame.payload[0], "message": frame.payload[1:].decode("utf-8", "replace")}
            self.emit(event)
            token = f"kind-{frame.payload[0]}"
            answer = self.answers.get(token)
            if answer is None and self.args.noninteractive:
                raise PromptRequired(f"device requested prompt {token!r}; provide --answer {token}=VALUE")
            if answer is None:
                answer = input("Answer (or 'skip'): ").strip()
            # The current device protocol has no ANSWER message type. Prompt
            # observations are recorded by the host; the device continues its
            # own test schedule after emitting the asynchronous prompt.
            self.emit({"type": "answer", "kind": frame.payload[0], "answer": answer})
        elif frame.msg_type == RESULT:
            if len(frame.payload) < 12:
                raise ProtocolError("RESULT event is missing fields")
            code, value, count = struct.unpack_from("<IiI", frame.payload)
            self.emit({"type": "result", "status": "pass" if code == 0 else "fail", "result_code": code, "value": value, "frame_count": count})

    def connect(self) -> SerialTransport:  # type: ignore[name-defined]
        if self.transport is None:
            port = resolve_port(self.args.port, self.args.baud, self.args.timeout)
            self.transport = SerialTransport(port, self.args.baud, self.args.timeout, self.args.retries, self.on_frame)
        else:
            self.transport.event_handler = self.on_frame
        self.transport.open()
        hello = self.transport.hello(
            self.args.handshake_timeout,
            retries=max(0, self.args.hello_retries),
            retry_wait=max(0.0, self.args.hello_retry_wait),
        )
        self.emit({"type": "connected", "port": self.transport.port, "hello": {**hello, "state": _state(hello["state"])}})
        return self.transport

    def reconnect(self) -> None:
        assert self.transport is not None
        self.emit({"type": "reconnect", "port": self.transport.port})
        self.transport.reconnect()
        self.connect()

    def finish_info(self) -> None:
        """Release the Windows read boundary after a complete INFO reply."""
        assert self.transport is not None
        # Reopening the host handle leaves the guest and device state intact.
        # Apply this to ordinary cycles too, not only the fixed batch command.
        self.transport.close()
        self.connect()

    def wait_for_runtime(self, timeout: float) -> Dict[str, Any]:
        """Wait for an admitted RUN without flooding requests during boot."""

        assert self.transport is not None
        deadline = time.monotonic() + timeout
        last_state: Optional[int] = None
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                info = self.transport.info(min(3.0, max(0.05, remaining)))
            except TimeoutError:
                time.sleep(min(0.25, max(0.0, deadline - time.monotonic())))
                continue
            except TransportError:
                if getattr(self.args, "batch_mode", False):
                    raise TransportError("batch stopped: runtime INFO transport became ambiguous")
                self.transport.reconnect(wait=0.25)
                continue
            last_state = info["state"]
            if last_state == 3:
                return info
            if last_state == 4:
                message = info.get("runtime_error_text", "")
                suffix = f" message={message!r}" if message else ""
                raise ProtocolError(
                    f"runtime boot failed: {_hex(info['runtime_error'])}{suffix}"
                )
            if last_state not in (2, 5):
                raise ProtocolError(f"runtime left boot flow in {_state(last_state)}")
            self.finish_info()
            time.sleep(min(0.25, max(0.0, deadline - time.monotonic())))
        state = _state(last_state) if last_state is not None else "unresponsive"
        raise TimeoutError(f"runtime did not finish booting within {timeout:.1f}s (last state: {state})")

    def request(self, msg_type: int, payload: bytes = b"", expected: Optional[int] = None, timeout: float = 5.0) -> Frame:  # type: ignore[name-defined]
        assert self.transport is not None
        return self.transport.request(msg_type, payload, timeout, expected)

    def list_apps(self) -> list[Dict[str, Any]]:
        _, data = status_payload(self.request(APPS_LIST, expected=0xa7, timeout=60.0))
        if len(data) < 8 or len(data) != 8 + u32(data) * 28:
            raise ProtocolError("invalid app catalog reply")
        apps = []
        for index in range(u32(data)):
            entry = data[8 + index * 28:36 + index * 28]
            apps.append({"name": entry[:8].decode("ascii").rstrip(),
                         "managed": bool(entry[11]), "bytes": u32(entry, 12),
                         "slot": u32(entry, 16) if entry[11] else None,
                         "hash_low": u32(entry, 20), "hash_high": u32(entry, 24)})
        self.emit({"type": "apps", "managed_capacity": u32(data, 4), "apps": apps})
        return apps

    def manage_app(self, command: str) -> None:
        assert self.transport is not None
        name = app_name(self.args.name)
        if self.transport.package_capacity < 4 * 1024 * 1024:
            self.enter_maintenance()
        self.request(STOP, timeout=10.0)
        if command == "install":
            self.upload_package(Path(self.args.package))
            expected = self.transport.info(self.args.handshake_timeout)
            self.request(APP_INSTALL, name, 0xa8, timeout=120.0)
            apps = self.list_apps()
            if not any(a["name"] == name[:8].decode().rstrip() and a["managed"] and
                       a["hash_low"] == expected["package_hash_low"] and
                       a["hash_high"] == expected["package_hash_high"] for a in apps):
                raise ProtocolError("installed package identity missing from catalog")
        elif command == "remove":
            self.request(APP_REMOVE, name, 0xa9, timeout=120.0)
            if any(a["name"] == name[:8].decode().rstrip() for a in self.list_apps()):
                raise ProtocolError("removed app is still listed")
        if command == "launch" or (command == "install" and self.args.launch):
            # Read the durable package back through the firmware's app loader.
            self.request(APP_LOAD, name, 0xaa, timeout=120.0)
            admitted = self.transport.info(self.args.handshake_timeout)
            self.request(RUN, timeout=5.0)
            info = self.wait_for_runtime(max(5.0, self.args.boot_timeout))
            if any(info[key] != admitted[key] for key in
                   ("package_length", "package_crc", "package_hash_low", "package_hash_high")):
                raise ProtocolError("launched package identity changed")
            self.emit({"type": "app", "operation": "launch", "name": self.args.name,
                       "status": "running", "info": info})
        else:
            self.reboot()
        self.emit({"type": "app", "operation": command, "name": self.args.name, "status": "pass"})

    def upload_bytes(self, payload: bytes, begin_type: int = UPLOAD_BEGIN, data_type: int = UPLOAD_DATA,
                     end_type: int = UPLOAD_END, begin_reply: int = UPLOAD_REPLY,
                     data_reply: int = UPLOAD_REPLY, end_reply: int = UPLOAD_REPLY) -> None:
        if not payload:
            raise ValueError("upload/image payload is empty")
        # A transport failure invalidates the contiguous DATA offset. Restart
        # the transaction from zero after a fresh sequence/re-handshake.
        for attempt in range(2):
            end_started = False
            try:
                self.request(begin_type, struct.pack("<II", len(payload), crc32(payload)), begin_reply, self.args.timeout * 3)
                offset = 0
                next_progress = 5.0
                chunk_limit = max(1, self.transport.max_payload - 4)
                if self.args.chunk_size:
                    chunk_limit = min(chunk_limit, self.args.chunk_size)
                if attempt:
                    chunk_limit = min(chunk_limit, 1024)
                while offset < len(payload):
                    chunk = payload[offset : offset + chunk_limit]
                    response = self.request(data_type, struct.pack("<I", offset) + chunk, data_reply, max(5.0, self.args.timeout * 3))
                    _, extra = status_payload(response)
                    acknowledged = u32(extra) if len(extra) >= 4 else offset + len(chunk)
                    if acknowledged != offset + len(chunk):
                        raise ProtocolError(f"device acknowledged offset {acknowledged}, expected {offset + len(chunk)}")
                    offset = acknowledged
                    percent = 100 * offset / len(payload)
                    if percent >= next_progress or offset == len(payload):
                        self.emit({"type": "upload", "bytes": offset, "size": len(payload), "percent": percent})
                        while next_progress <= percent:
                            next_progress += 5.0
                # END computes a CRC over the complete upload on the device.
                # The cache-safe A1099 service is substantially slower than
                # the nominal 80 MHz core rate: hardware measured an uncached
                # 1 MiB native image beyond the former 15 second estimate.
                # Size this for 16 KiB/s with a bounded floor/ceiling; retrying
                # after a completed END would incorrectly restart an already-
                # valid transaction.
                end_timeout = max(
                    self.args.timeout * 3,
                    min(120.0, max(15.0, len(payload) / 16384.0)),
                )
                end_started = True
                self.request(end_type, b"", end_reply, end_timeout)
                if not self.args.json:
                    print(flush=True)
                return
            except (TransportError, TimeoutError) as exc:
                if end_started:
                    if begin_type == UPLOAD_BEGIN:
                        # A lost END reply is resolved by identity, never by
                        # blindly repeating END or sending RUN. HELLO keeps
                        # admitted packages but discards incomplete staging.
                        self.reconnect()
                        info = self.transport.info(self.args.handshake_timeout)
                        self.finish_info()
                        if (info["state"] == 2 and
                                info["package_length"] == len(payload) and
                                info["package_crc"] == crc32(payload)):
                            self.emit({"type": "recovered", "operation": "upload_end",
                                       "status": "admitted_identity_confirmed"})
                            return
                        if (attempt == 0 and info["state"] == 0 and
                                info["package_length"] == 0 and
                                not info.get("image_valid", 0)):
                            self.emit({"type": "retry", "operation": "upload_staging",
                                       "reason": "lost END; device confirmed idle without a package"})
                            continue
                    # END may have committed the staged payload even when its
                    # reply was lost. Never destroy that evidence by starting
                    # another upload automatically; no RUN/CHAINLOAD follows.
                    raise TransportError(
                        "upload END reply was not received; staged admission "
                        "is unknown. No RUN or CHAINLOAD was sent. Inspect "
                        "device status before explicitly retrying the upload"
                    ) from exc
                if attempt:
                    raise
                # HELLO explicitly discards an incomplete staging transaction.
                # This bounded restart is safe in batches too: END and RUN
                # have not been sent, so no package can have been admitted.
                self.emit({"type": "retry", "operation": "upload_staging",
                           "reason": str(exc)})
                self.reconnect()
        raise AssertionError("unreachable")

    def upload_package(self, path: Path) -> tuple[int, int]:
        payload = path.read_bytes()
        identity = (len(payload), crc32(payload))
        self.upload_bytes(payload)
        self.emit({"type": "uploaded", "path": str(path), "bytes": identity[0], "crc32": _hex(identity[1])})
        return identity

    def chainload(self, path: Path) -> None:
        # A running QuickJS guest owns the shared upload buffer and native
        # resources.  Standalone chainload and cycle must both make the device
        # quiescent before IMAGE_BEGIN.
        status = self.transport.status(self.args.handshake_timeout)  # type: ignore[union-attr]
        if status["runtime_active"]:
            self.request(STOP, timeout=max(5.0, self.args.handshake_timeout))
        wrapped = path.read_bytes()
        image = decode_ipod(wrapped)  # validates big-endian ipco checksum/model
        self.emit({"type": "image", "path": str(path), "wrapper_bytes": len(wrapped), "image_bytes": len(image), "crc32": _hex(crc32(image))})
        self.upload_bytes(image, IMAGE_BEGIN, IMAGE_DATA, IMAGE_END,
                          IMAGE_REPLY, IMAGE_REPLY, IMAGE_REPLY)
        try:
            self.request(CHAINLOAD, b"", CHAINLOAD_REPLY, self.args.timeout * 3)
        except (TransportError, TimeoutError) as exc:
            # Windows can retire the COM handle when the new firmware starts
            # before delivering the final ACK to this process. Do not resend
            # CHAINLOAD. The fresh HELLO/INFO checks below must still succeed,
            # and reject the source service while it retains the staged image.
            self.emit({"type": "handoff_reply_missing", "detail": str(exc)})
        # CHAINLOAD intentionally drops USB. Probe every serial endpoint until
        # the re-enumerated PJSU service answers; Windows may assign a new COM
        # number after a device reset.
        deadline = time.monotonic() + self.args.reenumerate_timeout
        old_port = self.transport.port  # type: ignore[union-attr]
        self.transport.close()  # type: ignore[union-attr]
        disconnect_seen = False
        while time.monotonic() < deadline:
            ports = discover_ports()
            if old_port not in ports:
                disconnect_seen = True
            if old_port in ports:
                ports.remove(old_port)
                ports.insert(0, old_port)
            for port in ports:
                candidate = SerialTransport(
                    port, self.args.baud, min(self.args.timeout, 0.4), 0,
                    self.on_frame,
                )
                try:
                    candidate.open()
                    hello = candidate.hello(min(1.0, self.args.handshake_timeout),
                                             retries=2, retry_wait=0.15)
                    info = candidate.info(min(1.0, self.args.handshake_timeout))
                    if (port == old_port and not disconnect_seen and
                        (info["image_valid"] or info["chainload_ready"])):
                        # Windows can retain the same COM device throughout a
                        # controller reset.  A fresh service has no staged
                        # native image; the source service still does, so use
                        # protocol state when no port-removal event is visible.
                        candidate.close()
                        continue
                    self.transport = candidate
                    self.emit({"type": "chainloaded", "port": port,
                               "hello": {**hello, "state": _state(hello["state"])},
                               "info": info})
                    return
                except (TransportError, TimeoutError, ProtocolError):
                    if port == old_port:
                        disconnect_seen = True
                    candidate.close()
            time.sleep(0.25)
        raise TimeoutError(f"iPod USB service did not re-enumerate after leaving {old_port}")

    def cycle(self, package: Path, image: Optional[Path] = None,
              phase: Optional[str] = None) -> None:
        """Optionally reload native firmware, then upload and prove a guest runs."""

        def read_info() -> Dict[str, Any]:
            assert self.transport is not None
            for attempt in range(2):
                try:
                    return self.transport.info(self.args.handshake_timeout)
                except TimeoutError:
                    if attempt:
                        raise
                    # INFO is read-only. A new sequence can safely recover a
                    # lost response without reopening or repeating RUN/END.
                    self.emit({"type": "retry", "operation": "info"})
            raise AssertionError("unreachable")

        if image is not None:
            self.chainload(image)
        try:
            self.request(STOP, timeout=max(5.0, self.args.handshake_timeout))
        except (TransportError, TimeoutError):
            if getattr(self.args, "batch_mode", False):
                raise TransportError("batch stopped: STOP acknowledgement was ambiguous")
            self.reconnect()
            stopped = self.transport.status(self.args.handshake_timeout)  # type: ignore[union-attr]
            if stopped["runtime_active"]:
                raise TimeoutError("device did not stop the running guest before upload")
        expected_length, expected_crc = self.upload_package(package)
        admitted = read_info()
        if (admitted["state"] != 2 or
                admitted["package_length"] != expected_length or
                admitted["package_crc"] != expected_crc):
            raise ProtocolError("uploaded package identity was not confirmed; no RUN sent")
        # Reopen only after confirmed admission; never repeat RUN or END to
        # work around an ambiguous state-changing reply.
        self.emit({"type": "transport_boundary", "after": "admission_info"})
        self.finish_info()
        boot_timeout = max(5.0, self.args.boot_timeout)
        try:
            self.request(RUN, timeout=min(5.0, boot_timeout))
        except TimeoutError:
            if getattr(self.args, "batch_mode", False):
                raise TransportError("batch stopped: RUN acknowledgement was ambiguous")
            # RUN may already be booting. Inspect its state without sending
            # a second command that could restart it or cancel admission.
            pass
        before = self.wait_for_runtime(boot_timeout)
        self.finish_info()
        settle = max(0.05, self.args.settle)
        time.sleep(settle)
        after = read_info()
        self.finish_info()
        passed = (
            before["state"] == 3 and after["state"] == 3 and
            after["frame_count"] > before["frame_count"] and
            after["runtime_error"] == 0 and
            after["package_length"] == expected_length and
            after["package_crc"] == expected_crc and
            after["package_hash_low"] == admitted["package_hash_low"] and
            after["package_hash_high"] == admitted["package_hash_high"]
        )
        diagnostic_errors = {
            key: after[key] for key in (
                "runtime_error", "storage_error", "fs_store_error", "audio_error",
                "audio_dma_fault", "return_failures",
            ) if key in after and after[key] != 0
        }
        passed = passed and not diagnostic_errors
        self.last_cycle_result = {"type": "result", "status": "pass" if passed else "fail",
                                  "operation": "cycle", "port": self.transport.port,  # type: ignore[union-attr]
                                  "frames_before": before["frame_count"],
                                  "frames_after": after["frame_count"],
                                  "package_hash_low": after["package_hash_low"],
                                  "package_hash_high": after["package_hash_high"],
                                  "runtime_error": _hex(after["runtime_error"]),
                                  "diagnostic_errors": diagnostic_errors}
        if phase is not None:
            self.last_cycle_result["phase"] = phase
            self.last_cycle_result["before_info"] = before
            self.last_cycle_result["after_info"] = after
        self.emit(self.last_cycle_result)
        if not passed:
            raise ProtocolError("cycle did not remain RUNNING with an increasing frame count")

    def watch(self) -> None:
        assert self.transport is not None
        deadline = None if self.args.watch_timeout is None else time.monotonic() + self.args.watch_timeout
        while deadline is None or time.monotonic() < deadline:
            try:
                frame = self.transport.read_frame(self.args.timeout if deadline is None else max(0.05, min(self.args.timeout, deadline - time.monotonic())))
                self.transport._dispatch(frame)
                if frame.msg_type == RESULT:
                    return
            except TimeoutError:
                continue
            except TransportError:
                self.reconnect()
        raise TimeoutError("device did not produce a RESULT event before --watch-timeout")

    def enter_maintenance(self) -> None:
        """Request firmware-coordinated shutdown and handoff to RAM maintenance."""
        assert self.transport is not None
        deadline = time.monotonic() + self.args.maintenance_timeout
        try:
            status = self.transport.enter_maintenance(
                timeout=self.args.maintenance_timeout,
                poll_interval=min(0.25, max(0.05, self.args.timeout)),
            )
        except ProtocolError as exc:
            if "busy" in str(exc).lower():
                raise ProtocolError(
                    "device is BUSY; open an ordinary app from Apps and "
                    "stop playback, then retry maintenance"
                ) from exc
            raise
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise TimeoutError("maintenance readiness deadline expired after idle transition")
        hello = self.transport.hello(
            timeout=min(self.args.handshake_timeout, remaining),
            retries=0,
        )
        if hello["package_capacity"] < 4 * 1024 * 1024:
            raise ProtocolError(
                f"maintenance HELLO package capacity is only {hello['package_capacity']} bytes"
            )
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise TimeoutError("maintenance readiness deadline expired before final status")
        status = self.transport.status(min(self.args.handshake_timeout, remaining))
        if status["state"] != 0 or status["runtime_active"] != 0:
            raise ProtocolError(
                "maintenance did not reach idle: "
                f"state={status['state']} runtime_active={status['runtime_active']}"
            )
        self.emit({
            "type": "maintenance",
            "status": "ready",
            "ready": True,
            "port": self.transport.port,
            "hello": {**hello, "state_name": _state(hello["state"])},
            "device_status": {**status, "state_name": _state(status["state"])},
        })

    def commit_package(self) -> None:
        """Commit the already admitted maintenance package once."""
        assert self.transport is not None
        before = self.transport.info(self.args.handshake_timeout)
        expected_hash = (before.get("package_hash_low", 0),
                         before.get("package_hash_high", 0))
        try:
            result = self.transport.commit_package(self.args.commit_timeout)
        except (TimeoutError, TransportError) as exc:
            raise TimeoutError(
                "COMMIT_PACKAGE acknowledgement was lost; inspect INFO for "
                "commit_status/generation/hash and do not resend automatically"
            ) from exc
        info = self.transport.info(self.args.handshake_timeout)
        actual_hash = (info.get("commit_hash_low"), info.get("commit_hash_high"))
        if (info.get("commit_status") != 0 or
                info.get("commit_generation") != result["generation"] or
                actual_hash != expected_hash):
            raise ProtocolError(
                "COMMIT_PACKAGE verification failed; inspect INFO before any retry: "
                f"status={info.get('commit_status')!r} "
                f"generation={info.get('commit_generation')!r} "
                f"expected_generation={result['generation']!r} "
                f"hash={actual_hash!r} expected_hash={expected_hash!r}"
            )
        self.emit({"type": "commit", "status": "committed",
                   "generation": result["generation"],
                   "commit_status": info.get("commit_status", 0),
                   "commit_hash_low": info.get("commit_hash_low", 0),
                   "commit_hash_high": info.get("commit_hash_high", 0),
                   "port": self.transport.port})

    def reboot(self) -> None:
        """Request maintenance reboot once; callers inspect after timeout."""
        assert self.transport is not None
        self.transport.reboot(self.args.handshake_timeout)
        self.emit({"type": "reboot", "status": "acknowledged",
                   "port": self.transport.port})

    def batch(self) -> int:
        """Run an ordered package sequence with host-handle boundaries."""
        self.args.batch_mode = True
        if self.batch_log is None:
            self.batch_log = open(self.args.batch_log, "w", encoding="utf-8")
        if self.args.batch_package:
            cases = [(f"{index + 1}:{Path(path).stem}", Path(path))
                     for index, path in enumerate(self.args.batch_package)]
        else:
            cases = [("HOME", Path(self.args.home_package)),
                     ("TIMER", Path(self.args.timer_package)),
                     ("HOME_REOPEN", Path(self.args.home_package))]
        if self.args.repeat > 1:
            cases = [(f"{iteration + 1}/{name}", package)
                     for iteration in range(self.args.repeat) for name, package in cases]
        needs_maintenance = self.args.maintenance_once or (
            self.args.maintenance_auto and self.transport is not None and
            self.transport.package_capacity < 4 * 1024 * 1024)
        self.emit({"type": "batch", "status": "started", "cases": [name for name, _ in cases],
                   "maintenance": bool(needs_maintenance), "port": self.args.port})
        if needs_maintenance:
            try:
                self.enter_maintenance()
            except (TransportError, TimeoutError, ProtocolError, PromptRequired, OSError, ValueError) as exc:
                self.emit({"type": "batch", "status": "error", "failed_step": "maintenance",
                           "error": str(exc)})
                return 3
        for name, package in cases:
            started = time.monotonic()
            self.emit({"type": "batch_case", "case": name, "status": "started",
                       "package": str(package)})
            try:
                self.cycle(package, phase=name)
            except (TransportError, TimeoutError, ProtocolError, PromptRequired, OSError, ValueError) as exc:
                self.emit({"type": "batch_case", "case": name, "status": "error",
                           "error": str(exc), "elapsed_s": round(time.monotonic() - started, 3)})
                self.emit({"type": "batch", "status": "error", "failed_case": name})
                return 3
            summary = {"type": "batch_case", "case": name, "status": "pass",
                       "elapsed_s": round(time.monotonic() - started, 3)}
            if self.last_cycle_result is not None:
                for key in ("frames_before", "frames_after", "runtime_error",
                            "package_hash_low", "package_hash_high",
                            "diagnostic_errors"):
                    summary[key] = self.last_cycle_result[key]
            self.emit(summary)
        self.emit({"type": "batch", "status": "pass", "cases": [name for name, _ in cases]})
        return 0

    def execute(self) -> int:
        command = self.args.command
        if command == "discover" and not self.args.port:
            self.emit({"type": "ports", "ports": discover_ports()})
            return 0
        if command == "batch":
            if self.args.batch_log is None:
                log_dir = Path(__file__).resolve().parents[1] / "build" / "usb"
                log_dir.mkdir(parents=True, exist_ok=True)
                self.args.batch_log = str(log_dir / f"batch-{datetime.now():%Y%m%d-%H%M%S-%f}.jsonl")
            self.batch_log = open(self.args.batch_log, "w", encoding="utf-8")
            self.emit({"type": "batch_log", "path": str(Path(self.args.batch_log).resolve())})
        self.connect()
        if command == "discover":
            return 0
        if command == "batch":
            return self.batch()
        if command == "apps":
            self.list_apps()
            return 0
        if command in ("install", "remove", "launch"):
            self.manage_app(command)
            return 0
        if command == "info":
            info = self.transport.info(self.args.handshake_timeout)  # type: ignore[union-attr]
            self.emit({**info, "state": _state(info["state"])})
        elif command == "status":
            status = self.transport.status(self.args.handshake_timeout)  # type: ignore[union-attr]
            self.emit({**status, "state": _state(status["state"])})
        elif command == "stop":
            self.request(STOP)
        elif command == "upload":
            self.upload_package(Path(self.args.package))
        elif command in ("chainload", "flash"):
            self.chainload(Path(self.args.image))
        elif command == "cycle":
            self.cycle(Path(self.args.package),
                       Path(self.args.image) if self.args.image else None)
        elif command == "run":
            if self.args.package:
                self.upload_package(Path(self.args.package))
            boot_timeout = max(5.0, self.args.boot_timeout)
            self.request(RUN, timeout=min(5.0, boot_timeout))
            self.wait_for_runtime(boot_timeout)
            if not self.args.no_watch:
                self.watch()
        elif command == "watch":
            self.watch()
        elif command == "maintenance":
            self.enter_maintenance()
        elif command == "commit":
            self.commit_package()
        elif command == "disk-mode":
            assert self.transport is not None
            if self.transport.package_capacity < 4 * 1024 * 1024:
                self.enter_maintenance()
            self.request(STOP, timeout=10.0)
            self.transport.reboot(self.args.handshake_timeout, disk_mode=True)
            self.emit({"type": "reboot", "target": "disk-mode", "status": "acknowledged",
                       "port": self.transport.port})
        elif command == "reboot":
            self.reboot()
        return 0


def resolve_port(explicit: Optional[str], baud: int, timeout: float) -> str:
    if explicit:
        if os.name != "nt" and re.fullmatch(r"(?i)COM\d+", explicit):
            raise TransportError(
                f"{explicit} is a Windows host port, not a WSL/Linux device path; "
                "run this command with `py -3` in PowerShell, or attach the USB "
                "device to WSL2 with usbipd and use --port /dev/ttyACM0"
            )
        return explicit
    candidates = []
    for port in discover_ports():
        transport = SerialTransport(port, baud, min(timeout, 0.4), 0)  # type: ignore[name-defined]
        try:
            transport.open()
            transport.hello(0.8, retries=1, retry_wait=0.15)
            candidates.append(port)
        except Exception:
            pass
        finally:
            transport.close()
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        if os.name != "nt":
            raise TransportError(
                "no PocketJS PJSU service responded; WSL2 does not automatically "
                "pass USB through. Attach the device with usbipd-win and use "
                "--port /dev/ttyACM0, or run the runner with Windows `py -3`"
            )
        raise TransportError("no PocketJS PJSU service responded; connect the iPod or pass --port COMx")
    raise TransportError("multiple PocketJS PJSU services found; pass --port explicitly: " + ", ".join(candidates))


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="PocketJS iPod Photo binary PJSU USB runner")
    p.add_argument("command", choices=("discover", "info", "status", "upload", "run", "stop", "watch", "maintenance", "commit", "reboot", "disk-mode", "chainload", "flash", "cycle", "batch", "apps", "install", "remove", "launch"))
    p.add_argument("--port", help="serial port (Windows COM7, macOS /dev/cu.*, Linux /dev/ttyACM0)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--timeout", type=float, default=1.0)
    p.add_argument("--handshake-timeout", type=float, default=3.0)
    p.add_argument("--boot-timeout", type=float, default=60.0,
                   help="seconds to wait for guest startup after RUN; independent of USB handshake retries")
    p.add_argument("--hello-retries", type=int, default=3,
                   help="reopen and retry HELLO after a fresh-open timeout")
    p.add_argument("--hello-retry-wait", type=float, default=0.2,
                   help="seconds to wait between fresh-open HELLO attempts")
    p.add_argument("--reenumerate-timeout", type=float, default=60.0,
                   help="seconds to wait for the chainloaded PJSU service")
    p.add_argument("--retries", type=int, default=5)
    p.add_argument("--json", action="store_true", help="emit JSON Lines presentation output")
    p.add_argument("--noninteractive", action="store_true", help="fail on an unanswered prompt")
    p.add_argument("--answer", action="append", metavar="TOKEN=VALUE")
    p.add_argument("--package", help=".pocket/APP.PKT payload for upload or run")
    p.add_argument("--chunk-size", type=int, default=0,
                   help="maximum DATA bytes per request; 0 uses the negotiated limit")
    p.add_argument("--name", help="persistent app name (1-8 characters)")
    p.add_argument("--launch", action="store_true", help="launch the installed app from storage")
    p.add_argument("--image", help="native .ipod image for chainload/flash")
    p.add_argument("--no-watch", action="store_true")
    p.add_argument("--watch-timeout", type=float, default=None)
    p.add_argument("--maintenance-timeout", type=float, default=30.0,
                   help="seconds to wait for idle RAM maintenance readiness")
    p.add_argument("--commit-timeout", type=float, default=60.0,
                   help="seconds to wait for synchronous package commit")
    p.add_argument("--settle", type=float, default=1.0,
                   help="seconds between cycle frame-count checks")
    p.add_argument("--home-package", help="HOME .pocket/APP.PKT package for batch")
    p.add_argument("--timer-package", help="TIMER .pocket/APP.PKT package for batch")
    p.add_argument("--batch-package", action="append", metavar="PATH",
                   help="package to cycle in order; repeat this option for each batch step")
    p.add_argument("--repeat", type=int, default=1,
                   help="number of times to run the entire batch sequence")
    p.add_argument("--batch-log",
                   help="JSONL path for batch records (default: timestamped file under hosts/ipod-photo/build/usb)")
    p.add_argument("--maintenance-once", "--maintenance", dest="maintenance_once",
                   action="store_true", help="enter RAM maintenance once before the batch")
    p.add_argument("--maintenance-auto", action="store_true",
                   help="enter maintenance only when the batch connects to the resident observer")
    return p


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = build_parser().parse_args(list(argv) if argv is not None else None)
    if args.command in ("upload", "cycle") and args.package is None:
        build_parser().error(f"{args.command} requires --package PATH")
    if args.command in ("install", "remove", "launch") and not args.name:
        build_parser().error("app management requires --name NAME")
    if args.command == "install" and not args.package:
        build_parser().error("install requires --package PATH")
    if args.command in ("chainload", "flash") and args.image is None:
        build_parser().error(f"{args.command} requires --image PATH")
    if args.command == "batch":
        if args.batch_package and (args.home_package or args.timer_package):
            build_parser().error("use --batch-package or --home-package/--timer-package, not both")
        if not args.batch_package and (args.home_package is None or args.timer_package is None):
            build_parser().error("batch requires --batch-package PATH or --home-package PATH and --timer-package PATH")
    if args.repeat < 1:
        build_parser().error("--repeat must be positive")
    if args.chunk_size < 0:
        build_parser().error("--chunk-size must not be negative")
    if args.package and not Path(args.package).is_file():
        build_parser().error(f"package does not exist: {args.package}")
    if args.image and not Path(args.image).is_file():
        build_parser().error(f"image does not exist: {args.image}")
    if args.commit_timeout <= 0.0:
        build_parser().error("--commit-timeout must be positive")
    if not math.isfinite(args.settle) or args.settle < 0.0:
        build_parser().error("--settle must be finite and not negative")
    for path_arg in (args.home_package, args.timer_package, *(args.batch_package or [])):
        if path_arg and not Path(path_arg).is_file():
            build_parser().error(f"package does not exist: {path_arg}")
    runner = IpodRunner(args)
    try:
        return runner.execute()
    except (TransportError, ProtocolError, PromptRequired, TimeoutError, OSError, ValueError) as exc:
        runner.emit({"type": "error", "error": str(exc), "action": "check USB cable, device mode, image/package, and --port COMx"})
        return 3
    finally:
        if runner.transport is not None:
            runner.transport.close()
        if runner.batch_log is not None:
            runner.batch_log.close()


if __name__ == "__main__":
    raise SystemExit(main())
