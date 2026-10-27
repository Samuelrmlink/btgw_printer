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
- **Drivers** are PEX apps, one per protocol family and transport. A driver
  covers several models through a model table; a printer that is much
  different gets its own driver. The contract is in
  `drivers/include/printer_driver.h`.

| Driver | Protocol, transport | Models | Source |
|---|---|---|---|
| `nelko_tspl` | Nelko TSPL, Classic SPP | `pm220`, `p21` | `drivers/vendors/nelko/tspl/` |
| `nelko_pl70e` | Nelko TSPL, BLE | `pl70e` | `drivers/vendors/nelko/pl70e/` |
| `phomemo_d30` | ESC/POS raster, Classic SPP | `d30` | `drivers/vendors/phomemo/d30/` |
| `printer_virtual` | none (tests) | `virtual` | `drivers/virtual/` |

## Modes

| | Companion | Standalone |
|---|---|---|
| Firmware base | `btgw/configs/pico2w_sd_classic_defconfig` + `configs/companion.fragment` | `btgw/configs/pico2w_classic_defconfig` + `configs/standalone.fragment` |
| Parent web UI | kept; its "Printer" toolbar button comes from our `apps/printer/app.json` (parent launchers) | not uploaded |
| Our app | `/printer/` (LittleFS `printer/`), or `/sd/apps/printer/` (card `apps/printer/`, `deploy.py --target sd`; needs parent R2) | `/printer/`; `/` redirects there |
| Switch button in our app | "BTGW" back to `/` | hidden |

`mode.json` (`{"mode":"companion"|"standalone"}`) is written by
`tools/deploy.py` next to the app (`printer/mode.json` on flash, `apps/printer/mode.json`
on the card) and tells the app which buttons to show. The app fetches it
from beside itself, then falls back to flash `printer/mode.json`.

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
| `printer.status` | `{"driver"?,"addr"?}` | `{"printers":[{"driver","addr","model","state","why"?,"battery"?,"charging"?,"paper"?,"lid"?,"busy"?,"printer"?,"media"?:{"type","w_mm","h_mm"},"dpi"?,"fw"?}],"job":{"job","state","driver","copy","copies","sent","total"}|null}` |
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
driver as it is (`nelko_tspl`: `{"preflight":false}` skips the ESC !? check
before the job; virtual: `rate`, `fail_at`, `no_file`). `model` picks the
model in a driver with several; `nelko_tspl` follows it even when the connect
guessed another.

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
| `printer.found` | `{"driver","addr","name","rssi","model"}`; `model` is null when the driver can't tell (a device with no name: the user picks) |
| `printer.state` | `{"driver","addr","model","state":"idle|scanning|connecting|pairing|connected|waiting|printing","why"?,"pin"?,"channel"?,"retry_ms"?}` |
| `printer.status` | one printer's status object (as in `printer.status`), sent when a reply from the printer changes it |
| `printer.job` | `{"job","state":"queued|sending|printing|done|failed|cancelled","copy","copies","sent","total","message"?}` |
| `printer.log` | `{"driver","msg"}` |

Drivers put `"driver"` in every object they emit or list; the hub passes
them on as `printer.<kind>`. `state` `waiting` means a retry inside the current
attempt is due in `retry_ms` (the next PIN, or a busy radio); nelko_tspl no
longer waits to reconnect after a lost link. A Classic driver
reports the `pin` and `channel` that worked: save them in the printer's
config (`printers.json`) and pass that config to `printer.connect`, and the
next connect tries that PIN first and skips SDP. Put the printer's `model`
in that config too: a driver with several models takes it from there (else
from the inquiry name, else its first model, with a `printer.log` line). When a driver registers,
the hub connects every printer in `printer/printers.json` with that driver
and `auto_reconnect` not false, passing its whole entry as the config,
except printers whose model's `transport` is `classic`: those wait for the
user (logged as "waits for the user"). A Classic driver also never
reconnects by itself: a connect is one attempt (the PIN walk and a busy
radio aside), and a lost link or an unreachable printer ends it.

**The app starts the printer service it needs** (roadmap F11). Nothing
loads our PEX modules after a board boot (parent B3 keeps `cfg/launch.json`
unwritten, and the parent's launchers don't read our `app.json` `pex` list),
though they stay installed. So when `printer.drivers` answers `unknown_op`
after the WebSocket is up, the app sends `pex.load` then `pex.start` for
`printer_hub`, waits (≤ 5 s, asking once a second) for the hub to answer and
carries on. A driver is loaded the same way only when it is needed: before
`printer.connect` to a printer whose driver `printer.drivers` doesn't list
(`web/hub.js` `ensureDriver`, inside `requestConnect`), and before a scan for
a printer family (the Printers dialog's "Scan for": our drivers are the
`pex` list in the `app.json` next to the page, else `nelko_tspl`). When the
board answers that the PEX pool is full and another of our drivers is loaded
with no printer in use on it, that one gets `pex.stop` + `pex.unload` and the
load is tried once more. "Already loaded / running" counts as done. Ops go
one at a time; a failure shows the board's message with a Retry and is not
retried on its own.

**The web app never pages a saved Bluetooth Classic printer by itself**
(roadmap F9: paging one that is off stalls the gateway's Wi-Fi, parent B4).
Every connect goes through `web/hub.js` `requestConnect`. For a printer in
`printers.json` whose model's `transport` is `classic` it first asks "Turn on
the printer" and sends `printer.connect` only on Connect. At page load the
current printer (with `auto_reconnect` not false) is connected directly when
it is BLE, and asked about once, after boot, when it is Classic; Cancel
leaves a Connect action on the status line. A print with the printer down
asks the same and continues once it is connected. No prompt for a printer
being added from a scan, or for one a `printer.found` in this page reported,
until a `printer.state` `why` says it is lost again ("link lost", "not
reachable", "stopped retrying").

Driver model caps (`printer.drivers`) carry `presets`, a list of media
profile ids from `web/presets/media/` (`nelko-pm220-50x30`, `nelko-4x6`,
`nelko-p21-14x40`, `phomemo-d30-12x40`, ...), and `placement`: true when the
printer places the bitmap itself (TSPL `BITMAP x,y`; the web app sends the
cropped bitmap and its origin), false when the bitmap must be `head_dots`
wide with the origin baked in. `web/presets/models.json` is the web app's
copy of every driver's caps for designing without a board;
`drivers/test/test_caps.c` fails when a field differs.

Classic drivers open their session at security level 0 unless the printer's
config sets `"security_level"` (0..4). The parent firmware at b397a84 checks
the level before BTstack has read the encryption key size, so a session that
asks for level 2 or more fails right after pairing
(`docs/parent-requirements.md` B1). A printer that wants a legacy PIN still
asks for it at level 0, and the driver answers from its PIN list.

## Settings JSON

`printer/printers.json`:
```json
{"version":1,"printers":[{"addr":"..","driver":"nelko_pl70e","model":"pl70e","nickname":"Shipping",
  "media_id":"nelko-4x6","density":8,"speed":-1,"copies":1,"auto_reconnect":true,
  "calibration":{"nelko-4x6":{"offset_x_mm":0,"offset_y_mm":0,"gap_mm":3,"origin_x_adj":0,"origin_y_adj":0,
                        "print_height_adj":0,"safe":[16,16,815,1201]}}}]}
```
`printer/media.json` and `web/presets/media/*.json`:
```json
{"version":1,"media":[{"id":"nelko-4x6","name":"4 × 6 in shipping","w_mm":101.6,"h_mm":152.4,"gap_mm":3,
  "type":"gap","safe_mm":[2,2,2,2],"models":["pl70e"]}]}
```
