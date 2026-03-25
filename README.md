# AZ SensorSuite

[![License: TAPR-OHL](https://img.shields.io/badge/License-TAPR%20OHL-blue.svg)](https://web.tapr.org/TAPR_Open_Hardware_License_v1.0.txt)

> [!WARNING]
> This device has been mostly tested, however, problems may arise. Expect changes and please report any inaccuracies or issues.

<p align="center">
<img src="https://github.com/user-attachments/assets/5fea7b3b-b6b0-46b0-865e-5f31d0864868" width="49%"/>
<img src="https://github.com/user-attachments/assets/20d9fef6-5b72-4377-a723-6875384e79e8" width="49%"/>
</p>

## Overview

AZ SensorSuite is an ultra-low-power, data-acquisition hardware platform designed to be user-programmed for a variety of applications. 

The included example firmware detects when people pass through a doorway into a closed space. A machine learning model (run on a host computer) can then predict when the space is empty so that services like heating or lighting can be automatically disabled.

**Interactive Viewer:** View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware).

### Hardware Features

* **SoC / Connectivity:** Nordic NRF54L15 (via U-Blox Nora B206 module). Supports BLE 6.0, Matter, Thread, Zigbee, and proprietary 2.4GHz communication.
* **ToF Sensor:** ST Microelectronics VL53L4CD. Used for ULP presence detection ([STSW-IMG034](https://www.st.com/en/embedded-software/stsw-img034.html), as low as 55µA at 2.8V) or standard ranging ([STSW-IMG026](https://www.st.com/en/embedded-software/stsw-img026.html)).
* **Environmental Sensor:** Sensirion SHT30 for ambient temperature and humidity readings.
* **Power:** Powered by a CR2032 coin cell, converted to 2.8V. The entire system is ultra-low power (e.g., ~400µA in the human detection example).

> [!TIP]
> The VL53L4CD and SHT30 are both members of pin-compatible sensor families. They can easily be replaced to meet specific project requirements or to navigate supply chain shortages.

## Example Quickstart Guide

### Requirements
* NRF54L15DK (highly recommended) or another SWD debugger.
* SensorSuite V1.02 PCB and TC2030-IDC-NL cable.
    * *Alternatively:* NRF54L15DK with sensor breakouts.
* [nRF Connect SDK v3.1.1](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/index.html) via nRF Connect for VS Code.
* Python 3.7 or higher.

### Build and Flash Process
1.  Clone the repository to a path that contains **no spaces** (e.g., `/projects/sensor_suite` is good; `/projects/sensor suite/` will cause build errors).
2.  Open the application in nRF Connect and navigate to the `firmware` subfolder.
3.  Create a new build configuration:
    * **Target:** `nrf54l15dk/nrf54l15/cpuapp/ns`
    * **Base configuration:** `prj.conf`
    * **Base devicetree overlay:** `boards/nrf54l15dk_nrf54l15_cpuapp_ns.overlay`
    * Select your desired build optimization.
4.  Click **Generate and Build**.
* If flashing the Development Kit directly, skip step 5.
5.  Connect the TC2030-IDC-NL to the DK. The DK will automatically detect it and change the target to the connected custom PCB.
6.  Press **Flash and Erase**. (You may flash without erase after the first flash)
* If a module is being flashed for the first time, it may have NVM protections enabled. In this event, follow the advice in the VS Code pop-up to unlock it.

## License

Copyright © 2026 Adam Zembrzuski. 

This project is licensed under the TAPR Open Hardware License ([www.tapr.org/OHL](https://www.tapr.org/OHL)).

A full copy of the license is available in the `license.txt` file in this repository.
