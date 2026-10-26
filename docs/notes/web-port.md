# Web port notes (feature/web-port)

The app in `web/` is pm220_rp2_webapp's editor (1a7cd31), ported. Commit
8d13d60 is the unchanged pm220 copy, so `git diff 8d13d60 -- web/` shows
everything the port changed.

## Running it

```
npm install                          # Playwright, once
python3 dev/mockserver.py --port 8096   # http://127.0.0.1:8096/printer/
npm run test:unit                    # node --test, pure modules
npm run test:e2e                     # Playwright against the mock (port 8097)
python3 tools/make_dist.py all       # dist/companion, dist/standalone (gzip only)
python3 tools/deploy.py companion --base http://<board>
```

Against a real board from a local page: serve `web/` (the mock does it) and
open `/printer/?host=<board-ip>`. The parent's fs.js and wsapi.js honour
`?host=` / `?port=` / `?ws=`.

## What the drivers get (for the PM220 / PL70e / P21 / D30 streams)

- The browser renders, lays out and packs. `printer.print` names a file of
  exactly `rows × ceil(width_dots / 8)` bytes, MSB first, row 0 first, in
  head orientation, with the bit sense from the model's `black_bit` caps field
  (PM220: 0 = burn, as the old firmware).
- For models with `placement: true` (TSPL), the file is the pm220 firmware's
  cropped bitmap (`job.c layout_and_build`: shift, print-height crop, and the
  all-black guard) and `origin` is the BITMAP x,y. The driver must NOT crop or
  guard again; it builds the job from `width_dots`, `rows`, `origin`,
  `media.{w_mm,h_mm,gap_mm,type}`, `density`, `speed`, `copies`.
  tests/unit/pack.test.js checks the crop against an independent port of
  job.c, and tests/unit/layout.test.js checks the layout against pm220's
  computeLayout over ~2500 calibrations.
- For `placement: false` (P21, D30 assumed) the file is head-wide
  (`head_dots`) with the origin baked in as blank columns/rows; `origin` is
  `[0,0]`.
- Density in `printer.print` is the saved printer's density, else the
  model's default; speed is `-1` when unset.
- Identical labels go as one job with `copies`; labels that differ
  (counters, `{{n}}`) go as one job each, sent one after another after the
  previous job reports `done`.
- The app waits for `printer.job` events (`queued`, `sending`, `printing`,
  `done`, `failed`, `cancelled`) with `job` = the id `printer.print` returned;
  events that arrive before the reply are buffered. It gives up after 60 s
  without one.
- Job file path: `sd/printer/var/job.bin` when a card is mounted, else
  `printer/var/job.bin` (flash).

## Model caps fallback

`web/presets/models.json` holds the caps used until `printer.drivers`
answers. The PL70e head width (832), P21/D30 head width (96), polarity,
density/speed ranges and the D30 transport are **unconfirmed** placeholders;
the driver streams own the real numbers (their caps replace these).
Label sizes: `web/presets/media/*.json`; the D30 sizes are placeholders too.

## Differences from pm220 (deliberate)

- Fixed pm220 bugs: Delete inside the text box deleted the object; Download
  saved gzip bytes as `.json`; importing a `.json.gz` failed; Ctrl+X was
  redo (now Ctrl+Shift+Z / Ctrl+Y, Ctrl+X is cut again); the saved label was
  clamped to the fallback page before media loaded; a first print could
  rasterise an image before it decoded.
- Confirmations are in-app dialogs (Cancel focused) instead of `confirm()`.
- Keys: all bindings are data in `web/keymap.js`; the help overlay is built
  from it and a unit test fails on any clash. New keys when nothing is
  selected: P printers, L label size, V variables, C calibrate, W network,
  t add text; Ctrl+B batch; `[` `]` switch Flash/SD in Open/Save.
- Wi-Fi: the parent has no persisted mDNS hostname setting and no SoftAP
  "scan policy", so those two pm220 fields are gone; the mDNS tab shows
  `mdns.status`. Mode is the parent's (auto/sta/ap/both/off).
- The pm220 doc format `pm220-label` is read (v1, v2, bare arrays) and
  written as `btgw-printer-label` v3 with media, orientation, model and
  variables.

## Open / not done

- No hardware run: nothing in this branch has been on the board (another
  stream owns it). The board's own static server serves `printer/index.html`
  at `/printer` too; index.html redirects to `/printer/` so relative module
  paths resolve.
- Companion mode serves the app from flash (`printer/`); moving it to the SD
  card waits on parent requirement R2. The switch button in the parent UI is
  DOM insertion into `#toolbar` (R3 would make it an API).
- Calibration with no saved printer lasts only for the session (by design,
  stated in the dialog).
- tools/deploy.py uploads web assets only; PEX upload/install belongs with
  the drivers.
