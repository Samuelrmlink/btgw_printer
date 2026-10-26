# Adding a printer

A printer is a PEX app module that registers with `printer_hub`. The web app
does all the drawing; the driver turns rows into the printer's command
language and moves them over BLE or Bluetooth Classic.

## 1. Where it goes

```
drivers/vendors/<vendor>/<model>/
  <model>.c          the module: prn_drv(), pex_main()
  <model>_proto.c/h  pure protocol code (framing, status parsing): host-tested
  module.cmake       prn_add_module(<vendor>_<model> ...)   (same arguments as pex_add_module)
```

`drivers/CMakeLists.txt` picks up every `*/module.cmake` by itself. Give the
module a name of its own (`nelko_pm220`). That is the driver name in every op
and event. Copy `drivers/vendors/nelko/pm220/module.cmake`: list
`core/prn_core.c` (and `proto/tspl/tspl.c` for TSPL) in `SOURCES`, and add
`${PRN_INCLUDE}` plus the `drivers/` directory to `INCLUDES`.

## 2. The contract

Read `drivers/include/printer_driver.h`. The driver:

- exports one `int prn_drv(cmd, a0, a1, a2)` (`PEX_EXPORT(prn_drv)`);
- registers in `PEX_APP_RUN` with `prn_hub_attach(&hub, pex_name(pex_self()))`, and retries from its tick when the hub was not up yet (`prn_hub_ok()`);
- unregisters in `PEX_APP_STOP` (`prn_hub_detach`);
- answers:
  - `INFO`: caps JSON with one entry per model. The hub caches `id`, `black_bit`, and the `density` and `speed` ranges.
  - `SCAN`: start or stop finding printers; report each new one with `prn_hub_emit(&hub, "found", ...)`.
  - `DEVICES`: a JSON array.
  - `CONNECT`: asynchronous. Report progress with `"state"` emits.
  - `DISCONNECT`, and `STATUS` (a JSON array).
  - `JOB_BEGIN`: copy the job. Return 1 if you do copies yourself, 0 if the hub should feed each copy.
  - `JOB_FEED`: take whole rows, or `PEX_EBUSY`.
  - `JOB_END`, then `prn_hub_job_done()` once the printer has the job.
  - `CANCEL`.
- puts `"driver"` in everything it emits or lists.

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
- **Classic:** `pex_bt_classic_*` (group 14). Import every function with `PEX_WEAK_IMPORT`, and check `pex_avail()` so the module still loads on an image without Classic. Template: `vendors/nelko/pm220/pm220.c`.
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
2. Write `test_<model>.c` with a simulated printer. `test_pm220.c` is the pattern: the peer parses the job bytes and answers the status queries.
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
