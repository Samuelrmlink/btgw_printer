# Integration notes (feature/integration, 2026-10-26)

`feature/integration` is bootstrap plus nelko-pm220, p21-d30, nelko-pl70e and
web-port merged, then reconciled.

## What changed while reconciling

- **P21 / D30 on the final hub contract** (`drivers/vendors/common/spp_printer.inc.c`, `p21_app.c`):
  - media in hundredths of a mm;
  - `"driver"` in every listed and emitted object;
  - `PRN_DRV_STATUS` returns an array;
  - `JOB_BEGIN` returns 0, so the hub feeds the rows once per copy.
- **`placement` in every driver's caps:**
  - true: PM220, PL70e, virtual;
  - false: P21, D30.

  The web app reads it from the caps (`web/pack.js`) instead of only its own fallback.
- **One media preset id per label size,** shared by drivers and the web app:
  - `nelko-pm220-*`, `nelko-4x6/3x1/4x4/4x3/2x1`, `nelko-p21-*`, `phomemo-d30-*`;
  - the duplicate `phomemo.json` and the P21 rows in `nelko.json` are gone;
  - added `nelko-pm220-40x20/50x20/50x80` and `nelko-4x3` (marked unconfirmed), which the drivers listed.
- **`web/presets/models.json` is now the drivers' caps:**
  - it had placeholders: the P21 black bit, the D30 transport, the PL70e speed;
  - the virtual driver is `printer_virtual`, its module name;
  - `drivers/test/test_caps.c` compares every field and fails when they drift.
- **Classic drivers open at security level 0,** with a `"security_level"` override in `printers.json`. Reason: parent bug B1 in `docs/parent-requirements.md`.
  - The fake host now pairs a PIN printer at level 0 as the printer asks, the way real printers and mcusim do.
- **PM220 walks its PIN list across attempts.**
  - In mcusim the firmware fails the whole session when the printer rejects a PIN (HCI 0x05, then FAILED why rfcomm), so answering the next PIN within the same attempt never happens.
  - The next attempt starts at the next PIN, 500 ms later.
  - Before this fix the driver tried 0000 forever.
- **Module builds:** `prn_add_module()` in `drivers/CMakeLists.txt` links each module twice:
  - `build/drivers/pex/<name>.<arch>.pex` without debug info;
  - `pex/debug/` with it.
- **TSPL height for continuous media** uses 32-bit math.

## nelko_tspl merge (2026-10-27)

The rule: every driver is a PEX module; one driver per protocol family and
transport; a driver may cover several models through a model table.

- **`nelko_pm220` and `nelko_p21` are now one module, `nelko_tspl`** (`drivers/vendors/nelko/tspl/`).
  - It is pm220.c, with a model row each in `MODELS[]`: `pm220`, `p21`. A P22 is one more row.
  - A row has the id, the name rule, the head width, the density range, the PINs, the caps object, and flags for what differs (placement, copies, status byte as bits or codes, a check after the job, ESC !? with or without CRLF, the P21 job text, ESC !o on connect).
  - `pm220_proto.c` is `nelko_proto.c` (prefix `nk_`). It also has the P21 status names now. `p21_app.c` and `p21_proto.c` are gone.
  - The P21 job text comes from `proto/tspl` with two new fields: `mm_tenths` ("14.0 mm") and `mirror` (`DIRECTION 1,1`). The nelko-p21-rs golden files still match byte for byte.
  - `spp_printer.inc.c` stays, for the D30.
- **Matching (found on the bench):** pm220.c claimed any unnamed device of Class of Device major class 6. The bench PL70e-BT (Classic DC:0D:30:89:6C:C5, CoD 0x040680) shows up unnamed and gets its name "PL70e-BT_6CC5" a moment later, so it was reported as a PM220.
  - Now a device is claimed only when a model's name rule matches ("PM220" anywhere, "P21" at the start, any case).
  - An unnamed imaging device is held until the inquiry ends, plus 2 s for late names. If it still has no name it is reported once, with `"model":null`. `printer.devices` uses the same rule.
  - Connect takes the model from the config's `model`, else the inquiry name, else the first model (and says so in `printer.log`). A job's `model` overrides it.
  - The fake host's remote name requests now have a delay (`rnr_ms`) and may land after the inquiry. `test_nelko_scan.c` covers these cases.
- **Changes the P21 sees** (it was on `spp_printer.inc.c`):
  - an `ESC !?` check before the job, as the PM220 has;
  - `printer.status` fields as documented: `paper` and `lid` are booleans, `fw` replaces `firmware`, `printer` is the status code's name; the roll's colour and code are no longer reported;
  - the keepalive asks `BATTERY?` only.
- **Size:** `nelko_tspl.m33.pex` 14,564 B (bss 1,436) against `nelko_pm220` 12,188 B (bss 1,376) plus `nelko_p21` 10,904 B (bss 2,384).
- `web/presets/models.json` has one `nelko_tspl` entry with both models. Media preset ids are unchanged. `test_caps` also checks that `printer.drivers` stays under 2.8 KB (2,193 B now).
- mcusim, `tools/e2e_sim.py pm220 p21` with `nelko_tspl`: 0 dots differ for both. The PM220 connect had no model in its config and got `pm220` from the inquiry name; PIN 1234 worked on the second attempt.
- Saved printers with `"driver":"nelko_pm220"` or `"nelko_p21"`: the web app loads them as `nelko_tspl` with their model (`web/store.js`). The hub's auto-connect reads `printers.json` itself, so it skips them until the web app has saved the file once.

## End to end in mcusim

`tools/e2e_sim.py` (`tools/run_tests.py sim`):
1. boots the standalone image with mcusim's printer simulators (feature/printer-sims);
2. uploads printer_hub and one driver at a time (`pex.load` from LittleFS, `pex.start`);
3. builds the job with the web app's own `pageFor` / `buildJobBitmap` (`tools/e2e_job.mjs`) and writes it to `printer/var/job.bin`;
4. runs `printer.scan` / `connect` / `print` and waits for `printer.job` done;
5. compares the simulator's page PNG with the bitmap placed at its origin.

`run_btgw.py`'s readiness check polls `/api/sd`, which the standalone image never answers. e2e_sim waits for `/api/fs/`, joins the fake AP and waits for `bt.state ready` itself.

Results (2026-10-26, standalone image 835,680 B, parent b397a84, mcusim feature/printer-sims c8c1059):

| printer | bitmap (origin) | page | dots differing |
|---|---|---|---|
| PM220 (Classic, legacy PIN 1234: 0000 rejected, 1234 next attempt) | 384×216 at 0,24 | 400×240 | 0 |
| P21 (Classic, SSP) | 96×320 at 0,0 | 112×320 | 0 |
| D30 (Classic, SSP, ESC/POS raster) | 96×320 at 0,0 | 96×320 | 0 |
| PL70e-BT (BLE) | 812×1218 at 0,0 | 832×1218 | 0 |

A full run takes about 25 minutes of wall time.

## Open

- The simulated PM220 rejects the PIN with HCI 0x05 and the firmware reports why rfcomm. A real PM220 may differ; the driver now copes with both.
- Parent bugs B1 to B3 and C10: `docs/parent-requirements.md`, section C.
- Hardware: nothing in this branch has run on the bench.
