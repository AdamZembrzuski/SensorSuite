## Software

This folder contains the host-side Python client for communicating with 
AZSensorSuite over BLE.

**Python 3.7 or higher is required.**

> [!WARNING]
> The software in this folder has been heavily vibecoded. Expect unreliability. (A human rewrite is planned)

##

### Overview

The intended data flow is:
```
Device (BLE) → azss_client.py → csvs/ → azss_train.py → model → azss_infer.py → prediction
```

The BLE client is currently implemented. Model training and infer scripts 
are in progress.

## azss_client.py
### Setup
```bash
pip install -r requirements.txt
```

> [!NOTE]
> On macOS, Bleak exposes CoreBluetooth UUIDs rather than MAC addresses. 
> When prompted for a Bluetooth address, use the UUID string instead.

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
| `connect` | No | Connect to a device by Bluetooth address |
| `disconnect` | No | Disconnect from the current device |
| `unpair` | No | Unpair the current device |
| `read uuid` | Yes | Read a characteristic by UUID |
| `read count` | Yes | Read detected object count since last initialisation |
| `read temp` | Yes | Read temperature (returns °C, °F, and K) |
| `read humidity` | Yes | Read relative humidity |
| `read time` | Yes | Read the current device timestamp in milliseconds |
| `write` | Yes | Send a raw hex command byte to the Command characteristic |
| `write time` | Yes | Sync device base timestamp to system time |
| `write time custom` | Yes | Write a custom base timestamp in milliseconds |
| `download` | Yes | Download the full timestamp and ambient log |

### Download Output

Data is written to the `csvs/` folder, created automatically if it does not exist.

**`stamps.csv`** — ToF event log

| Column | Description |
|---|---|
| `row` | Row index |
| `seq` | BLE packet sequence number |
| `delta_i32` | Time delta from previous event in seconds |
| `cumulative_i64` | Cumulative elapsed seconds since base timestamp |
| `real_i64` | Absolute Unix timestamp in seconds |

**`ambient.csv`** — Temperature and humidity log (sampled every 15 minutes)

| Column | Description |
|---|---|
| `row` | Row index |
| `seq` | BLE packet sequence number |
| `temp_i8` | Temperature in °C |
| `humidity_u8` | Relative humidity as a percentage |
| `real_i64` | Absolute Unix timestamp in seconds |

## azss_train.py

## azss_infer.py
