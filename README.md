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

> [!NOTE]
> The included firmware detects when people pass through a doorway into a closed space. A machine learning then models when the space is empty so that services (e.g. heating or lighting) can be disabled.

It integrates a VL53L4CD ToF sensor from ST Microelectronics that can be used for ULP presence detection (as low as 55uA at 2V8) or for normal ranging. This is implemented alongside the Sensirion SHT30, an ambient condtion sensor that takes temperature and humidity readings. The SHT30 is pin compatible with the SHT31 and SHT35 if more precision is desired. The sensors are accompanied by the Nordic NRF54L15 on a U-Blox Nora B206 module which enables BLE 6.0, Matter, Thread, Zigbee and other proprietary 2.4GHz communication. The entire system is ultra low power (400uA @ 2V8 in human detection example) and can be easily powered from a CR2032 battery.
##
View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware)



