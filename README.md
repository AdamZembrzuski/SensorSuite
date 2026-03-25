# **AZ SensorSuite**<br>
Copyright © 2026 Adam Zembrzuski

_Licensed under the TAPR Open Hardware License (www.tapr.org/OHL)_<br>
_A full copy of the TAPR Open Hardware License is available in license.txt_

> [!WARNING]  
> The device has been mostly tested however it is always possible problems arise. Expect changes and report inaccuracies.

<p>
<img src="https://github.com/user-attachments/assets/5fea7b3b-b6b0-46b0-865e-5f31d0864868" width="49%"/>
<img src="https://github.com/user-attachments/assets/20d9fef6-5b72-4377-a723-6875384e79e8" width="49%"/>
</p>


## MANIFEST
AZSensorSuite is a data-acquistion hardware platform that can be user programmed for a variety of applications. 

It integrates a VL53L4CD ToF sensor from ST Microelectronics that can be used for ULP presence detection (as low as 55uA at 2V8) or for normal ranging. This is implemented alongside the Sensirion SHT30, an ambient condtion sensor that takes temperature and humidity readings.<br><br>

> [!TIP]
> The VL53L4CD and SHT30 are both members of pin-compatible sensor families and hence can be replaced in the event of specific project requirements or shortages.
<br>

The sensors are accompanied by the Nordic NRF54L15 implemented via the U-Blox Nora B206 module enabling BLE 6.0, Matter, Thread, Zigbee or other (proprietary) 2.4GHz communication. The entire system is ultra low power (400uA @ 2V8 in human detection example) and is powered from a common CR2032 coin cell.<br><br>

The included example firmware detects when people pass through a doorway into a closed space. Later a machine learning model (ran on a computer) predicts when the space is empty so that services (e.g. heating or lighting) can be disabled.

## EXAMPLE QUICKSTART GUIDE
### Requirements
- NRF54L15DK (highly reccomended) or other SWD debugger
- SensorSuite V1.02 PCB and TC2030-IDC-NL<br>
  **OR**<br>
  NRF54L15DK with sensor breakouts 
- NRF SDK v3.1.1 via NRF Connect for VSCode
- Python 3 (At least 3.07)

### Build Process
- Clone the repository to a path that contains no spaces at any point (eg *"/user/Sensor Suite/repo"* disallowed)
- Select open application in NRF Connect and navigate to the *"firmware"* subfolder.
- Create a build configuration <br>
  - Set target to "nrf54l15dk/nrf54l15/ns"
  - Set base board overlay as "boards/nrf54l15dk_nrf54l15_cpuapp_ns.overlay"
  - Set KConfig file to "prj.conf"
  - Select build optimisation as required
- Press "Generate and build"

### Flashing Process
If flashing the DK skip step 1

- Connect TC2030-IDC-NL to the DK, the DK will automatically detect it and change target to your connected PCB.
- Press flash and erase. If a module is being flashed for the first time, it may have protections enabled. Follow the advice in the vscode pop-up.
##
View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware)



