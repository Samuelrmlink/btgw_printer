# Adding a printer

Every printer driver is a PEX app module that registers with `printer_hub`.
The web app does all the drawing; the driver turns rows into the printer's
command language and moves them over BLE or Bluetooth Classic.

## 0. A model row first

A driver covers one protocol family over one transport, and can hold several
models in a model table. So first look for a driver of the same protocol
family and transport:

- **Same family, same transport:** add a model row to that driver. Example:
  the P22 is the P21's firmware, so it is one more row in `MODELS[]` in
  `drivers/vendors/nelko/ble/nelko_ble.c` (id, name prefix, head dots, density
  range, caps object), plus its presets and a `web/presets/models.json` entry.
  A Classic one goes in `nelko_tspl.c` the same way (with flags and PINs).
- **A new protocol or a new transport:** write a new driver (the rest of this
  page). The P21 speaks the PM220's commands, but over BLE, so it is in
  `nelko_ble`, not `nelko_tspl`. The PL70e-BT speaks TSPL over BLE too, but with
  other status replies (`READSTA`) and other GATT pipes, so it has its own driver.
- **One printer, one driver.** A dual-mode printer (the P21 pages over Classic
  and advertises over LE, at one address) gets a row in one driver only, or two
  drivers would claim it. Each module also has to install from the card in one
  piece of the PEX pool (parent R11): keep it small, and check the size that
  `tools/run_tests.py pex` prints.

| Driver | Family, transport | Models |
|---|---|---|
| `nelko_tspl` | Nelko TSPL, Classic SPP | `pm220` |
| `nelko_ble` | Nelko TSPL, BLE (GATT `ff00`, the Jieli firmware) | `p21` |
| `nelko_pl70e` | Nelko TSPL, BLE | `pl70e` |
| `phomemo_d30` | Phomemo ESC/POS raster, Classic SPP | `d30` |
| `printer_virtual` | none (a fake printer for tests) | `virtual` |

The hub caches at most four models per driver, and the whole
`printer.drivers` reply must stay under ~2.8 KB (`test_caps` checks it), so
keep the caps short.

## 1. Where it goes

```
drivers/vendors/<vendor>/<family>/
  <driver>.c         the module: prn_drv(), pex_main(), the model table
  <vendor>_proto.c/h pure protocol code (framing, status parsing): host-tested
  module.cmake       prn_add_module(<vendor>_<family> ...)   (same arguments as pex_add_module)
```

`drivers/CMakeLists.txt` picks up every `*/module.cmake` by itself. Give the
module a name of its own (`nelko_tspl`). That is the driver name in every op
and event. Copy `drivers/vendors/nelko/tspl/module.cmake`: list
`core/prn_core.c` (and `proto/tspl/tspl.c` for TSPL) in `SOURCES`, and add
`${PRN_INCLUDE}` plus the `drivers/` directory to `INCLUDES`.

## 2. The contract

Read `drivers/include/printer_driver.h`. The driver:

- exports one `int prn_drv(cmd, a0, a1, a2)` (`PEX_EXPORT(prn_drv)`);
- registers in `PEX_APP_RUN` with `prn_hub_attach(&hub, pex_name(pex_self()))`, and retries from its tick when the hub was not up yet (`prn_hub_ok()`);
- unregisters in `PEX_APP_STOP` (`prn_hub_detach`);
- answers:
  - `INFO`: caps JSON with one entry per model. The hub caches `id`, `black_bit`, and the `density` and `speed` ranges.
  - `SCAN`: start or stop finding printers; report each new one once with `prn_hub_emit(&hub, "found", ...)`.
  - `DEVICES`: a JSON array, by the same rule as `SCAN`.
  - `CONNECT`: asynchronous. Report progress with `"state"` emits. The model comes from the config's `"model"`.
  - `DISCONNECT`, and `STATUS` (a JSON array).
  - `JOB_BEGIN`: copy the job. `job->model` is the model the user picked. Return 1 if you do copies yourself, 0 if the hub should feed each copy.
  - `JOB_FEED`: take whole rows, or `PEX_EBUSY`.
  - `JOB_END`, then `prn_hub_job_done()` once the printer has the job.
  - `CANCEL`.
- puts `"driver"` in everything it emits or lists.

Claim a device only by its name. Many printers share a Class of Device
(imaging), so the CoD alone says nothing about the model: the bench PL70e-BT
was once claimed as a PM220 that way. A Classic device often has no name in
the inquiry result and gets it a moment later. `nelko_tspl` holds such a
device until the inquiry ends (and 2 s more for late names), then reports it
with `"model":null` if it still has no name, and the user picks the model.

Pointers in calls are the caller's and valid only during the call. Never
call back into the hub's feed from inside your own event callback: use
`prn_hub_kick()`, which the hub runs on the next loop pass.

Back-pressure is yours. Take only as many rows as the transport has room for:
- Classic: `pex_bt_classic_tx_room`;
- BLE: the 4-deep op queue, with writes of at most MTU − 3 and 252 bytes.

Return `PEX_EBUSY` otherwise; the hub retries every 5 ms.

## 3. Transports

- **BLE:** `pex_bt_*` (group `bt`). Use `pex_bt_scan` and `pex_bt_devices` to find the printer, `pex_bt_connect` then wait for `PEX_BT_READY`, `pex_bt_find` for the characteristics, `pex_bt_subscribe` for notifications, and `pex_bt_write(..., want_done)` for data.
  - The listener queue holds 8 events and drops the oldest.
  - Don't count on seeing every `PEX_BT_WRITE`. Retry on `PEX_EBUSY` instead.
  - Template: `btgw/pex/modules/drivers/pinecil_app.c`.
- **Classic:** `pex_bt_classic_*` (group 14). Import every function with `PEX_WEAK_IMPORT`, and check `pex_avail()` so the module still loads on an image without Classic. Template: `vendors/nelko/tspl/nelko_tspl.c`.
  - RX: the `PEX_CL_RX` event only peeks at the data. Call `pex_bt_classic_listen` in a loop until it returns 0.
  - Ending: a remote close comes as `PEX_CL_FAILED` with `why` 9.
  - PINs: answer them yourself through `PEX_CL_PIN` if you need more than one.

## 4. Tests

Driver tests are native: `drivers/test/` builds the modules with gcc `-m32`
against a fake PEX host (`fakehost/`):
- The Classic session rules are the parent's real `cl_sess.c`.
- BLE peers and Classic peers are simulated (`fh_ble.h`, `fh_classic.h`).

To add one:
1. Add your module to `drivers/test/modules.mk` (`$(eval $(call module,<short>,<sources>))` and `MODS_OBJ`), and its entry points to `drv.h`.
2. Write `test_<model>.c` with a simulated printer. `test_pm220.c` is the pattern: the peer parses the job bytes and answers the status queries. A new model row gets its own test too (`test_p21.c` tests the `p21` row of `nelko_ble` against a simulated BLE peer, `fh_ble.h`).
3. Run `make -C drivers/test` (or `tools/run_tests.py drivers`).
4. Check the cross build with `tools/run_tests.py pex` (it builds the `.pex` files and runs `pexlink dump`).

## 5. The web side

Add the model's media presets to `web/presets/media/` and list their ids in
the caps `presets`. Calibration and layout are the web app's job;
`drivers/test/golden/layout_vectors.json` holds the numbers its layout must
reproduce.

## 6. Docs

Write `docs/protocols/<vendor>-<model>.md`: the link, the commands, what was
checked on hardware, and every source with its license (`docs/references.md`).
