## Software

The host-side Python client reads live values from AZSensorSuite over BLE and exports ToF and ambient logs to CSV.

> [!WARNING]
> This client was written quickly to test the firmware and is due for a rewrite. Please expect and report bugs.

## azss_client.py

### Setup

Use Python 3.10 or newer for the current [Bleak dependency](https://pypi.org/project/bleak/). Run these commands from the `software` directory.

```bash
python -m pip install -r requirements.txt
```

> [!NOTE]
> `connect` requires a Bluetooth MAC address. On macOS, Bleak returns a CoreBluetooth UUID, which the current client rejects. Use the client on a host where Bleak exposes MAC addresses.

### Usage
```bash
python azss_client.py
```

The client runs as an interactive shell. Type `help` for a list of commands 
or `exit` to quit.

### Commands

| Command | Requires Connection | Description |
|---|---|---|
| `scan` | No | Scan for nearby BLE devices |
| `connect` | No | Connect to a device by Bluetooth MAC address |
| `disconnect` | No | Disconnect from the current device |
| `read count` | Yes | Read the number of retained ToF events |
| `read temperature` (`read temp`) | Yes | Read temperature (returns °C, °F, and K) |
| `read humidity` | Yes | Read relative humidity |
| `read ambient` | Yes | Display the latest cached temperature and relative humidity |
| `sample ambient` | Yes | Request a fresh SHT30 reading and display temperature and relative humidity |
| `read time` | Yes | Read the current device timestamp in milliseconds |
| `write` | Yes | Send a raw hex command byte to the Command characteristic |
| `write time` | Yes | Sync device base timestamp to system time |
| `write time custom` | Yes | Write a custom base timestamp in milliseconds |
| `download` | Yes | Download the ToF and ambient logs |

### Download Output

The client creates `csvs/` in the current working directory and overwrites the two output files on each download. Copy or rename any files you want to keep before downloading again. The firmware drains entries that are still in RAM during a download, so save the CSVs if you need those entries later.

**`stamps.csv`** — ToF event log

| Column | Description |
|---|---|
| `row` | Row index |
| `seq` | BLE packet sequence number |
| `delta_i32` | Time delta from previous event in seconds |
| `cumulative_i64` | Cumulative elapsed seconds since base timestamp |
| `real_i64` | Absolute Unix timestamp in seconds |

**`ambient.csv`** — Temperature and humidity log. The client estimates timestamps from the base timestamp using a fixed 15-minute interval. Pauses in sampling or a custom firmware sampling interval can make these timestamps differ from the actual sample times.

| Column | Description |
|---|---|
| `row` | Row index |
| `seq` | BLE packet sequence number |
| `temp_i8` | Temperature in °C |
| `humidity_u8` | Relative humidity as a percentage |
| `real_i64` | Estimated Unix timestamp in seconds |
