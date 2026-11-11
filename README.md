# AZSensorSuite

[![License: TAPR-OHL](https://img.shields.io/badge/License-TAPR%20OHL-blue.svg)](https://web.tapr.org/TAPR_Open_Hardware_License_v1.0.txt)
![Sponsored by PCBWay](https://img.shields.io/badge/PCB%20Sponsor-PCBWay-green)

> [!WARNING]
> This device has been mostly tested, however, problems may arise. Expect changes and please report any inaccuracies or issues.


## Overview
<img width="1920" height="1080" alt="land_export" src="https://github.com/user-attachments/assets/4e11e4e2-a3a2-4446-b661-37c5dac60c5e" />
   
AZSensorSuite (AZSS) is an ultra-low-power, data-acquisition hardware platform designed to be user-programmed for a variety of applications. 

The example firmware counts objects passing the ToF sensor and logs temperature and humidity. Over BLE, you can read the latest values and download the logs.

**Current hardware:** PCB v1.20 has been tested with the current firmware on `main`. Firmware v1.1 is upcoming. See the [release notes](#release-notes) for the planned changes.

**Interactive Viewer:** View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware).

#

**Special thanks to [**PCBWay**](https://www.pcbway.com/) for sponsoring the development of this project.** 

If you're looking for high-quality advanced PCB manufacturing and assembly and great customer support, check them out.

#

### Hardware Features

* **SoC / Connectivity:** Nordic NRF54L15 (via U-Blox Nora B206 module). Supports BLE 6.0, Matter, Thread, Zigbee, and proprietary 2.4 GHz communication.
* **ToF Sensor:** ST Microelectronics VL53L4CD. Used for ULP presence detection ([STSW-IMG034](https://www.st.com/en/embedded-software/stsw-img034.html), as low as 154 µW) or standard ranging ([STSW-IMG026](https://www.st.com/en/embedded-software/stsw-img026.html)).
* **Environmental Sensor:** Sensirion SHT30 for ambient temperature and humidity readings.
* **Accelerometer:** PCB v1.20 includes the STMicroelectronics LIS2DUX12. Accelerometer support is pending in the example firmware, including the upcoming v1.1 release.
* **Power:** Powered by a CR2032 coin cell, converted to 2.8 V. The entire system is ultra-low power (e.g., ~400 µA in the human detection example).

> [!TIP]
> The VL53L4CD and SHT30 are both members of pin-compatible sensor families. They can easily be replaced to meet specific project requirements or to navigate supply chain shortages.

## Example Quickstart Guide

### Requirements
* NRF54L15DK (highly recommended) or another SWD debugger.
* SensorSuite PCB and TC2030-IDC-NL cable.
    * *Alternatively:* NRF54L15DK with sensor breakouts.
* [nRF Connect SDK v3.1.1](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/index.html) via nRF Connect for VS Code.
* For BLE downloads, see the [Python client setup](software/readme.md#setup).

### Build and Flash Process
* If working with the development kit, skip step 5 and in `firmware/prj.conf` set `CONFIG_APP_BOARD_IS_PROD=n`.
1.  Clone the repository to a path that contains **no spaces** (e.g., `/projects/sensor_suite` is good; `/projects/sensor suite/` will cause build errors).
2.  Open the application in nRF Connect and navigate to the `firmware` subfolder.
3.  Create a new build configuration:
    * **Target:** `nrf54l15dk/nrf54l15/cpuapp/ns`
    * **Base configuration:** `prj.conf`
    * **Base devicetree overlay:** `boards/nrf54l15dk_nrf54l15_cpuapp_ns.overlay`
    * For a PCB with Y1 unpopulated, add `lf-rc.conf` under **Extra Kconfig fragments** to use the internal low-frequency RC oscillator.
    * Select your desired build optimization.
4.  Click **Generate and Build**.
5.  Connect the TC2030-IDC-NL to the DK. The DK will automatically detect it and change the target to the connected custom PCB.
6.  For a first flash or an upgrade from v1.0.0, use **Flash and Erase**. For an upgrade, export the logs before erasing as described below. For later builds with the same partition layout, you can use **Flash** to preserve stored data.
* If a module is being flashed for the first time, it may have memory protections enabled. Click `Yes` in the pop-up to allow recovery.

<img width="462" height="150" alt="error" src="https://github.com/user-attachments/assets/c01f1521-834b-4669-94a2-276189f38c14" />

### Firmware Upgrade Process

Before upgrading to v1.1, use the [Python client's `download` command](software/readme.md#download-output) with the existing firmware and save a copy of `stamps.csv` and `ambient.csv`. Use **Flash and Erase** for a clean install. Erasing also clears settings and BLE bonds. After flashing, reconnect, sync the device time, and reapply any custom schedule.

## PCB Ordering Guide

| I used [**PCBWay**](https://www.pcbway.com/) for PCB fabrication as they offer reliable quality, competitive pricing, and quick turnaround, even with the nonstandard specs required by this project. |
|---|
### 1. Clone and Open the Project
1. Clone the repository to your local machine.
2. Open [`hardware/azss.kicad_pro`](hardware/azss.kicad_pro) in **KiCad 10** or newer for PCB v1.20.
3. *(Optional)* Make any custom modifications needed for your project.

### 2. Run Design Rules Check (DRC)
Run DRC in KiCad before exporting fabrication files. KiCad 10 reports a missing `+BATT` connection between a front copper trace and pad 1 of battery holder BT1. Review that connection before ordering.

### 3. Export Gerber & Drill Files
1. Open the PCB Editor.
2. Go to **File → Fabrication Outputs → Gerbers (.gbr)**.
3. Choose or create a dedicated output directory (e.g., `fabrication/`).
4. Click **Plot**.
5. Click **Generate Drill Files**, keep default options, and click **Generate**.
6. Compress the contents of your output directory into a single **`.zip`** file.

### 4. Upload to PCBWay
1. Go to the [PCBWay Quick Order Page](https://www.pcbway.com/QuickOrderOnline.aspx).
2. Upload your `.zip` file.
3. Inspect your layout using the online **Gerber Viewer** to verify the gerber exported correctly.

### 5. Configure PCB Parameters
Select **4 Layers**. Use the default stackup and follow the [fabrication specifications and battery-contact via guidance](hardware/readme.md#pcb-fabrication).

### 6. Add Assembly / Stencil & Checkout
1. **Stencil / Assembly:** Add an **SMD Stencil** if hand-assembling, or select PCBWay's **Assembly Service** if you want them professionally assembled.
2. Click **Save to Cart**.
3. Wait for PCBWay's order review to complete, typically within an hour.
4. Proceed to checkout, pay, and wait for your boards!
   
## Release Notes

### v1.1 (upcoming)

- Greatly improve reliability across counting and download
- Add LIS2DUX12 accelerometer in hardware
- Add more Kconfig symbols for easier customisation
- Expand log storage size
- Allow ranging during connection
- More changes upcoming ...

### v1.0.0 (17 April 2026)

- Initial release with the core counting, environmental logging, and BLE features.


## License

Copyright © 2026 Adam Zembrzuski. 

This project is licensed under the TAPR Open Hardware License ([www.tapr.org/OHL](https://www.tapr.org/OHL)).

A full copy of the license is available in the `license.txt` file in this repository.
