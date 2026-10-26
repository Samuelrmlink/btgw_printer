# Phomemo D30 protocol

Status: researched from public sources only. **Not yet tried against a printer.** Source tags:
- **[P]** polskafan/phomemo_d30 (MIT), the reference we adapt.
- **[A]** theacodes/phomemo_m02s (MIT), which [P] took its image helpers from.
- **[C]** crabdancing/phomemo-d30 (AGPL), facts only.
- **[V]** vivier/phomemo-tools `rastertopd30.py` (GPL-3), facts only.
- **[X]** the Phomemo Android app, decompiled facts only and never committed.

**Summary:** the D30 is an ESC/POS-style raster printer over Classic SPP. A job is a short magic
preamble, `ESC @`, and one `GS v 0` raster block that is 96 dots wide. There is no print command
and no reply is needed.

## Transport

| Fact | Confidence | Source |
|---|---|---|
| Bluetooth Classic SPP, RFCOMM **channel 1** (`rfcomm connect 1 <mac>`) | high | [P] [C] |
| The current Phomemo app maps the `D30` / `D30S` / `D30Pro` name prefixes to its generic label-printer class, whose transport is an SPP socket on UUID `00001101-…` (no BLE connector for this class) | high | [X] |
| Name prefix `D30`. The app matches models by `name.startsWith(model)` over its model list (which also has `Q30`, `Q30S`, `Q31`, `Q32`) | high | [X] |
| Pairing PIN: none known. The app opens an insecure RFCOMM socket | medium | [X], [P] pairs with `rfcomm` |
| BLE: the app's generic BLE connector (write char `0000FF02…`, notify on the others, credit packets `01 NN` / `02 LL` / `1a 36 NN`) is used by other Phomemo models, **not** the D30 class | medium | [X] |

## Job format

```
1f 11 24 00                         magic, "seems needed" [V]; [P] [C] send it as well
1b 40                               ESC @  initialise
1d 76 30 00  wl wh  hl hh           GS v 0, mode 0 (normal), bytes per row LE, rows LE
<rows × wbytes raster bytes>
```

- [P] [C] send a preamble before the job (sniffed from the "Print Master" app):
  `1f1138`, `1f11121f1113`, `1f1109`, `1f1111`, `1f1119`, `1f1107`, `1f110a1f110202`. These look
  like queries and settings (`1f 11 xx` is Phomemo's vendor prefix). Their replies aren't
  documented. **Our driver sends them before the first job on a link** to match the app, and logs
  whatever comes back. medium
- `[P]` writes the GS v 0 header once with the full height (320 rows), then the data in 255-row
  pieces with no extra headers. The pieces are only write chunks. high
- **wbytes = 12 (96 dots)**: the head is 96 dots at 203 dpi = 12 mm. high [P] [C]
- **Bits:** MSB first, **1 = black** (the opposite of the Nelko TSPL printers): `black_bit = 1`.
  high [P] (image inverted before `convert("1")`) [V] (inverted)
- **Orientation:** the 96 dots run across the tape, and rows run along the label. Text that reads
  along the label is rotated 90° in the bitmap. [P] renders 320×96 landscape and rotates it 270°;
  [V] rotates 90°. Either way the bitmap is 96 wide and the label length tall. high
- **Rows:** 320 for a 40 mm label [P] (`0x0140`). The printer feeds to the next gap on its own
  after the image. [V] adds feed lines from the CUPS AdvanceDistance; [P] adds none. medium
- **Density:** no density command is known for the D30. Not exposed: `density.min = max = 0`.
- **Copies:** send the job again (the bitmap is cached in the hub's job file).

## Labels

The app gets label sizes from Phomemo's server, so the APK has no D30 size table [X]. Known
D30 rolls are 12 mm wide in several lengths (12 × 40 is the common one); [P]'s "fruit" mode
prints 240 × 80 inside the same 320 × 96 frame with an x offset of −60 (a 30 mm label).

**The user's roll:** "1.13″ diameter × 1.295″ width × 5.17″ height". That's the roll package:
about a 28.7 mm roll diameter, 32.9 mm across including the core flanges, and 131 mm tall.
**The label face size has to be read off the roll.** Proposed presets (all 96 dots across):

| Preset id | Label | Rows |
|---|---|---|
| `phomemo-d30-12x40` (default) | 12 × 40 mm | 320 |
| `phomemo-d30-12x30` | 12 × 30 mm | 240 |
| `phomemo-d30-14x40` | 14 × 40 mm | 320 (head is 12 mm; 1 mm margin each side) |
| `phomemo-d30-15x30` | 15 × 30 mm | 240 |
| `phomemo-d30-12x22` | 12 × 22 mm | 176 |

## Status

No reply format is documented for the D30. The driver reports `connected` and `printing` from the
link state only, and keeps every received byte in `last_rx` (hex) for the bench session.

## Open questions for the bench

1. Do the preamble queries get replies (battery, paper, version)? Capture them.
2. Is `1f 11 24 00` a density/heat setting (the 4th byte varying)? Try 0..? on the bench.
3. Does the D30 need a trailing feed, or does it advance to the gap itself?
4. The user's label face size.
