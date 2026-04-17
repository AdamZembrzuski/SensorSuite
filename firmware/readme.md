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
| VL53L4CD ToF detection | Complete |
| SHT30 temperature / humidity | Complete |
| BLE service | Complete |
| NVM storage | Complete |
| Dynamic ToF detection interval | Planned |
| SAADC VCC monitoring | Planned |

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

| Command | Hex | Return | Description |
|---|---|---|---|
| Connection Interval 20ms | `0x00` | none | Sets new connection interval |
| Connection Interval 100ms | `0x01` | none | Sets new connection interval |
| Connection Interval 512ms | `0x02` | none | Sets new connection interval |
| Connection Interval 1024ms | `0x03` | none | Sets new connection interval |
| Download Start | `0x10` | 0 on success | Starts timestamp download notifications |
| Download Stop | `0x11` | 0 on success | Stops timestamp download notifications |
| Storage Clear | `0x13` | 0 on success | Clears NVM, including bonding, configuration, and logs |
| Firmware Version | `0x20` |int (eg `12` for V1.2)| Returns FW Version |
| Active Period 08-16 | `0x30` |  0 on success | Sets the device to only detect <sup>¶</sup> between 08:00 and 16:00 |
| Active Period 08-18 | `0x31` |  0 on success | Sets the device to only detect between 08:00 and 18:00 |
| Active Period 09-21 | `0x32` |  0 on success | Sets the device to only detect between 09:00 and 21:00 |
| Active Period 00-24 | `0x33` | 0 on success | Sets the device to detect irrespective of time (not date) |
| Inactive Weekend | `0x34` | 0 on success | Sets the device to not detect during the weekend |
| Active Weekend | `0x35` | 0 on success | Sets the device to detect during the weekend |

*¶ Detect refers to human/object detection, ambient detection is still carried out*

### Data Collection
The device only collects timestamp logs if it is within the active period, definable via the 0x3- commands. The device defaults to 24/7 operation on reset.

Ambient logs are collected regardless of the system state (if the SHT3x is enabled in `prj.conf`).

### Logging

*Logging described in this section is independent of sensor logging*

RTT logging is enableable in `prj.conf`. Connect via the nRF Connect for Desktop 
terminal or a J-Link RTT Viewer.

### Kconfig Options
All Kconfig options can be adjusted in `firmware/prj.conf`. If changes are not being applied, ensure `prj.conf` is included in the build config.

#### System Level
<details>
<summary>For lowest current consumption disable all logging, this will reduce current consumption by roughly 200μA</summary>

  
```
CONFIG_USE_SEGGER_RTT=n
CONFIG_SERIAL=n

CONFIG_LOG=n
CONFIG_LOG_BACKEND_RTT=n
CONFIG_LOG_BACKEND_UART=n

CONFIG_PRINTK=n
CONFIG_BOOT_BANNER=n

CONFIG_UART_CONSOLE=n
CONFIG_RTT_CONSOLE=n
```

</details>
<details>
<summary>Bluetooth options such as device name and default connection interval are available to be defined statically.</summary>
  
```
CONFIG_BT_DEVICE_NAME="AZSensorSuite-001"
```
</details>

#### Application Level


