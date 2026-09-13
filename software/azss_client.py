#!/usr/bin/env python3
"""
azss_client.py — Production BLE client for AZSensorSuite

Firmware service: 229a0001-ad33-4a06-9bce-c34201743655
Requires Python 3.8+ and bleak.
"""

from __future__ import annotations

import asyncio
import contextlib
import csv
import enum
import gc
import re
import time
import threading
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Callable, Dict, Any, List

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

# ============================================================================
# Constants
# ============================================================================

class StreamId(enum.IntEnum):
    STAMP   = 1
    AMBIENT = 2

class BleCmd(enum.IntEnum):
    CONN_20MS    = 0x00
    CONN_100MS   = 0x01
    CONN_512MS   = 0x02
    CONN_1024MS  = 0x03
    STREAM_START = 0x10
    STREAM_STOP  = 0x11
    FW_VERSION   = 0x20

BULK_FLAG_LAST = 0x01
BULK_HDR_LEN   = 4

AMBIENT_INTERVAL_S = 15 * 60
DELTA_ROLLOVER_THRESHOLD_S = 10 * 365 * 24 * 3600

SERVICE_UUID        = "229a0001-ad33-4a06-9bce-c34201743655"
COUNT_UUID          = "229a0002-ad33-4a06-9bce-c34201743655"
TEMP_UUID           = "229a0003-ad33-4a06-9bce-c34201743655"
HUMIDITY_UUID       = "229a0004-ad33-4a06-9bce-c34201743655"
BULK_UUID           = "229a0005-ad33-4a06-9bce-c34201743655"
CURR_TIMESTAMP_UUID = "229a0006-ad33-4a06-9bce-c34201743655"
BASE_TIMESTAMP_UUID = "229a0007-ad33-4a06-9bce-c34201743655"
COMMAND_UUID        = "229a0008-ad33-4a06-9bce-c34201743655"

SCAN_TIMEOUT           = 15
CONNECT_TIMEOUT        = 45
CONNECT_RETRIES        = 3
POST_CONNECT_DELAY     = 0.5
DOWNLOAD_TOTAL_TIMEOUT = 900   # 15 minutes

CSV_DIR = Path("csvs")

# ============================================================================
# Data structures
# ============================================================================

@dataclass
class SeenDevice:
    name: str
    address: str
    device: BLEDevice

@dataclass(frozen=True)
class BulkPacket:
    stream_id: int
    seq: int
    flags: int
    payload: bytes

    @property
    def is_last(self) -> bool:
        return bool(self.flags & BULK_FLAG_LAST)

@dataclass
class DownloadState:
    base_time_s: int = 0
    stamp_buf: bytearray = field(default_factory=bytearray)
    ambient_buf: bytearray = field(default_factory=bytearray)
    stamp_cumulative_s: int = 0
    stamp_rows: int = 0
    ambient_rows: int = 0
    seen_last_stamp: bool = False
    seen_last_ambient: bool = False
    expected_seq_stamp: Optional[int] = None
    expected_seq_ambient: Optional[int] = None

    @property
    def done(self) -> bool:
        return self.seen_last_stamp and self.seen_last_ambient

# ============================================================================
# Logging
# ============================================================================

def info(msg: str) -> None:
    print(f"[INFO] {msg}")

def warn(msg: str) -> None:
    print(f"[WARN] {msg}")

def error(msg: str) -> None:
    print(f"[ERROR] {msg}")

# ============================================================================
# Input helpers
# ============================================================================

_HEX_U8_RE  = re.compile(r"^(?:0x)?([0-9a-fA-F]{1,2})$")
_BT_ADDR_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}([0-9A-Fa-f]{2})$")

def parse_hex_u8(text: str) -> int:
    m = _HEX_U8_RE.match(text.strip())
    if not m:
        raise ValueError("Expected a hex byte such as '2A' or '0x2A'.")
    return int(m.group(1), 16)

def normalize_bt_address(text: str) -> str:
    text = text.strip().upper()
    if not _BT_ADDR_RE.match(text):
        raise ValueError("Bluetooth address must be XX:XX:XX:XX:XX:XX")
    return text

async def ainput(prompt: str = "") -> str:
    return await asyncio.to_thread(input, prompt)

# ============================================================================
# Thread‑safe CSV writer – disk I/O fully off the event loop
# ============================================================================

class AsyncCsvWriter:
    def __init__(self, path: Path, header: List[str], flush_every: int = 512) -> None:
        self.flush_every = flush_every
        self._rows: List[List[Any]] = []
        self._flush_event = threading.Event()
        self._done = False
        self._file = path.open("w", newline="")
        self._writer = csv.writer(self._file)
        self._writer.writerow(header)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def add_row(self, row: List[Any]) -> None:
        self._rows.append(row)

    def maybe_flush(self) -> None:
        if len(self._rows) >= self.flush_every:
            self._flush_event.set()

    async def finalize(self) -> None:
        self._done = True
        self._flush_event.set()
        await asyncio.to_thread(self._thread.join)

    def _run(self) -> None:
        while not self._done or self._rows:
            self._flush_event.wait(timeout=0.5)
            self._flush_event.clear()
            if self._rows:
                batch, self._rows = self._rows, []
                for row in batch:
                    self._writer.writerow(row)
                self._file.flush()
        self._file.close()

# ============================================================================
# BLE session manager
# ============================================================================

class AzssSession:
    def __init__(self) -> None:
        self._client: Optional[BleakClient] = None
        self._device: Optional[BLEDevice] = None
        self._scan_cache: Dict[str, SeenDevice] = {}
        self._disconnect_event = asyncio.Event()
        self._lock = asyncio.Lock()

    async def __aenter__(self) -> "AzssSession":
        return self

    async def __aexit__(self, *_: Any) -> None:
        if self.is_connected:
            await self.disconnect()

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    @property
    def address(self) -> Optional[str]:
        return self._device.address.upper() if self._device else None

    async def scan(self, timeout: float = SCAN_TIMEOUT) -> List[SeenDevice]:
        devices = await BleakScanner.discover(timeout=timeout)
        cache: Dict[str, SeenDevice] = {}
        entries = []
        for d in devices:
            entry = SeenDevice(name=d.name or "(unknown)", address=d.address.upper(), device=d)
            entries.append(entry)
            cache[entry.address] = entry
        self._scan_cache = cache
        return entries

    async def find_device(self, address: str, prefer_fresh: bool = True) -> Optional[BLEDevice]:
        address = normalize_bt_address(address)
        if not prefer_fresh:
            cached = self._scan_cache.get(address)
            if cached:
                return cached.device
        entries = await self.scan()
        for e in entries:
            if e.address == address:
                return e.device
        return None

    async def connect(self, address: str) -> bool:
        address = normalize_bt_address(address)
        if self.is_connected and self.address == address:
            info(f"Already connected to {address}")
            return True
        if self.is_connected:
            await self.disconnect()

        device = await self.find_device(address, prefer_fresh=True)
        if not device:
            error("Device not found. Is it advertising?")
            return False

        for attempt in range(1, CONNECT_RETRIES + 1):
            self._disconnect_event.clear()
            def on_disconnect(_: BleakClient) -> None:
                self._disconnect_event.set()
                warn("Unexpected disconnect.")

            client = BleakClient(device, disconnected_callback=on_disconnect,
                                 timeout=CONNECT_TIMEOUT)
            try:
                info(f"Connecting to {address} (attempt {attempt}/{CONNECT_RETRIES})...")
                await client.connect()
                if not client.is_connected:
                    raise RuntimeError("Connection failed silently")
                await asyncio.sleep(POST_CONNECT_DELAY)
                if not client.is_connected:
                    raise RuntimeError("Disconnected during settle")

                info("Securing connection...")
                await self._read_char(client, BASE_TIMESTAMP_UUID)

                self._client = client
                self._device = device
                info(f"Connected to {address}")
                return True
            except Exception as e:
                warn(f"Attempt failed: {e}")
                with contextlib.suppress(Exception):
                    await client.disconnect()
                await asyncio.sleep(0.5 * (2 ** (attempt - 1)))
        error(f"Failed to connect after {CONNECT_RETRIES} attempts.")
        return False

    async def disconnect(self) -> bool:
        if self._client is None:
            return False
        try:
            await self._client.disconnect()
            return True
        except Exception as e:
            warn(f"Disconnect error: {e}")
            return False
        finally:
            self._client = None
            self._device = None
            self._disconnect_event.set()

    async def _read_char(self, client: BleakClient, uuid_str: str, retries: int = 3) -> bytes:
        for attempt in range(retries):
            try:
                async with self._lock:
                    data = await client.read_gatt_char(uuid_str)
                if data is None:
                    raise RuntimeError("Empty read")
                return bytes(data)
            except Exception as e:
                if attempt == retries - 1:
                    raise
                await asyncio.sleep(0.12 * (attempt + 1))
        return b""

    async def read_char(self, uuid_str: str) -> bytes:
        if self._client is None or not self._client.is_connected:
            raise RuntimeError("Not connected")
        return await self._read_char(self._client, uuid_str)

    async def write_char(self, uuid_str: str, data: bytes, *, response: bool = True) -> None:
        if self._client is None or not self._client.is_connected:
            raise RuntimeError("Not connected")
        async with self._lock:
            await self._client.write_gatt_char(uuid_str, data, response=response)

    async def start_notify(self, uuid_str: str, callback: Callable[[int, bytearray], None]) -> None:
        if self._client is None or not self._client.is_connected:
            raise RuntimeError("Not connected")
        async with self._lock:
            await self._client.start_notify(uuid_str, callback)

    async def stop_notify(self, uuid_str: str) -> None:
        if self._client and self._client.is_connected:
            async with self._lock:
                await self._client.stop_notify(uuid_str)

# ============================================================================
# Notification queue (thread‑safe)
# ============================================================================

def make_notify_queue() -> tuple[asyncio.Queue[bytes], Callable[[int, bytearray], None]]:
    q: asyncio.Queue[bytes] = asyncio.Queue()
    loop = asyncio.get_running_loop()
    def handler(_sender: int, data: bytearray) -> None:
        loop.call_soon_threadsafe(q.put_nowait, bytes(data))
    return q, handler

# ============================================================================
# Payload decoders (non‑blocking, with recovery timeout)
# ============================================================================

async def decode_stamp_payload(
    payload: bytes,
    seq: int,
    state: DownloadState,
    writer: AsyncCsvWriter,
    session: AzssSession,
) -> None:
    if payload:
        state.stamp_buf.extend(payload)

    while len(state.stamp_buf) >= 4:
        delta_s = int.from_bytes(state.stamp_buf[:4], "little", signed=True)
        del state.stamp_buf[:4]

        if abs(delta_s) >= DELTA_ROLLOVER_THRESHOLD_S:
            try:
                # Attempt recovery with a strict timeout – never hang the event loop
                raw = await asyncio.wait_for(
                    session.read_char(CURR_TIMESTAMP_UUID),
                    timeout=5.0
                )
                current_ms = int.from_bytes(raw, "little", signed=True)
                current_s = current_ms // 1000
                delta_s = current_s - (state.base_time_s + state.stamp_cumulative_s)
                warn(f"Recovered rollover delta: {delta_s}s")
            except asyncio.TimeoutError:
                warn("Rollover recovery timed out. Writing raw delta (may be corrupt).")
                # Continue with the original delta_s – do NOT abort.
            except Exception as e:
                warn(f"Rollover recovery failed ({e}). Writing raw delta (may be corrupt).")
                # Continue with the original delta_s.

        state.stamp_cumulative_s += delta_s
        writer.add_row([
            state.stamp_rows,
            seq,
            delta_s,
            state.stamp_cumulative_s,
            state.base_time_s + state.stamp_cumulative_s,
        ])
        state.stamp_rows += 1

def decode_ambient_payload(
    payload: bytes,
    seq: int,
    state: DownloadState,
    writer: AsyncCsvWriter,
) -> None:
    if payload:
        state.ambient_buf.extend(payload)
    while len(state.ambient_buf) >= 2:
        temp_c = int.from_bytes(state.ambient_buf[:1], "little", signed=True)
        humidity = state.ambient_buf[1]
        del state.ambient_buf[:2]
        writer.add_row([
            state.ambient_rows,
            seq,
            temp_c,
            humidity,
            state.base_time_s + AMBIENT_INTERVAL_S * state.ambient_rows,
        ])
        state.ambient_rows += 1

# ============================================================================
# Bulk Download Manager
# ============================================================================

class BulkDownloader:
    def __init__(self, session: AzssSession, total_timeout: float = DOWNLOAD_TOTAL_TIMEOUT) -> None:
        self.session = session
        self.total_timeout = total_timeout

    async def run(self, stamps_path: Path, ambient_path: Path) -> None:
        # Fetch base timestamp (seconds)
        try:
            raw = await self.session.read_char(BASE_TIMESTAMP_UUID)
            base_time_ms = int.from_bytes(raw, "little", signed=True)
            base_time_s = (base_time_ms + 500) // 1000
        except Exception as e:
            warn(f"Could not read base timestamp: {e}, using 0")
            base_time_s = 0

        state = DownloadState(base_time_s=base_time_s)
        stamps_path.parent.mkdir(parents=True, exist_ok=True)
        ambient_path.parent.mkdir(parents=True, exist_ok=True)

        stamps_w = AsyncCsvWriter(stamps_path, ["row", "seq", "delta_i32", "cumulative_i64", "real_i64"])
        ambient_w = AsyncCsvWriter(ambient_path, ["row", "seq", "temp_i8", "humidity_u8", "real_i64"])

        # Use a slow connection interval to avoid flooding the host controller
        #await self.session.write_char(COMMAND_UUID, bytes([BleCmd.CONN_512MS]))
        queue, on_notify = make_notify_queue()
        await self.session.start_notify(BULK_UUID, on_notify)
        await asyncio.sleep(0.8)
        await self.session.write_char(COMMAND_UUID, bytes([BleCmd.STREAM_START]))

        gc.disable()   # Prevent GC pauses from breaking the BLE link
        try:
            await self._drain(queue, state, stamps_w, ambient_w)
        finally:
            gc.enable()
            # Best‑effort cleanup
            with contextlib.suppress(Exception):
                await self.session.stop_notify(BULK_UUID)
            with contextlib.suppress(Exception):
                await self.session.write_char(COMMAND_UUID, bytes([BleCmd.STREAM_STOP]))
            if self.session.is_connected:
                with contextlib.suppress(Exception):
                    await asyncio.sleep(0.2)
                    #await self.session.write_char(COMMAND_UUID, bytes([BleCmd.CONN_512MS]))
            await stamps_w.finalize()
            await ambient_w.finalize()

        info(f"Download finished. stamps={state.stamp_rows}, ambient={state.ambient_rows}")

    async def _drain(
        self,
        queue: asyncio.Queue[bytes],
        state: DownloadState,
        stamps_w: AsyncCsvWriter,
        ambient_w: AsyncCsvWriter,
    ) -> None:
        deadline = time.monotonic() + self.total_timeout
        BATCH_SIZE = 20

        while not state.done:
            # Process queued packets in small batches, yielding after each batch
            for _ in range(BATCH_SIZE):
                try:
                    raw = queue.get_nowait()
                except asyncio.QueueEmpty:
                    break
                if not await self._process_packet(raw, state, stamps_w, ambient_w):
                    return

            # Yield to the event loop so BLE callbacks can run
            await asyncio.sleep(0)

            if state.done:
                break

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                warn("Total download timeout reached – download incomplete.")
                break

            try:
                raw = await asyncio.wait_for(queue.get(), timeout=min(remaining, 0.5))
                if not await self._process_packet(raw, state, stamps_w, ambient_w):
                    return
            except asyncio.TimeoutError:
                pass  # no data yet – loop back

    async def _process_packet(
        self,
        raw: bytes,
        state: DownloadState,
        stamps_w: AsyncCsvWriter,
        ambient_w: AsyncCsvWriter,
    ) -> bool:
        pkt = self._parse(raw)
        if pkt is None:
            return True

        if pkt.stream_id == StreamId.STAMP:
            if state.expected_seq_stamp is not None and pkt.seq != state.expected_seq_stamp:
                warn(f"Stamp seq gap: expected {state.expected_seq_stamp}, got {pkt.seq}")
            state.expected_seq_stamp = (pkt.seq + 1) & 0xFFFF
            try:
                await decode_stamp_payload(pkt.payload, pkt.seq, state, stamps_w, self.session)
            except Exception:
                return False
            if pkt.is_last:
                state.seen_last_stamp = True
        elif pkt.stream_id == StreamId.AMBIENT:
            if state.expected_seq_ambient is not None and pkt.seq != state.expected_seq_ambient:
                warn(f"Ambient seq gap: expected {state.expected_seq_ambient}, got {pkt.seq}")
            state.expected_seq_ambient = (pkt.seq + 1) & 0xFFFF
            decode_ambient_payload(pkt.payload, pkt.seq, state, ambient_w)
            if pkt.is_last:
                state.seen_last_ambient = True

        stamps_w.maybe_flush()
        ambient_w.maybe_flush()
        return True

    @staticmethod
    def _parse(packet: bytes) -> Optional[BulkPacket]:
        if len(packet) < BULK_HDR_LEN:
            return None
        return BulkPacket(
            stream_id=packet[0],
            seq=int.from_bytes(packet[1:3], "little"),
            flags=packet[3],
            payload=bytes(packet[BULK_HDR_LEN:]),
        )

# ============================================================================
# Interactive command handlers
# ============================================================================

async def cmd_scan(s: AzssSession) -> None:
    devices = await s.scan()
    if not devices:
        warn("No devices found.")
        return
    for d in devices:
        print(f"{d.name:30} {d.address}")

async def cmd_connect(s: AzssSession) -> None:
    addr = (await ainput("Address (XX:XX:XX:XX:XX:XX): ")).strip()
    if addr.lower() in {"exit", "quit"}:
        return
    try:
        addr = normalize_bt_address(addr)
    except ValueError as e:
        error(str(e))
        return
    if await s.connect(addr):
        info("Connected.")
    else:
        error("Connection failed.")

async def cmd_disconnect(s: AzssSession) -> None:
    if not s.is_connected:
        warn("Not connected.")
        return
    if await s.disconnect():
        info("Disconnected.")

async def cmd_read_count(s: AzssSession) -> None:
    data = await s.read_char(COUNT_UUID)
    count = int.from_bytes(data, "little", signed=False)
    info(f"Object count: {count}")

async def cmd_read_temp(s: AzssSession) -> None:
    raw = await s.read_char(TEMP_UUID)
    c = int.from_bytes(raw, "little", signed=True)
    info(f"Temperature: {c} °C / {9*c/5+32:.1f} °F / {c+273.15:.1f} K")

async def cmd_read_humidity(s: AzssSession) -> None:
    raw = await s.read_char(HUMIDITY_UUID)
    rh = int.from_bytes(raw, "little", signed=False)
    info(f"Humidity: {rh} %")

async def cmd_read_time(s: AzssSession) -> None:
    raw = await s.read_char(CURR_TIMESTAMP_UUID)
    ms = int.from_bytes(raw, "little", signed=True)
    info(f"Current timestamp: {ms} ms")

async def cmd_write_time(s: AzssSession) -> None:
    now_ms = int(time.time() * 1000)
    await s.write_char(BASE_TIMESTAMP_UUID, now_ms.to_bytes(8, "little", signed=True))
    info(f"Base timestamp set to {now_ms} ms")

async def cmd_write_custom_time(s: AzssSession) -> None:
    raw = (await ainput("Timestamp in milliseconds: ")).strip()
    try:
        ms = int(raw)
    except ValueError:
        error("Invalid integer.")
        return
    await s.write_char(BASE_TIMESTAMP_UUID, ms.to_bytes(8, "little", signed=True))
    info(f"Base timestamp set to {ms} ms")

async def cmd_write_command(s: AzssSession) -> None:
    raw = (await ainput("Hex command byte (e.g. 0x20): ")).strip()
    try:
        cmd = parse_hex_u8(raw)
    except ValueError as e:
        error(str(e))
        return
    await s.write_char(COMMAND_UUID, bytes([cmd]))
    reply = await s.read_char(COMMAND_UUID)
    info(f"Command 0x{cmd:02X} returned {reply[0]} (0x{reply[0]:02X})")

async def cmd_download(s: AzssSession) -> None:
    dl = BulkDownloader(s, total_timeout=DOWNLOAD_TOTAL_TIMEOUT)
    stamps_path = CSV_DIR / "stamps.csv"
    ambient_path = CSV_DIR / "ambient.csv"
    try:
        await dl.run(stamps_path, ambient_path)
    except Exception as e:
        error(f"Download failed: {e}")

# Command registry
COMMANDS: Dict[str, tuple[Callable, bool, tuple[str, ...]]] = {
    "scan":              (cmd_scan,              False, ()),
    "connect":           (cmd_connect,           False, ()),
    "disconnect":        (cmd_disconnect,        False, ()),
    "read count":        (cmd_read_count,        True,  ("read people", "read objects")),
    "read temperature":  (cmd_read_temp,         True,  ("read temp",)),
    "read humidity":     (cmd_read_humidity,     True,  ("read rh", "read humid")),
    "read time":         (cmd_read_time,         True,  ()),
    "write time":        (cmd_write_time,        True,  ("write time system",)),
    "write time custom": (cmd_write_custom_time, True,  ()),
    "write":             (cmd_write_command,     True,  ()),
    "download":          (cmd_download,          True,  ()),
}

CMD_ALIAS_MAP: Dict[str, str] = {}
for name, (_, _, aliases) in COMMANDS.items():
    CMD_ALIAS_MAP[name] = name
    for alias in aliases:
        CMD_ALIAS_MAP[alias] = name

# ============================================================================
# Interactive shell
# ============================================================================

AZSS_ART = r"""
    ___ _____      _____                            _____       _ __
   /   /__  /     / ___/___  ____  _________  _____/ ___/__  __(_) /____
  / /| | / /      \__ \/ _ \/ __ \/ ___/ __ \/ ___/\__ \/ / / / / __/ _ \
 / ___ |/ /__    ___/ /  __/ / / (__  ) /_/ / /   ___/ / /_/ / / /_/  __/
/_/  |_/____/   /____/\___/_/ /_/____/\____/_/   /____/\__,_/_/\__/\___/
"""

async def interactive_shell(session: AzssSession) -> None:
    print(AZSS_ART)
    info("Type 'help' for commands, 'exit' to quit.")
    while True:
        try:
            line = (await ainput("> ")).strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        if line in {"exit", "quit"}:
            break
        if line == "help":
            print("Available commands:", ", ".join(sorted(COMMANDS.keys())))
            continue
        canonical = CMD_ALIAS_MAP.get(line)
        if not canonical:
            warn(f"Unknown command: {line}. Try 'help'.")
            continue
        handler, needs_conn, _ = COMMANDS[canonical]
        if needs_conn and not session.is_connected:
            warn("This command requires an active connection. Run 'connect' first.")
            continue
        try:
            await handler(session)
        except Exception as e:
            error(f"Command failed: {e}")

async def main() -> None:
    async with AzssSession() as session:
        await interactive_shell(session)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nTerminated by user.")