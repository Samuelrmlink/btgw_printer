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
| `printer.status` | `{"driver"?,"addr"?}` | `{"printers":[{"driver","addr","model","state","battery"?,"paper"?,"lid"?,"media"?,"error"?}],"job":{...}|null}` |
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
`sd/printer/...` is the card (`sd:/` for `pex_fs_*`).

## Events

| Event | Data |
|---|---|
| `printer.found` | `{"driver","addr","name","rssi","model"}` |
| `printer.state` | `{"driver","addr","state":"idle|scanning|connecting|pairing|connected|printing|error","why"?}` |
| `printer.status` | driver status JSON (battery, paper, lid, media) |
| `printer.job` | `{"job","state":"queued|sending|printing|done|failed|cancelled","copy","copies","sent","total","message"?}` |
| `printer.log` | `{"driver","msg"}` |

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
