# References

Every outside source used for a printer protocol, with its license and how we used it.

**Rule:**
- MIT, BSD, Apache-2.0 and Unlicense code may be adapted, with attribution in `THIRD_PARTY.md`.
- GPL, AGPL, unlicensed code and decompiled vendor apps are read for facts only (opcodes, byte
  layouts, UUIDs). No code is taken, and nothing decompiled is committed.

| Source | License | Printer | Used for | How |
|---|---|---|---|---|
| [julienvincent/nelko-p21-rs](https://github.com/julienvincent/nelko-p21-rs) | MIT | P21 | job layout, status/config/battery parsing, CRC test vectors | adapted (THIRD_PARTY.md) |
| [TylerCode/Fyne-P21-Print](https://github.com/TylerCode/Fyne-P21-Print) | MIT | P21 | label size table (rows per size), TSPL builder shape | facts + sizes |
| [lab759/carny-labels](https://github.com/lab759/carny-labels) | MIT | P21, PM220 | cross-check of TSPL fields | facts |
| [yowhatscookingkai/NelkoP21LabelMaker](https://github.com/yowhatscookingkai/NelkoP21LabelMaker) | Unlicense | P21 | BLE characteristics `ff02` / `ff01` | facts |
| [merlinschumacher/nelko-p21-print](https://github.com/merlinschumacher/nelko-p21-print) | AGPL-3.0 | P21 | transport, full command list, status byte values, label block layout, head width, full-length print | facts only |
| [polskafan/phomemo_d30](https://github.com/polskafan/phomemo_d30) | MIT | D30 | preamble, `GS v 0` job, 96-dot rotated raster | adapted (THIRD_PARTY.md) |
| [theacodes/phomemo_m02s](https://github.com/theacodes/phomemo_m02s) | MIT | M02S (via D30) | raster packing | facts |
| [crabdancing/phomemo-d30](https://github.com/crabdancing/phomemo-d30) | AGPL-3.0 | D30 | cross-check of the preamble bytes | facts only |
| [vivier/phomemo-tools](https://github.com/vivier/phomemo-tools) | GPL-3.0 | D30 | `rastertopd30.py`: job framing, inversion, rotation | facts only |
| Nelko Android app (`~/Downloads/Nelko_apk.zip`) | proprietary | P21, PM220, PL70e-BT | device name table, the P21 being found by BLE name, Jieli service set, MTU | facts only |
| Phomemo Android app (`~/Downloads/Phomemo_apk.zip`) | proprietary | D30 | D30 name prefixes, SPP transport for that class, BLE credit scheme of other models | facts only |
| pm220_rp2_webapp, nelko-pm220 (own) | MIT | PM220 | TSPL job, status queries | ported |
