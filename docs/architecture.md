# Architecture

btgw_printer is a web app and a set of PEX modules that run on an unmodified
bluetooth_rp2_webapp (BTGW) image. The parent is the submodule `btgw/`.

```
browser (web/)  --WS /ws + POST /api/op-->  printer_hub (PEX)  --pex_call-->  driver (PEX)  --BLE / Classic-->  printer
      |                                         ^  streams job.bin via pex_fs_*
      +-- PUT /api/fs or /api/sd/files ---------+  (raster, labels, settings)
```

- **The web app renders.** It lays out the label, applies calibration, and
  packs a 1-bit bitmap the size of the printed area in the model's head
  orientation. Drivers never rasterise. They encode the rows (TSPL, ESC/POS
  raster, vendor framing) and move them over the transport.
- **printer_hub** owns every `printer.*` op and event, so the parent's 16-op
  table isn't spent once per driver. It keeps the driver registry and streams
  job files to drivers.
- **Drivers** are PEX apps, one per vendor family. The contract is in
  `drivers/include/printer_driver.h`.

## Modes

| | Companion | Standalone |
|---|---|---|
| Firmware base | `btgw/configs/pico2w_sd_classic_defconfig` + `configs/companion.fragment` | `btgw/configs/pico2w_classic_defconfig` + `configs/standalone.fragment` |
| Parent web UI | kept, plus `js/ext/printer-switch/` (a "Printer" button) | not uploaded |
| Our app | `/printer/` (LittleFS `printer/`; SD once parent R2 lands) | `/printer/`; `/` redirects there |
| Switch button in our app | "BTGW" back to `/` | hidden |

`printer/mode.json` (`{"mode":"companion"|"standalone"}`) is written by
`tools/deploy.py` and tells the app which buttons to show.

## Files on the board

| Path | What |
|---|---|
| `printer/` | the app (gzipped assets, not minified) |
| `printer/printers.json` | saved printer configs |
| `printer/media.json` | user media profiles (presets ship in the app) |
| `printer/labels/` | saved labels and templates on flash |
| `sd/printer/labels/` | the same on the SD card (companion, card mounted) |
| `printer/var/job.bin` | the last print job's bitmap (`sd/printer/var/job.bin` when on SD) |
| `pex/printer_hub.m33.pex`, `pex/<driver>.m33.pex` | modules |

## Ops (all owned by printer_hub)

Requests are `{"op":..., "params":{...}}` over `/ws` or `POST /api/op`. Params
must stay under ~960 bytes and replies under ~2.8 KB (parent limits).

| Op | Params | Reply |
|---|---|---|
| `printer.drivers` | – | `{"drivers":[<caps JSON per driver>]}` |
| `printer.scan` | `{"ms":8000, "driver"?}` | `{"started":[driver,...]}`; results arrive as `printer.found` |
| `printer.devices` | `{"driver"?}` | `{"devices":[{"driver","addr","name","rssi","model","transport"}]}` |
| `printer.connect` | `{"driver","addr","config"?:{...}}` | `{"pending":true}` |
| `printer.disconnect` | `{"driver"?,"addr"?}` | `{"ok":true}` |
| `printer.status` | `{"driver"?,"addr"?}` | `{"printers":[{"driver","addr","model","state","why"?,"battery"?,"charging"?,"paper"?,"lid"?,"busy"?,"media"?:{"type","w_mm","h_mm"},"dpi"?,"fw"?}],"job":{"job","state","driver","copy","copies","sent","total"}|null}` |
| `printer.print` | see below | `{"job":id}` or error `busy` / `not_connected` / `bad_request` |
| `printer.cancel` | `{"job"?}` | `{"ok":true}` |

`printer.print` params:
```json
{"driver":"nelko_pl70e","addr":"AA:BB:CC:DD:EE:FF","model":"pl70e",
 "path":"printer/var/job.bin","width_dots":832,"rows":1218,
 "origin":[0,0],"media":{"w_mm":101.6,"h_mm":152.4,"gap_mm":3,"type":"gap"},
 "density":8,"speed":-1,"copies":1,"rotate":0,"options":{}}
```
`path` uses the parent's virtual names: `printer/...` is LittleFS, and
`sd/printer/...` is the card (`sd:/` for `pex_fs_*`; `lfs:/...` and `sd:/...`
are accepted as they are). The file must be exactly `rows * ceil(width_dots/8)`
bytes: 1 bit per dot, MSB first, row 0 first, and the burning bit value is
the model's `black_bit` (0 for the Nelko models: 0 = black, 1 = white).
`origin` is where the bitmap goes on the label, in dots, after calibration.
`density`/`speed` are clamped to the model's ranges (-1 or absent = the
model's default; a model with `"speed":null` gets no speed command).
`media.h_mm` 0 or absent means continuous media. `options` is passed to the
driver as it is (PM220: `{"preflight":false}` skips the ESC !? check before
the job; virtual: `rate`, `fail_at`, `no_file`).

The hub reads the file once to see whether it is all black (and if so clears
the first dot of every 16th row, the old firmware's head protection), then
streams it in chunks of whole rows. It feeds the rows once when the driver
prints copies itself (TSPL `PRINT n`), else once per copy.

**Calibration and layout live in the web app.** The arithmetic the old PM220
firmware did (`job.c`) is `drivers/test/ref/layout.c`, checked against the old
code byte for byte; `drivers/test/golden/layout_vectors.json` holds 120
input/output cases for the JavaScript port to match.

## Events

| Event | Data |
|---|---|
| `printer.found` | `{"driver","addr","name","rssi","model"}` |
| `printer.state` | `{"driver","addr","model","state":"idle|scanning|connecting|pairing|connected|waiting|printing","why"?,"pin"?,"channel"?,"retry_ms"?}` |
| `printer.status` | one printer's status object (as in `printer.status`), sent when a reply from the printer changes it |
| `printer.job` | `{"job","state":"queued|sending|printing|done|failed|cancelled","copy","copies","sent","total","message"?}` |
| `printer.log` | `{"driver","msg"}` |

Drivers put `"driver"` in every object they emit or list; the hub passes
them on as `printer.<kind>`. `state` `waiting` means the link dropped and a
reconnect is due in `retry_ms` (8 s, doubling to 60 s). A Classic driver
reports the `pin` and `channel` that worked: save them in the printer's
config (`printers.json`) and pass that config to `printer.connect`, and the
next connect tries that PIN first and skips SDP. When a driver registers,
the hub connects every printer in `printer/printers.json` with that driver
and `auto_reconnect` not false, passing its whole entry as the config.

Driver model caps (`printer.drivers`) carry `presets`, a list of media
profile ids from `web/presets/media/`, written `<w>x<h>mm` (`50x30mm`) or
`<w>x<h>in` (`4x6in`).

## Settings JSON

`printer/printers.json`:
```json
{"version":1,"printers":[{"addr":"..","driver":"nelko_pl70e","model":"pl70e","nickname":"Shipping",
  "media_id":"4x6","density":8,"speed":-1,"copies":1,"auto_reconnect":true,
  "calibration":{"4x6":{"offset_x_mm":0,"offset_y_mm":0,"gap_mm":3,"origin_x_adj":0,"origin_y_adj":0,
                        "print_height_adj":0,"safe":[16,16,815,1201]}}}]}
```
`printer/media.json` and `web/presets/media/*.json`:
```json
{"version":1,"media":[{"id":"4x6","name":"4×6 in shipping","w_mm":101.6,"h_mm":152.4,"gap_mm":3,
  "type":"gap","safe_mm":[2,2,2,2],"models":["pl70e"]}]}
```
