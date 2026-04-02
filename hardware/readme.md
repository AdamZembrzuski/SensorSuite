## Hardware

This folder contains all hardware design files and documentation.

**Interactive Viewer:** View the PCB and schematic entirely in your browser on [KiCanvas](https://kicanvas.org/?repo=https%3A%2F%2Fgithub.com%2FAZT-GH%2FSensorSuite%2Ftree%2Fmain%2Fhardware).
##
### PCB Fabrication

The PCB is four layers, based on the [OshPark](https://docs.oshpark.com/services/four-layer/) 
four-layer stackup. A different stackup may be used if preferable, as only two GPIO traces 
require controlled impedance.

OshPark is recommended for fabrication as they offer the most affordable 2µ" ENIG finish for small orders.
If ordering elsewhere, the minimum required specs are:

| Parameter | Value |
|---|---|
| Layers | 4 |
| Board thickness | 1.6mm |
| Surface finish | 2µ" ENIG (preferred) |
| Via drill / pad | 0.3 / 0.4mm |
| Via tenting | Tented (minimum) |

### Stencil

Stencils are recommended from JLCPCB. Electropolished stencils are preferred over 
laser-cut for cleaner paste release on the LGA pads of the Nora B206.

### Assembly Notes

- The U-Blox Nora B206 is an LGA component and **must be reflow soldered**. 
  Hand soldering is not possible.

### Schematic Notes

- **R11** should be replaced to a suitable value for current measurement instead of 0 ohms.
- Several components are marked DNP. These are intended for development boards only 
  and should be omitted on production boards.

> [!TIP]
> If the SHT3x is not required, C11 and U4 can be DNP'd.
