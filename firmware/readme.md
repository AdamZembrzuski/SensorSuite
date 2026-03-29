## Firmware

This folder contains all firmware source files and documentation. The latest firmware 
has been verified working on PCB V1.02. The SHT30 implementation has been tested 
separately on the Development Kit.

For build and flash instructions, see the 
[main README](../README.md#example-quickstart-guide).
##

### Implementation Status

| Feature | Status |
|---|---|
| VL53L4CD ToF ranging | Complete |
| SHT30 temperature / humidity | Complete |
| BLE service | Complete |
| NVM storage | In development |
| Firmware over the air | Planned |
| Mesh Networking | Planned |

### BLE Service

The device exposes a custom BLE service with the following characteristics:

Service UUID : `229a0001-ad33-4a06-9bce-c34201743655`

| Characteristic | UUID | Properties | Encryption | Description |
|---|---|---|---|---|
| Count | `229a0002-ad33-4a06-9bce-c34201743655` | Notify, Read | None | Returns detected object count |
| Temperature | `229a0003-ad33-4a06-9bce-c34201743655` | Notify, Read | None | Returns last good temperature |
| Humidity | `229a0004-ad33-4a06-9bce-c34201743655` | Notify, Read | None | Returns last good humidity |
| Bulk Download | `229a0005-ad33-4a06-9bce-c34201743655` | Notify | Level 2 | Used to download timestamp log |
| Current Timestamp | `229a0006-ad33-4a06-9bce-c34201743655` | Read | None | Returns current timestamp |
| Base Timestamp | `229a0007-ad33-4a06-9bce-c34201743655` | Write, Read | Level 2 | Used to write base timestamp |
| Command | `229a0008-ad33-4a06-9bce-c34201743655` | Write, Read | Level 2 | Used to call one of many commands |

### 'Command' Commands

| Command | Hex | Return |
|---|---|---|
| Connection Interval 20ms | `0x00` | 0 on success |
| Connection Interval 100ms | `0x01` | 0 on success |
| Connection Interval 512ms | `0x02` | 0 on success |
| Connection Interval 1024ms | `0x03` | 0 on success |
| Download Start | `0x10` | 0 on success |
| Download Stop | `0x11` | 0 on success |
| Firmware Version | `0x20` | FW Version as int (eg `12` for V1.2)|
| Active Period 08-16 | `0x30` |  0 on success |
| Active Period 08-18 | `0x31` |  0 on success |
| Active Period 09-21 | `0x32` |  0 on success |
| Active Period 00-24 | `0x33` | 0 on success |
| Inactive Weekend | `0x34` | 0 on success |
| Active Weekend | `0x35` | 0 on success |

### Data Collection
The device only collects timestamp logs if it is within the active period, definable via the 0x3- commands. The device defaults to 24/7 operation on reset.

Ambient logs are collected regardless of the system state (if they are enabled).

### Logging

*Logging in this section is independent of sensor logging*

RTT logging is enabled by default. Connect via the nRF Connect for Desktop 
terminal or a J-Link RTT Viewer.

### Key Kconfig Options
For lowest current consumption **disable all logging** in `prj.conf` this will reduce current consumption by roughly 200μA.<br>
Key Bluetooth settings such as device name or connection settings can be adjusted in `prj.conf`.
