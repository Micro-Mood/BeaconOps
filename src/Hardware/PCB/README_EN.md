# PCB

This directory holds the BeaconOps PCB source project — just one file: `ProPrj_BeaconOps.epro`, made with JLCEDA Pro.

## About This Board

I drew this board last year. The main chip ESP32-C3 is QFN-32; the external flash Winbond W25Q128JV is WSON-8 — those two are manageable. The tricky parts are the two leadless components:

- The ST LSM6DS3TR-C IMU is LGA-14: all 14 pads are hidden under the chip body. You can't see the solder joints after reflow — positional accuracy from a pick-and-place or hot-air station is what keeps them reliable. A 0.1 mm shift can produce a cold joint.
- The MAX98357AEWL+ audio amp is WLP-9 (1.34 × 1.34 mm): a bare die with 9 solder balls — effectively a BGA-class package. Rework requires X-Ray; in a hand-soldering context a bad joint usually means replacing the part.

Passive components are all 0402, with the tightest pad spacing around 0.4 mm. Parts sit fairly close together overall. **The hard part of cloning this board is not the files — it is the soldering.** Whether you can reflow it cleanly and place the parts straight really depends on your equipment and experience.

I did not try to soften the design into something more beginner-friendly, because BeaconOps was never meant to be a "just hand-solder it" kind of board.

## How to Open It

- Open the `.epro` project with JLCEDA Pro
- Inside you can browse the schematic, PCB, footprints, and project settings
- If you want Gerbers, BOM, or pick-and-place files, export them yourself from the project — they are intentionally not duplicated in the repo

## Notes

- Only the source project is kept here; no production exports, to avoid version drift
- For future revisions, keep treating this project as the single editing source
- The board outline and connector positions are tightly coupled to the shell, so it is worth glancing at `../Enclosure/` while reviewing this board