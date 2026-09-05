#!/usr/bin/env python3
"""Binary PJSU transport for the PocketJS iPod Photo USB service.

The wire format mirrors ``include/usb_protocol.h`` exactly. It is a fixed
20-byte little-endian header followed by a raw payload; only the runner's
presentation layer uses JSON.
"""

from __future__ import annotations

import struct
import time
import zlib
from dataclasses import dataclass
from typing import Any, Callable, Dict, Iterator, Optional

MAGIC = b"PJSU"
VERSION = 1
HEADER_BYTES = 20
MAX_PAYLOAD = 4096
PERFORMANCE_TRAILER_MARKER = 0x31465250  # PRF1
BOOT_PROFILE_TRAILER_MARKER = 0x31504251  # QBP1
POWER_TRAILER_MARKER = 0x31525750  # PWR1
PACKAGE_STATUS_TRAILER_MARKER = 0x31545350  # PST1
DIAGNOSTICS_TRAILER_MARKER = 0x314e4744  # DGN1
DEFAULT_MAX_PAYLOAD = 1024
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 1.0
POCKETJS_VID = 0x1209
POCKETJS_PID = 0x504A

HELLO = 0x01
HELLO_REPLY = 0x81
INFO = 0x02
INFO_REPLY = 0x82
UPLOAD_BEGIN = 0x10
UPLOAD_DATA = 0x11
UPLOAD_END = 0x12
UPLOAD_REPLY = 0x90
IMAGE_BEGIN = 0x13
IMAGE_DATA = 0x14
IMAGE_END = 0x15
IMAGE_REPLY = 0x91
RUN = 0x20
STOP = 0x21
STATUS = 0x22
CHAINLOAD = 0x23
ENTER_MAINTENANCE = 0x24
COMMIT_PACKAGE = 0x25
REBOOT = 0x26
RUN_REPLY = 0xA0
STOP_REPLY = 0xA1
STATUS_REPLY = 0xA2
CHAINLOAD_REPLY = 0xA3
ENTER_MAINTENANCE_REPLY = 0xA4
COMMIT_PACKAGE_REPLY = 0xA5
REBOOT_REPLY = 0xA6
LOG = 0x30
RESULT = 0x31
PROMPT = 0x32
ERROR = 0x7F

STATUS_NAMES = {
    0: "ok", 1: "bad_request", 2: "bad_version", 3: "bad_sequence",
    4: "busy", 5: "too_large", 6: "out_of_order", 7: "bad_crc",
    8: "bad_package", 9: "not_ready", 10: "runtime", 11: "no_memory",
    12: "internal",
}
STATE_NAMES = {
    0: "idle", 1: "uploading", 2: "ready", 3: "running", 4: "error",
    5: "booting",
}


class ProtocolError(RuntimeError):
    """Malformed peer data or a non-OK device status."""


class TransportError(RuntimeError):
    """Serial connection failure."""


def crc32(payload: bytes) -> int:
    """Reflected CRC-32, initialized to 0xffffffff and complemented at end."""

    return zlib.crc32(payload) & 0xFFFFFFFF


def crc32_hex(payload: bytes) -> str:
    return f"{crc32(payload):08x}"


def frame_crc(version: int, msg_type: int, flags: int, sequence: int, payload: bytes) -> int:
    sequence &= 0xFFFFFFFF
    covered = struct.pack("<BBHII", version, msg_type, flags, sequence, len(payload)) + payload
    return crc32(covered)


@dataclass(frozen=True)
class Frame:
    msg_type: int
    flags: int
    sequence: int
    payload: bytes


def encode_frame(msg_type: int, sequence: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if not 0 <= msg_type <= 0xFF or not 0 <= flags <= 0xFFFF:
        raise ValueError("message type or flags out of range")
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"payload is {len(payload)} bytes; maximum is {MAX_PAYLOAD}")
    sequence &= 0xFFFFFFFF
    header = struct.pack(
        "<4sBBHIII", MAGIC, VERSION, msg_type, flags, sequence & 0xFFFFFFFF,
        len(payload), frame_crc(VERSION, msg_type, flags, sequence, payload),
    )
    return header + payload


def decode_frame(raw: bytes) -> Frame:
    if len(raw) < HEADER_BYTES:
        raise ProtocolError("truncated PJSU header")
    magic, version, msg_type, flags, sequence, length, expected_crc = struct.unpack(
        "<4sBBHIII", raw[:HEADER_BYTES]
    )
    if magic != MAGIC:
        raise ProtocolError(f"bad PJSU magic: {magic!r}")
    if version != VERSION:
        raise ProtocolError(f"unsupported PJSU version {version}")
    if length > MAX_PAYLOAD or len(raw) != HEADER_BYTES + length:
        raise ProtocolError(f"invalid PJSU payload length {length}")
    payload = raw[HEADER_BYTES:]
    actual_crc = frame_crc(version, msg_type, flags, sequence, payload)
    if actual_crc != expected_crc:
        raise ProtocolError(f"bad PJSU frame CRC: got {expected_crc:08x}, expected {actual_crc:08x}")
    return Frame(msg_type, flags, sequence, payload)


def u32(payload: bytes, offset: int = 0) -> int:
    if offset + 4 > len(payload):
        raise ProtocolError("response payload is missing a u32")
    return struct.unpack_from("<I", payload, offset)[0]


def pack_u32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def status_payload(frame: Frame) -> tuple[int, bytes]:
    if len(frame.payload) < 4:
        raise ProtocolError("response payload is missing status")
    return u32(frame.payload), frame.payload[4:]


class DeviceStatusError(ProtocolError):
    def __init__(self, status: int, detail: bytes = b"") -> None:
        self.status = status
        self.detail = detail
        name = STATUS_NAMES.get(status, f"status_{status}")
        detail_text = f" detail=0x{u32(detail):08x}" if len(detail) >= 4 else ""
        if len(detail) > 4:
            message = detail[4:].decode("utf-8", errors="replace").rstrip("\0")
            if message:
                detail_text += f" message={message!r}"
        super().__init__(f"device rejected request: {name} ({status}){detail_text}")


class SerialTransport:
    """Synchronous PJSU serial transport with event dispatch and reconnect."""

    def __init__(
        self,
        port: str,
        baud: int = DEFAULT_BAUD,
        timeout: float = DEFAULT_TIMEOUT,
        reconnect_attempts: int = 5,
        event_handler: Optional[Callable[[Frame], None]] = None,
    ) -> None:
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.reconnect_attempts = max(0, reconnect_attempts)
        self.event_handler = event_handler
        self._serial: Any = None
        self._sequence = 1
        self.max_payload = DEFAULT_MAX_PAYLOAD

    def open(self) -> None:
        if self._serial is not None and getattr(self._serial, "is_open", False):
            return
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise TransportError("pyserial is required; install it with `python -m pip install pyserial`") from exc
        try:
            self._serial = serial.Serial(
                self.port, self.baud, timeout=0,
                write_timeout=max(1.0, self.timeout), dsrdtr=False, rtscts=False,
            )
            self._serial.dtr = False
            self._serial.rts = False
        except Exception as exc:
            self._serial = None
            raise TransportError(f"could not open serial port {self.port!r}: {exc}") from exc

    def close(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            finally:
                self._serial = None

    def reconnect(self, wait: float = 0.25) -> None:
        self.close()
        # The device protocol context may survive a transient USB disconnect.
        # Reusing sequence 1 here can be mistaken for a duplicate command and
        # also breaks a contiguous upload retry.  Keep the sequence epoch;
        # after a full device reset the peer accepts the next value anyway.
        if wait:
            time.sleep(wait)
        last: Optional[Exception] = None
        for attempt in range(self.reconnect_attempts + 1):
            try:
                self.open()
                return
            except TransportError as exc:
                last = exc
                if attempt < self.reconnect_attempts:
                    time.sleep(min(2.0, 0.25 * (attempt + 1)))
        raise TransportError(str(last) if last else f"could not reconnect to {self.port}")

    def __enter__(self) -> "SerialTransport":
        self.open()
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    def _write_all(self, data: bytes) -> None:
        self.open()
        offset = 0
        try:
            while offset < len(data):
                written = self._serial.write(data[offset:])
                if not written:
                    raise TransportError("serial write made no progress")
                offset += written
            # A request/response transaction already proves delivery. On
            # Windows USB CDC, flush() can wait forever when an old device
            # endpoint has stalled, bypassing both write_timeout and the
            # protocol request deadline.
        except TransportError:
            self.close()
            raise
        except Exception as exc:
            self.close()
            raise TransportError(f"serial write failed on {self.port}: {exc}") from exc

    def _read_exact(self, length: int, timeout: Optional[float] = None) -> bytes:
        self.open()
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        out = bytearray()
        try:
            while len(out) < length and time.monotonic() < deadline:
                # Keep the port nonblocking and enforce the deadline here.
                # Changing pyserial.timeout calls SetCommState on Windows,
                # reconfiguring CDC in the middle of a request/response.
                remaining = min(length - len(out), max(1, getattr(self._serial, "in_waiting", 0)))
                part = self._serial.read(remaining)
                if part:
                    out.extend(part)
                else:
                    time.sleep(0.003)
        except Exception as exc:
            self.close()
            raise TransportError(f"serial read failed on {self.port}: {exc}") from exc
        if len(out) != length:
            raise TimeoutError(f"timed out after {timeout or self.timeout:.1f}s waiting for PJSU frame")
        return bytes(out)

    def read_frame(self, timeout: Optional[float] = None) -> Frame:
        window = bytearray()
        while True:
            window.extend(self._read_exact(1, timeout))
            if len(window) > 4:
                del window[:-4]
            if bytes(window) == MAGIC:
                break
        rest = self._read_exact(HEADER_BYTES - 4, timeout)
        header = MAGIC + rest
        _, version, msg_type, flags, sequence, length, _ = struct.unpack("<4sBBHIII", header)
        if version != VERSION:
            raise ProtocolError(f"unsupported PJSU version {version}")
        if length > MAX_PAYLOAD:
            raise ProtocolError(f"peer payload exceeds {MAX_PAYLOAD}: {length}")
        payload = self._read_exact(length, timeout) if length else b""
        return decode_frame(header + payload)

    def _dispatch(self, frame: Frame) -> None:
        if frame.msg_type in (LOG, RESULT, PROMPT) and self.event_handler is not None:
            self.event_handler(frame)

    def request(self, msg_type: int, payload: bytes = b"", timeout: float = 3.0,
                expected_type: Optional[int] = None) -> Frame:
        sequence = self._sequence
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF or 1
        self._write_all(encode_frame(msg_type, sequence, payload))
        expected = expected_type if expected_type is not None else {
            HELLO: HELLO_REPLY, INFO: INFO_REPLY,
            UPLOAD_BEGIN: UPLOAD_REPLY, UPLOAD_DATA: UPLOAD_REPLY,
            UPLOAD_END: UPLOAD_REPLY, RUN: RUN_REPLY, STOP: STOP_REPLY,
            IMAGE_BEGIN: IMAGE_REPLY, IMAGE_DATA: IMAGE_REPLY,
            IMAGE_END: IMAGE_REPLY, STATUS: STATUS_REPLY,
            CHAINLOAD: CHAINLOAD_REPLY,
            ENTER_MAINTENANCE: ENTER_MAINTENANCE_REPLY,
            COMMIT_PACKAGE: COMMIT_PACKAGE_REPLY, REBOOT: REBOOT_REPLY,
        }.get(msg_type, ERROR)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.read_frame(max(0.01, deadline - time.monotonic()))
            if frame.msg_type in (LOG, RESULT, PROMPT):
                self._dispatch(frame)
                continue
            if frame.sequence != sequence:
                continue
            if frame.msg_type == ERROR:
                status, detail = status_payload(frame)
                raise DeviceStatusError(status, detail)
            if frame.msg_type != expected:
                raise ProtocolError(f"expected response 0x{expected:02x}, got 0x{frame.msg_type:02x}")
            status, detail = status_payload(frame)
            if status != 0:
                raise DeviceStatusError(status, detail)
            return frame
        raise TimeoutError(f"timed out waiting for PJSU response to 0x{msg_type:02x}")

    def hello(self, timeout: float = 3.0, retries: int = 0,
              retry_wait: float = 0.2) -> Dict[str, int]:
        """Handshake, reopening the port after a fresh-open timeout.

        A newly enumerated CDC function can accept the host open before its
        bulk endpoints are ready.  Retrying on a newly opened handle lets the
        device finish that transition.  Do not reset ``_sequence`` here: a
        timed-out request may already have reached the device, while HELLO
        itself is explicitly allowed to start a new host epoch.
        """
        attempts = max(0, retries)
        last: Optional[Exception] = None
        for attempt in range(attempts + 1):
            try:
                self.open()
                _, extra = status_payload(self.request(HELLO, timeout=timeout))
                if len(extra) < 16:
                    raise ProtocolError("HELLO reply is missing capability fields")
                info = {"version": u32(extra), "max_payload": u32(extra, 4),
                        "package_capacity": u32(extra, 8), "state": u32(extra, 12)}
                if info["version"] != VERSION or not 64 <= info["max_payload"] <= MAX_PAYLOAD:
                    raise ProtocolError(f"unsupported device capabilities: {info}")
                self.max_payload = info["max_payload"]
                return info
            except (TransportError, TimeoutError) as exc:
                last = exc
                self.close()
                if attempt >= attempts:
                    raise
                delay = max(0.0, retry_wait) * (attempt + 1)
                if delay:
                    time.sleep(delay)
        raise TransportError(str(last) if last else "HELLO handshake failed")

    def info(self, timeout: float = 3.0) -> Dict[str, int]:
        _, extra = status_payload(self.request(INFO, timeout=timeout))
        if len(extra) < 48:
            raise ProtocolError("INFO reply is missing fields")
        info = {"version": u32(extra), "state": u32(extra, 4),
                "package_length": u32(extra, 8), "package_crc": u32(extra, 12),
                "package_hash_low": u32(extra, 16), "package_hash_high": u32(extra, 20),
                "runtime_error": u32(extra, 24), "frame_count": u32(extra, 28),
                "image_valid": u32(extra, 32), "image_length": u32(extra, 36),
                "image_crc": u32(extra, 40), "chainload_ready": u32(extra, 44)}
        if len(extra) >= 64:
            info.update({"crash_magic": u32(extra, 48),
                         "crash_reason": u32(extra, 52),
                         "crash_pc": u32(extra, 56),
                         "crash_spsr": u32(extra, 60)})
        if len(extra) >= 96:
            info.update({"storage_error": u32(extra, 64),
                         "storage_sector_reads": u32(extra, 68),
                         "storage_first_failed_lba": u32(extra, 72),
                         "storage_sector_writes": u32(extra, 76),
                         "storage_sector_flushes": u32(extra, 80),
                         "storage_failed_operation": u32(extra, 84),
                         "storage_failed_status": u32(extra, 88),
                         "storage_failed_error": u32(extra, 92)})
        if len(extra) >= 100:
            value = u32(extra, 96)
            info["fs_store_error"] = value - (1 << 32) if value & 0x80000000 else value
        if len(extra) >= 104:
            text_length = u32(extra, 100)
            if text_length > 128 or 104 + text_length > len(extra):
                raise ProtocolError("INFO reply has an invalid runtime error message")
            text_end = 104 + text_length
            info["runtime_error_text"] = extra[104:text_end].decode(
                "utf-8", errors="replace"
            )
            while len(extra) - text_end >= 4:
                marker = u32(extra, text_end)
                available = len(extra) - text_end
                if marker == PERFORMANCE_TRAILER_MARKER:
                    names = ("performance_flags", "app_load_us", "app_boot_us",
                             "launcher_teardown_us", "first_present_us",
                             "last_frame_us", "max_frame_us", "last_present_us",
                             "max_present_us")
                    if available >= 44 and u32(extra, text_end + 40) not in (
                            BOOT_PROFILE_TRAILER_MARKER, POWER_TRAILER_MARKER,
                            PACKAGE_STATUS_TRAILER_MARKER):
                        names += ("lineage_commit_us",)
                    consumed = 4 + len(names) * 4
                    if available < consumed:
                        raise ProtocolError("INFO reply has an invalid performance trailer")
                    for index, name in enumerate(names):
                        info[name] = u32(extra, text_end + 4 + index * 4)
                    text_end += consumed
                    continue
                if marker == BOOT_PROFILE_TRAILER_MARKER:
                    names = ("qjs_runtime_us", "qjs_context_us", "qjs_host_us",
                             "qjs_native_pak_us", "qjs_eval_us", "qjs_jobs_us",
                             "qjs_allocation_calls", "qjs_usb_service_us")
                    if available >= 52 and u32(extra, text_end + 36) not in (
                            POWER_TRAILER_MARKER, PACKAGE_STATUS_TRAILER_MARKER):
                        names += ("clock_source", "pll_control", "memory_timing", "device_init")
                    consumed = 4 + len(names) * 4
                    if available < consumed:
                        raise ProtocolError("INFO reply has an invalid boot profile trailer")
                    for index, name in enumerate(names):
                        info[name] = u32(extra, text_end + 4 + index * 4)
                    text_end += consumed
                    continue
                if marker == POWER_TRAILER_MARKER:
                    if available < 48:
                        raise ProtocolError("INFO reply has an invalid power trailer")
                    names = ("battery_raw", "battery_mv", "power_flags",
                             "power_sample_count", "power_failure_count",
                             "charger_requested_mode", "gpo_enable", "gpo_value",
                             "gpo_input", "usb_configured", "usb_suspended")
                    for index, name in enumerate(names):
                        info[name] = u32(extra, text_end + 4 + index * 4)
                    text_end += 48
                    continue
                if marker == DIAGNOSTICS_TRAILER_MARKER:
                    if available < 72:
                        raise ProtocolError("INFO reply has an invalid diagnostics trailer")
                    names = ("heap_free", "heap_largest_free", "heap_allocated",
                             "audio_error", "audio_busy", "warm_returns", "sleeps",
                             "wakes", "return_failures", "app_sessions",
                             "kernel_mode", "first_kernel_error", "lineage_error",
                             "lineage_generation", "lineage_source",
                             "audio_underruns", "audio_dma_fault")
                    for index, name in enumerate(names):
                        info[name] = u32(extra, text_end + 4 + index * 4)
                    text_end += 72
                    continue
                if marker == PACKAGE_STATUS_TRAILER_MARKER:
                    if available < 20:
                        raise ProtocolError("INFO reply has an invalid package status trailer")
                    value = u32(extra, text_end + 4)
                    info.update({
                        "commit_status": value - (1 << 32) if value & 0x80000000 else value,
                        "commit_generation": u32(extra, text_end + 8),
                        "commit_hash_low": u32(extra, text_end + 12),
                        "commit_hash_high": u32(extra, text_end + 16),
                    })
                    text_end += 20
                    continue
                break
        return info

    def status(self, timeout: float = 3.0) -> Dict[str, int]:
        _, extra = status_payload(self.request(STATUS, timeout=timeout))
        if len(extra) < 36:
            raise ProtocolError("STATUS reply is missing fields")
        return {"state": u32(extra), "runtime_active": u32(extra, 4),
                "package_valid": u32(extra, 8), "package_length": u32(extra, 12),
                "image_valid": u32(extra, 16), "image_length": u32(extra, 20),
                "runtime_error": u32(extra, 24), "package_error": u32(extra, 28),
                "image_crc": u32(extra, 32)}

    def commit_package(self, timeout: float = 30.0) -> Dict[str, int]:
        """Commit the admitted RAM package through the device callback."""
        _, extra = status_payload(self.request(COMMIT_PACKAGE, timeout=timeout))
        if len(extra) < 4:
            raise ProtocolError("COMMIT_PACKAGE reply is missing generation")
        return {"generation": u32(extra)}

    def reboot(self, timeout: float = 3.0) -> None:
        """Request maintenance reboot; never retries this mutation."""
        self.request(REBOOT, timeout=timeout)

    def enter_maintenance(self, timeout: float = 30.0,
                          poll_interval: float = 0.2) -> Dict[str, int]:
        """Request the explicit observer-to-RAM maintenance transition.

        The device acknowledges the request before its main loop performs the
        clean shutdown.  Poll STATUS until that transition reaches idle, so a
        caller never starts an upload while the resident app still owns the
        runtime or storage.  The device returns BUSY when its app is not in a
        safe, available state; callers may surface that status directly.
        """
        if timeout <= 0.0:
            raise ValueError("maintenance timeout must be positive")
        self.request(ENTER_MAINTENANCE, timeout=min(3.0, timeout))
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise TimeoutError("timed out waiting for maintenance idle")
            try:
                status = self.status(timeout=min(3.0, remaining))
            except (TimeoutError, TransportError):
                # Entry may already have succeeded. Reopen only the host
                # transport and inspect state; never repeat the entry request.
                self.close()
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    raise TimeoutError("timed out confirming maintenance entry")
                try:
                    self.hello(timeout=min(3.0, remaining), retries=0)
                except (TimeoutError, TransportError):
                    self.close()
                continue
            if status["state"] == 0 and status["runtime_active"] == 0:
                return status
            time.sleep(min(max(0.0, poll_interval),
                           max(0.0, deadline - time.monotonic())))

    def stream(self, timeout: Optional[float] = None) -> Iterator[Frame]:
        while True:
            try:
                frame = self.read_frame(timeout)
            except TimeoutError:
                return
            self._dispatch(frame)
            yield frame


def discover_ports() -> list[str]:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError as exc:
        raise TransportError("pyserial is required; install it with `python -m pip install pyserial`") from exc
    ports = sorted(list_ports.comports(), key=lambda p: str(getattr(p, "device", p)).lower())
    selected: list[str] = []
    for port in ports:
        # Windows exposes VID/PID for normal USB CDC ports.  Keep ports with
        # incomplete metadata so Linux/WSL and unusual USB serial drivers can
        # still be probed, but never auto-probe a known non-PocketJS adapter.
        vid = getattr(port, "vid", None)
        pid = getattr(port, "pid", None)
        if vid is not None and pid is not None:
            try:
                if int(vid) != POCKETJS_VID or int(pid) != POCKETJS_PID:
                    continue
            except (TypeError, ValueError):
                pass
        selected.append(str(port.device))
    return selected
