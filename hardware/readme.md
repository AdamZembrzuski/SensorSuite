## Hardware

This folder contains all hardware design files and documentation.

**Interactive Viewer:** View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware).
##
### PCB Fabrication

The PCB is four layers, based on the [OshPark](https://docs.oshpark.com/services/four-layer/) 
four-layer stackup. A different stackup may be used if preferable, as only two GPIO traces 
require controlled impedance.

| Parameter | Value |
|---|---|
| Layers | 4 |
| Board thickness | 1.6mm |
| Surface finish | 2µ" ENIG (preferred) |
| Via drill / pad | 0.3 / 0.4mm |
| Via tenting | Tented minimum, plugged preferred |

### Stencil

Electropolished stencils are preferred over 
laser-cut for cleaner paste release on the LGA pads of the Nora B206.

### Assembly Notes

- The U-Blox Nora B206 and ST VL53L4CD are LGA components and **must be reflow soldered**. 
  Hand soldering is not possible.

### Schematic Notes

- **R1** should be replaced to a suitable value for current measurement instead of 0 ohms.
- Several components are marked DNP. These are intended for development boards only 
  and should be omitted on production boards.

> [!TIP]
> If the SHT3x is not required, C11 and U4 can be DNP'd, and in the example firmware, `CONFIG_APP_SHT_ENABLE` should be set to `n`in `firmware/prj.conf`.

### Test Points

Test point layout is visible below and is consistent with the schematic.


<img width="1920" height="1080" alt="test points" src="https://github.com/user-attachments/assets/e3f01004-9dde-4c46-b176-f7fd68e1c987" />
