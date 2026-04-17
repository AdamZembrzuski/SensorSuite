from __future__ import annotations

import asyncio
import contextlib
import csv
import enum
import random
import re
import time
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Coroutine, Final, Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice


# ---------------------------------------------------------------------------
# Stream / protocol constants
# ---------------------------------------------------------------------------

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


BULK_FLAG_LAST: Final[int] = 0x01
BULK_HDR_LEN:   Final[int] = 4

AMBIENT_INTERVAL_S: Final[int] = 15 * 60

# 315_360_000 s ≈ 10 years.  Deltas this large indicate a counter rollover
# or a first-boot sentinel value from the firmware, not real elapsed time.
_DELTA_ROLLOVER_THRESHOLD_S: Final[int] = 10 * 365 * 24 * 3600

# ---------------------------------------------------------------------------
# GATT UUIDs
# ---------------------------------------------------------------------------

BT_SERVICE_UUID       = "229a0001-ad33-4a06-9bce-c34201743655"
BT_COUNT_UUID         = "229a0002-ad33-4a06-9bce-c34201743655"
BT_TEMP_UUID          = "229a0003-ad33-4a06-9bce-c34201743655"
BT_HUMIDITY_UUID      = "229a0004-ad33-4a06-9bce-c34201743655"
BT_BULK_UUID          = "229a0005-ad33-4a06-9bce-c34201743655"
BT_CURR_TIMESTAMP_UUID = "229a0006-ad33-4a06-9bce-c34201743655"
BT_BASE_TIMESTAMP_UUID = "229a0007-ad33-4a06-9bce-c34201743655"
BT_COMMAND_UUID       = "229a0008-ad33-4a06-9bce-c34201743655"

# ---------------------------------------------------------------------------
# Scan / connect defaults
# ---------------------------------------------------------------------------

DEFAULT_SCAN_TIMEOUT_S      = 15
DEFAULT_CONNECT_TIMEOUT_S   = 45
DEFAULT_CONNECT_RETRIES     = 3
DEFAULT_POST_CONNECT_SETTLE_S = 0.5

# ---------------------------------------------------------------------------
# Branding
# ---------------------------------------------------------------------------

AZSS_ART = r"""
    ___ _____      _____                            _____       _ __
   /   /__  /     / ___/___  ____  _________  _____/ ___/__  __(_) /____
  / /| | / /      \__ \/ _ \/ __ \/ ___/ __ \/ ___/\__ \/ / / / / __/ _ \
 / ___ |/ /__    ___/ /  __/ / / (__  ) /_/ / /   ___/ / /_/ / / /_/  __/
/_/  |_/____/   /____/\___/_/ /_/____/\____/_/   /____/\__,_/_/\__/\___/
"""

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------


def info(msg: str) -> None:
    print(f"[INFO] {msg}")


def warn(msg: str) -> None:
    print(f"[WARN] {msg}")


def error(msg: str) -> None:
    print(f"[ERROR] {msg}")


def format_exc(exc: BaseException) -> str:
    return f"{type(exc).__name__}: {exc}"


# ---------------------------------------------------------------------------
# Async input
# ---------------------------------------------------------------------------


async def ainput(prompt: str = "") -> str:
    """Read a line from stdin without blocking the event loop.

    EOFError and KeyboardInterrupt are *not* caught here; callers that want
    graceful handling should catch them at the appropriate level.
    """
    return await asyncio.to_thread(input, prompt)


# ---------------------------------------------------------------------------
# Input validators
# ---------------------------------------------------------------------------

_HEX_U8_RE = re.compile(r"^(?:0x)?([0-9a-fA-F]{1,2})$")

# On Linux/Windows, Bleak exposes classic MAC addresses (XX:XX:XX:XX:XX:XX).
# On macOS, Bleak exposes CoreBluetooth UUIDs instead; if you are targeting
# macOS you will need to accept UUID strings here as well.
_BT_ADDR_RE = re.compile(r"^([0-9A-Fa-f]{2}:){5}([0-9A-Fa-f]{2})$")


def parse_hex_u8(text: str) -> int:
    match = _HEX_U8_RE.match(text.strip())
    if not match:
        raise ValueError("Expected a hex byte such as '2A' or '0x2A'.")
    return int(match.group(1), 16)


def normalize_bt_address(text: str) -> str:
    text = text.strip().upper()
    if not _BT_ADDR_RE.match(text):
        raise ValueError("Bluetooth address must be in the form XX:XX:XX:XX:XX:XX.")
    return text


async def prompt_bt_address() -> Optional[str]:
    while True:
        raw = (await ainput("Bluetooth address (XX:XX:XX:XX:XX:XX, or 'exit'): ")).strip()
        if raw.lower() in {"exit", "quit"}:
            return None
        try:
            return normalize_bt_address(raw)
        except ValueError as exc:
            warn(str(exc))


async def prompt_uuid() -> Optional[str]:
    while True:
        raw = (await ainput("128-bit UUID (or 'exit'): ")).strip()
        if raw.lower() in {"exit", "quit"}:
            return None
        try:
            return str(uuid.UUID(raw))
        except ValueError:
            warn("Invalid UUID format.")


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class SeenDevice:
    name: str
    address: str
    device: BLEDevice


@dataclass(frozen=True, slots=True)
class BulkPacket:
    stream_id: int
    seq: int
    flags: int
    payload: bytes

    @property
    def is_last(self) -> bool:
        return bool(self.flags & BULK_FLAG_LAST)


@dataclass(slots=True)
class BulkState:
    seen_last_stamp:   bool      = False
    seen_last_ambient: bool      = False
    stamp_buf:         bytearray = field(default_factory=bytearray)
    ambient_buf:       bytearray = field(default_factory=bytearray)
    stamp_cumulative_s: int      = 0
    stamp_rows:        int       = 0
    ambient_rows:      int       = 0

    @property
    def done(self) -> bool:
        return self.seen_last_stamp and self.seen_last_ambient


# ---------------------------------------------------------------------------
# BLE session
# ---------------------------------------------------------------------------


class AzssBleSession:
    def __init__(self) -> None:
        self._client: Optional[BleakClient] = None
        self._device: Optional[BLEDevice] = None
        self._scan_cache: dict[str, SeenDevice] = {}
        self._disconnect_event = asyncio.Event()
        self._gatt_lock = asyncio.Lock()
        self._expect_disconnect = False

    # -- Context manager -----------------------------------------------------

    async def __aenter__(self) -> "AzssBleSession":
        return self

    async def __aexit__(self, *_: object) -> None:
        if self.is_connected():
            await self.disconnect()

    # -- State helpers --------------------------------------------------------

    @property
    def client(self) -> BleakClient:
        if self._client is None:
            raise RuntimeError("No active BLE client.")
        return self._client

    def is_connected(self) -> bool:
        return bool(self._client and self._client.is_connected)

    def connected_address(self) -> Optional[str]:
        return self._device.address.upper() if self._device else None

    # -- Scanning -------------------------------------------------------------

    async def scan(self, timeout_s: float = DEFAULT_SCAN_TIMEOUT_S) -> list[SeenDevice]:
        devices = await BleakScanner.discover(timeout=timeout_s)
        entries: list[SeenDevice] = []
        cache: dict[str, SeenDevice] = {}
        for dev in devices:
            entry = SeenDevice(
                name=dev.name or "(no name)",
                address=dev.address.upper(),
                device=dev,
            )
            entries.append(entry)
            cache[entry.address] = entry
        self._scan_cache = cache
        return entries

    async def find_device(
        self,
        address: str,
        scan_timeout_s: float = DEFAULT_SCAN_TIMEOUT_S,
        *,
        prefer_fresh_scan: bool = False,
    ) -> Optional[BLEDevice]:
        address = normalize_bt_address(address)

        if not prefer_fresh_scan:
            cached = self._scan_cache.get(address)
            if cached is not None:
                return cached.device

        entries = await self.scan(timeout_s=scan_timeout_s)
        for entry in entries:
            if entry.address == address:
                return entry.device

        cached = self._scan_cache.get(address)
        return cached.device if cached is not None else None

    # -- Connect / disconnect -------------------------------------------------

    async def connect(
        self,
        address: str,
        *,
        retries: int = DEFAULT_CONNECT_RETRIES,
        connect_timeout_s: float = DEFAULT_CONNECT_TIMEOUT_S,
        scan_timeout_s: float = DEFAULT_SCAN_TIMEOUT_S,
        settle_s: float = DEFAULT_POST_CONNECT_SETTLE_S,
    ) -> bool:
        address = normalize_bt_address(address)

        if self.is_connected():
            if self.connected_address() == address:
                info(f"Already connected to {address}")
                return True
            await self.disconnect()

        device = await self.find_device(
            address,
            scan_timeout_s=scan_timeout_s,
            prefer_fresh_scan=True,
        )
        if device is None:
            error("Device not found during scan. Make sure the peripheral is advertising.")
            return False

        exc: BaseException
        for attempt in range(1, retries + 1):
            self._disconnect_event.clear()
            try:
                def on_disconnect(_client: BleakClient) -> None:
                    self._disconnect_event.set()
                    if not self._expect_disconnect:
                        warn("Peripheral disconnected.")
                    self._expect_disconnect = False

                client = BleakClient(
                    device,
                    disconnected_callback=on_disconnect,
                    timeout=connect_timeout_s,
                )

                info(f"Connecting to {address} (attempt {attempt}/{retries})...")
                await client.connect()
                if not client.is_connected:
                    raise RuntimeError("connect() returned, but the link is not up.")

                self._client = client
                self._device = device

                await asyncio.sleep(settle_s)
                if not client.is_connected:
                    raise RuntimeError("Disconnected during post-connect settle period.")

                # Trigger BlueZ's security manager by accessing an encrypted characteristic.
                # BlueZ handles pairing and LTK re-use automatically — no explicit pair() needed.
                info("Securing connection...")
                try:
                    await self.read_char(BT_BASE_TIMESTAMP_UUID)
                except Exception as e:
                    raise RuntimeError(f"Failed to secure connection: {format_exc(e)}")

                return True

            except Exception as e:
                exc = e
                warn(f"Connect attempt failed: {format_exc(e)}")
                with contextlib.suppress(Exception):
                    if self._client is not None:
                        self._expect_disconnect = True
                        await self._client.disconnect()
                self._client = None
                self._device = None
                await asyncio.sleep(0.5 * (2 ** (attempt - 1)) + random.uniform(0, 0.3))

        error(f"Failed to connect after {retries} attempts: {format_exc(exc)}")
        return False

    async def disconnect(self) -> bool:
        if self._client is None:
            return False
        try:
            self._expect_disconnect = True
            await self._client.disconnect()
            return True
        except Exception as exc:
            warn(f"Disconnect raised: {format_exc(exc)}")
            return False
        finally:
            self._client = None
            self._device = None
            self._disconnect_event.set()

    async def unpair(self) -> bool:
        if self._client is None:
            warn("No active Bleak client. Connect first, then unpair.")
            return False
        if not hasattr(self._client, "unpair"):
            warn("This Bleak backend does not support unpair().")
            return False
        try:
            self._expect_disconnect = True
            await self._client.unpair()
            info("Unpaired device.")
            return True
        except Exception as exc:
            error(f"Unpair failed: {format_exc(exc)}")
            return False
        finally:
            self._client = None
            self._device = None
            self._disconnect_event.set()

    # -- GATT operations ------------------------------------------------------

    async def ensure_connected(self) -> None:
        if self._client is None or not self._client.is_connected:
            raise RuntimeError("Not connected.")

    async def read_char(self, uuid_str: str, *, retries: int = 3, retry_delay_s: float = 0.12) -> bytes:
        exc: BaseException
        for attempt in range(1, retries + 1):
            try:
                await self.ensure_connected()
                async with self._gatt_lock:
                    data = await self.client.read_gatt_char(uuid_str)
                if not data:
                    raise RuntimeError("Empty read.")
                return bytes(data)
            except Exception as e:
                exc = e
                if attempt < retries:
                    await asyncio.sleep(retry_delay_s * attempt)
        raise exc

    async def write_char(
        self,
        uuid_str: str,
        data: bytes,
        *,
        response: bool = True,
        retries: int = 3,
        retry_delay_s: float = 0.12,
    ) -> None:
        exc: BaseException
        for attempt in range(1, retries + 1):
            try:
                await self.ensure_connected()
                async with self._gatt_lock:
                    await self.client.write_gatt_char(uuid_str, data, response=response)
                return
            except Exception as e:
                exc = e
                if attempt < retries:
                    await asyncio.sleep(retry_delay_s * attempt)
        raise exc

    async def start_notify(self, uuid_str: str, callback: Callable[[int, bytearray], None]) -> None:
        await self.ensure_connected()
        async with self._gatt_lock:
            await self.client.start_notify(uuid_str, callback)

    async def stop_notify(self, uuid_str: str) -> None:
        if self._client is None or not self._client.is_connected:
            return
        async with self._gatt_lock:
            await self.client.stop_notify(uuid_str)


# ---------------------------------------------------------------------------
# Packet helpers
# ---------------------------------------------------------------------------


def parse_bulk_packet(packet: bytes) -> Optional[BulkPacket]:
    if len(packet) < BULK_HDR_LEN:
        return None
    return BulkPacket(
        stream_id=packet[0],
        seq=int.from_bytes(packet[1:3], "little"),
        flags=packet[3],
        payload=bytes(packet[BULK_HDR_LEN:]),
    )


def make_notify_queue() -> tuple[asyncio.Queue[bytes], Callable[[int, bytearray], None]]:
    queue: asyncio.Queue[bytes] = asyncio.Queue()
    loop = asyncio.get_running_loop()

    # call_soon_threadsafe is used deliberately: on Windows (and some Linux
    # Bleak backends) notification callbacks arrive from a different OS thread.
    # Using call_soon_threadsafe is safe on all platforms and avoids a subtle
    # data race that would appear only on Windows.
    def on_notify(_sender: int, data: bytearray) -> None:
        loop.call_soon_threadsafe(queue.put_nowait, bytes(data))

    return queue, on_notify


# ---------------------------------------------------------------------------
# Payload decoders
# ---------------------------------------------------------------------------


async def decode_stamp_payload(
    payload: bytes,
    seq: int,
    state: BulkState,
    writer: csv.writer,
    base_time_s: int,
    session: AzssBleSession,
) -> None:
    if payload:
        state.stamp_buf.extend(payload)
    while len(state.stamp_buf) >= 4:
        delta_s = int.from_bytes(state.stamp_buf[:4], "little", signed=True)
        del state.stamp_buf[:4]

        if abs(delta_s) >= _DELTA_ROLLOVER_THRESHOLD_S:
            try:
                current_s = int.from_bytes(
                    await session.read_char(BT_CURR_TIMESTAMP_UUID),
                    "little",
                    signed=True,
                ) // 1000
            except Exception as exc:
                warn(f"Could not recover timestamp delta: {format_exc(exc)}")
                continue
            delta_s = current_s - (base_time_s + state.stamp_cumulative_s)

        state.stamp_cumulative_s += delta_s
        writer.writerow([
            state.stamp_rows,
            seq,
            delta_s,
            state.stamp_cumulative_s,
            base_time_s + state.stamp_cumulative_s,
        ])
        state.stamp_rows += 1


async def decode_ambient_payload(
    payload: bytes,
    seq: int,
    state: BulkState,
    writer: csv.writer,
    base_time_s: int,
) -> None:
    if payload:
        state.ambient_buf.extend(payload)
    while len(state.ambient_buf) >= 2:
        temp_c = int.from_bytes(bytes([state.ambient_buf[0]]), "little", signed=True)
        humidity_rh = state.ambient_buf[1]
        del state.ambient_buf[:2]
        writer.writerow([
            state.ambient_rows,
            seq,
            temp_c,
            humidity_rh,
            base_time_s + AMBIENT_INTERVAL_S * state.ambient_rows,
        ])
        state.ambient_rows += 1


# ---------------------------------------------------------------------------
# Download helpers
# ---------------------------------------------------------------------------


async def _read_base_time(session: AzssBleSession) -> int:
    """Return the firmware base timestamp rounded to whole seconds."""
    raw = await session.read_char(BT_BASE_TIMESTAMP_UUID)
    return (int.from_bytes(raw, "little", signed=True) + 500) // 1000


@asynccontextmanager
async def _bulk_stream(session: AzssBleSession):
    """Switch to the fast connection interval, arm notifications, trigger
    streaming, and restore everything on exit regardless of how we leave."""
    queue, on_notify = make_notify_queue()
    await session.write_char(BT_COMMAND_UUID, bytes([BleCmd.CONN_20MS]), response=True)
    await session.start_notify(BT_BULK_UUID, on_notify)
    await asyncio.sleep(0.8)
    await session.write_char(BT_COMMAND_UUID, bytes([BleCmd.STREAM_START]), response=True)
    try:
        yield queue
    finally:
        with contextlib.suppress(Exception):
            await session.stop_notify(BT_BULK_UUID)
        with contextlib.suppress(Exception):
            await session.write_char(BT_COMMAND_UUID, bytes([BleCmd.STREAM_STOP]), response=True)
        if session.is_connected():
            with contextlib.suppress(Exception):
                await asyncio.sleep(0.2)
                await session.write_char(BT_COMMAND_UUID, bytes([BleCmd.CONN_512MS]), response=True)


async def _consume_packets(
    queue: asyncio.Queue[bytes],
    state: BulkState,
    stamps_writer: csv.writer,
    ambient_writer: csv.writer,
    stamps_file,
    ambient_file,
    base_time_s: int,
    session: AzssBleSession,
    inactivity_timeout_s: float,
) -> bool:
    """Drain bulk packets until both streams signal done or a timeout fires.

    Returns True if we timed out without seeing the last-packet flag.
    """
    timed_out = False
    while not state.done:
        try:
            packet = await asyncio.wait_for(queue.get(), timeout=inactivity_timeout_s)
        except asyncio.TimeoutError:
            timed_out = True
            break

        parsed = parse_bulk_packet(packet)
        if parsed is None:
            continue

        if parsed.stream_id == StreamId.STAMP:
            await decode_stamp_payload(
                parsed.payload, parsed.seq, state, stamps_writer, base_time_s, session
            )
            if parsed.is_last:
                state.seen_last_stamp = True
        elif parsed.stream_id == StreamId.AMBIENT:
            await decode_ambient_payload(
                parsed.payload, parsed.seq, state, ambient_writer, base_time_s
            )
            if parsed.is_last:
                state.seen_last_ambient = True

        if state.stamp_rows and (state.stamp_rows % 128) == 0:
            stamps_file.flush()
        if state.ambient_rows and (state.ambient_rows % 128) == 0:
            ambient_file.flush()

    return timed_out


# ---------------------------------------------------------------------------
# CLI command handlers
# ---------------------------------------------------------------------------


async def cmd_scan(session: AzssBleSession) -> None:
    info("Scanning for BLE devices...")
    try:
        devices = await session.scan()
    except Exception as exc:
        error(f"Scan failed: {format_exc(exc)}")
        return
    if not devices:
        warn("No devices found.")
        return
    for entry in devices:
        print(f"{entry.name} {entry.address}")


async def cmd_connect(session: AzssBleSession) -> None:
    info("Enter the peripheral Bluetooth address. Run 'scan' if needed.")
    address = await prompt_bt_address()
    if address is None:
        warn("Cancelled.")
        return
    ok = await session.connect(address)
    if ok:
        info(f"Connected to {address}")
    else:
        error(f"Could not connect to {address}")


async def cmd_disconnect(session: AzssBleSession) -> None:
    if not session.is_connected():
        warn("Not connected.")
        return
    if await session.disconnect():
        info("Disconnected.")
    else:
        error("Disconnect failed.")


async def cmd_read_uuid(session: AzssBleSession) -> None:
    uuid_str = await prompt_uuid()
    if uuid_str is None:
        warn("Cancelled.")
        return
    try:
        data = await session.read_char(uuid_str)
        value = int.from_bytes(data, "little", signed=False)
        info(f"Value: {value} from {uuid_str}")
    except Exception as exc:
        error(f"Read failed: {format_exc(exc)}")


async def cmd_read_count(session: AzssBleSession) -> None:
    try:
        value = int.from_bytes(await session.read_char(BT_COUNT_UUID), "little", signed=False)
        info(f"Object count since last initialisation: {value}")
    except Exception as exc:
        error(f"Read count failed: {format_exc(exc)}")


async def cmd_read_temp(session: AzssBleSession) -> None:
    try:
        raw = await session.read_char(BT_TEMP_UUID)
        value_c = int.from_bytes(raw, "little", signed=True)
        info(f"Temperature: {value_c} °C | {(9 * value_c / 5) + 32:.1f} °F | {value_c + 273.15:.1f} K")
    except Exception as exc:
        error(f"Read temperature failed: {format_exc(exc)}")


async def cmd_read_humidity(session: AzssBleSession) -> None:
    try:
        value = int.from_bytes(await session.read_char(BT_HUMIDITY_UUID), "little", signed=False)
        info(f"Relative humidity: {value}%")
    except Exception as exc:
        error(f"Read humidity failed: {format_exc(exc)}")


async def cmd_read_time(session: AzssBleSession) -> None:
    try:
        value_ms = int.from_bytes(await session.read_char(BT_CURR_TIMESTAMP_UUID), "little", signed=True)
        info(f"Current timestamp: {value_ms} ms")
    except Exception as exc:
        error(f"Read timestamp failed: {format_exc(exc)}")


async def cmd_write_command(session: AzssBleSession) -> None:
    raw = (await ainput("Hex command byte (e.g. 2A or 0x2A): ")).strip()
    try:
        command = parse_hex_u8(raw)
    except ValueError as exc:
        error(str(exc))
        return
    try:
        await session.write_char(BT_COMMAND_UUID, bytes([command]), response=True)
        await asyncio.sleep(0.01)
        # Single read_char with its built-in retry — no second retry loop needed.
        result_bytes = await session.read_char(BT_COMMAND_UUID)
        result = result_bytes[0]
        info(f"Command returned {result} (0x{result:02X})")
    except Exception as exc:
        error(f"Command transaction failed: {format_exc(exc)}")


async def cmd_write_time_system(session: AzssBleSession) -> None:
    current_ms = time.time_ns() // 1_000_000
    try:
        await session.write_char(
            BT_BASE_TIMESTAMP_UUID,
            current_ms.to_bytes(8, "little", signed=True),
            response=True,
        )
        returned_ms = int.from_bytes(await session.read_char(BT_BASE_TIMESTAMP_UUID), "little", signed=True)
        info(f"Base timestamp set to {returned_ms} ms")
    except Exception as exc:
        error(f"Write/read timestamp failed: {format_exc(exc)}")


async def cmd_write_time_custom(session: AzssBleSession) -> None:
    raw = (await ainput("Timestamp in milliseconds: ")).strip()
    try:
        current_ms = int(raw)
    except ValueError:
        error("Please enter a valid integer timestamp.")
        return
    try:
        await session.write_char(
            BT_BASE_TIMESTAMP_UUID,
            current_ms.to_bytes(8, "little", signed=True),
            response=True,
        )
        returned_ms = int.from_bytes(await session.read_char(BT_BASE_TIMESTAMP_UUID), "little", signed=True)
        info(f"Base timestamp set to {returned_ms} ms")
    except Exception as exc:
        error(f"Write/read timestamp failed: {format_exc(exc)}")


CSV_DIR: Final[Path] = Path("csvs")


async def cmd_download(
    session: AzssBleSession,
    stamps_csv_path: Optional[Path] = None,
    ambient_csv_path: Optional[Path] = None,
    inactivity_timeout_s: float = 30.0,
) -> None:
    if not session.is_connected():
        error("Not connected.")
        return

    CSV_DIR.mkdir(parents=True, exist_ok=True)
    stamps_path  = stamps_csv_path  or CSV_DIR / "stamps.csv"
    ambient_path = ambient_csv_path or CSV_DIR / "ambient.csv"

    try:
        base_time_s = await _read_base_time(session)
    except Exception as exc:
        warn(f"Could not read base timestamp: {format_exc(exc)}")
        base_time_s = 0

    try:
        with (
            stamps_path.open("w", newline="") as stamps_file,
            ambient_path.open("w", newline="") as ambient_file,
        ):
            stamps_writer = csv.writer(stamps_file)
            ambient_writer = csv.writer(ambient_file)
            stamps_writer.writerow(["row", "seq", "delta_i32", "cumulative_i64", "real_i64"])
            ambient_writer.writerow(["row", "seq", "temp_i8", "humidity_u8", "real_i64"])

            state = BulkState()
            async with _bulk_stream(session) as queue:
                timed_out = await _consume_packets(
                    queue, state,
                    stamps_writer, ambient_writer,
                    stamps_file, ambient_file,
                    base_time_s, session, inactivity_timeout_s,
                )

        info(
            f"Download complete. stamp_rows={state.stamp_rows}, "
            f"ambient_rows={state.ambient_rows}, timed_out={timed_out}\n"
            f"  stamps  -> {stamps_path}\n"
            f"  ambient -> {ambient_path}"
        )
    except Exception as exc:
        error(f"Download failed: {format_exc(exc)}")


# ---------------------------------------------------------------------------
# Command dispatch table
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class _Command:
    handler: Callable[["AzssBleSession"], Coroutine]
    aliases: tuple[str, ...] = ()
    needs_connection: bool = False


_COMMANDS: dict[str, _Command] = {
    "scan":              _Command(cmd_scan),
    "connect":           _Command(cmd_connect),
    "disconnect":        _Command(cmd_disconnect),
    "unpair":            _Command(lambda s: s.unpair()),
    "read uuid":         _Command(cmd_read_uuid,         needs_connection=True),
    "read count":        _Command(cmd_read_count,        needs_connection=True,
                                  aliases=("read people", "read objects")),
    "read temperature":  _Command(cmd_read_temp,         needs_connection=True,
                                  aliases=("read temp",)),
    "read humidity":     _Command(cmd_read_humidity,     needs_connection=True,
                                  aliases=("read rh", "read humid")),
    "read time":         _Command(cmd_read_time,         needs_connection=True),
    "write":             _Command(cmd_write_command,     needs_connection=True),
    "write time":        _Command(cmd_write_time_system, needs_connection=True,
                                  aliases=("write time system",)),
    "write time custom": _Command(cmd_write_time_custom, needs_connection=True),
    "download":          _Command(cmd_download,          needs_connection=True),
}

# Flatten aliases into a single lookup dict at import time.
_CMD_LOOKUP: dict[str, str] = {}
for _name, _cmd in _COMMANDS.items():
    _CMD_LOOKUP[_name] = _name
    for _alias in _cmd.aliases:
        _CMD_LOOKUP[_alias] = _name


async def run_command(command: str, session: AzssBleSession) -> bool:
    command = command.strip().lower()
    if not command:
        return True

    if command in {"exit", "quit", "stop", "terminate"}:
        if session.is_connected():
            await session.disconnect()
        return False

    if command == "help":
        info(
            "Commands: "
            + ", ".join(sorted(_COMMANDS))
            + ", exit"
        )
        return True

    canonical = _CMD_LOOKUP.get(command)
    if canonical is None:
        warn("Unknown command. Type 'help' for a list of commands.")
        return True

    cmd = _COMMANDS[canonical]
    if cmd.needs_connection and not session.is_connected():
        warn(f"'{canonical}' requires an active connection. Run 'connect' first.")
        return True

    try:
        await cmd.handler(session)
    except Exception as exc:
        error(format_exc(exc))
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


async def main() -> None:
    print(AZSS_ART)
    print(
        """
v1.00 © 2026 Adam Zembrzuski.

Licensed under the TAPR Open Hardware License (OHL).
This software and associated design files may be used, modified,
and redistributed under the terms of the license.

See the LICENSE file for full terms.
"""
    )

    await asyncio.sleep(0.2)

    async with AzssBleSession() as session:
        running = True
        while running:
            try:
                command = (await ainput("> ")).strip()
            except EOFError:
                break
            except KeyboardInterrupt:
                break
            running = await run_command(command, session)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[INFO] Terminated by user")
