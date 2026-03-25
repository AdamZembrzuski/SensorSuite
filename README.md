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

The included firmware detects when people pass through a doorway into a closed space. A machine learning then models when the space is empty so that services (e.g. heating or lighting) can be disabled.
##
View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware)



