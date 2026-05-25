# Enclosure

This is the enclosure that BeaconOps currently uses.

## Files

- `shell.stl`: main shell, ready to print
- `back-cover.stl`: back cover, ready to print
- `model.3dm`: editable mechanical source
- `model_embedded_files/`: embedded resources used by `model.3dm`

## Why It Is the Same as pocket

To be honest: **the PCB was already drawn last year, and I did not feel like redesigning the shell for BeaconOps. Since the whole [pocket](../../../../pocket/hardware) project shares the same dimensions, I just reused it.**  
So what is kept here is literally the same STL / 3DM set — not some clever shared-design pattern, just a pragmatic choice.

## How to Use

- For printing, use the STL files
- To change dimensions, cutouts, or assembly details, edit `model.3dm` first and then re-export STL
- Before editing, glance at `../PCB/` to confirm the board outline and connector positions still match

## Looking Forward

If BeaconOps ever diverges mechanically from pocket, this directory will start maintaining a BeaconOps-specific shell on its own, instead of staying lock-step with pocket.