## Hardware

PCB **v1.20** is the current revision and has been tested with the firmware on `main`. Open [`azss.kicad_pro`](azss.kicad_pro) in KiCad 10 or newer.

This revision adds the LIS2DUX12 accelerometer (U5). Firmware support is pending, including in the upcoming v1.1 release.

**Interactive Viewer:** View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware).

### PCB Fabrication

Special thanks to [**PCBWay**](https://www.pcbway.com/) for sponsoring the development of this project.

| Parameter | Value |
|---|---|
| Layers | 4 |
| Board thickness | 1.6 mm nominal |
| Surface finish | 2 µin ENIG, preferably with selective hard gold on the battery contact |
| Minimum hole size | 0.3 mm |
| Minimum track width / spacing | 4 / 4 mil |
| Via process | Copper- or resin-filled vias preferred at the battery contact |

**Vias on the battery contact must be covered by some method.**


### Stencil

Electropolished stencils are preferred over 
laser-cut for cleaner paste release on the LGA pads of the Nora B206.

### Assembly Notes

- Reflow soldering required for the u-blox NORA-B206, VL53L4CD, and LIS2DUX12.

### Schematic Notes

- For current measurements across **R1**, replace the 0 Ω link with a suitable shunt resistor.
- Several components are marked DNP. These should only be populated if you have a specific reason to do so.

> [!TIP]
> To build without the SHT3x, leave C11 and U4 unpopulated and set `CONFIG_APP_SHT_ENABLE=n` in [`firmware/prj.conf`](../firmware/prj.conf).

### Test Points

Use the [v1.20 PCB layout](azss.kicad_pcb) to locate the probe points and the [schematic](azss.kicad_sch) to trace their nets. The front silkscreen marks `SCL`, `SDA`, and `IO` for `/I2C_SCL`, `/I2C_SDA`, and `/IO_0`. The layout also includes front-side probe points for `+BATT`, `+2V8`, and `/SHUNT LO`, and a back-side point for `/ERROR_OUT`.
