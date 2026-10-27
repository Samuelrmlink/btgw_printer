# Roadmap

What btgw_printer does next, in order. The parent firmware's own list is
`btgw/docs/Feature-Roadmap.md`. What we need from the parent is
[parent-requirements.md](parent-requirements.md) (its items are `C#`, `R#` and `B#`; ours here are `F#`, so the two never clash).

A box is ticked when the behaviour is true of the branch that holds it and has been seen working: in the browser tests, in mcusim, or on the bench board (10.255.255.23) when the item needs hardware. Write down which.

## How to change this list

Copy the template at the bottom into Now, Next, Later or Parked. Fill in the area, why, what it needs and the condition that ticks the box.

The `F` number is the order of implementation. Now is the front of that order, then Next, then Later. An item that cuts in line takes the lower number, and the items it passes move up one. When the done condition is met, tick the box, move the whole entry under Done and add the date. Leave it there so the reason stays readable.

Work is committed on a `feature/*` branch. The owner vets it before it merges into `dev`. Releases go from `dev` to `main` with a tag and a GitHub release (README.md, Branches).

Parked means not now. Out of scope means it was considered and rejected.

Areas: **web** (the app in `web/`), **hub** (`drivers/hub`), **driver** (a module under `drivers/vendors`), **sim** (mcusim printer simulators), **parent** (needs a parent change first).

## Recorded

These are part of the design. They are not tasks.

- **Drivers are PEX modules.** printer_hub owns every `printer.*` op and event. Each driver is its own module behind `drivers/include/printer_driver.h`.
- **One driver per protocol family and transport; a model row per printer.** A driver may cover several printers (a model table). A printer that differs a lot gets its own driver. Today: `nelko_tspl` (PM220, P21; Classic SPP), `nelko_pl70e` (PL70e-BT; BLE), `phomemo_d30` (D30; Classic SPP, ESC/POS), `printer_virtual`. See [adding-a-printer.md](adding-a-printer.md).
- **The web app renders.** It lays out the label, applies calibration and packs the 1-bit bitmap. Drivers only encode and transport it.
- **Plain ES modules, gzipped, never minified or bundled.** Modules load lazily, so the board's SD route is never asked for many files at once.
- **Be gentle with the board.** Other clients and WebSockets share it. The app keeps at most 2 HTTP requests in flight (one queue for module loads, presets, settings and files), backs off on `409 busy` / `503`, and our tools never load-test it.
- **Two modes.** Companion: the parent UI plus our app on the SD card at `/sd/apps/printer/`. Standalone: our app is the only web app. [architecture.md](architecture.md).
- **Calibration is per printer × label size** and keeps the pm220 fields, names and limits. Old pm220 `settings/print.json` files load in the calibration dialog.

## Now

- [ ] **F1 — First labels from the browser on real printers**
  - Area: web, hub, driver.
  - Why: Every printer prints in mcusim, and the PM220 has printed a test page sent from a PC script. Nothing has printed from the app's own UI yet.
  - Needs: F2 for loading from the card (or `tools/serve_dist.py` meanwhile). On the bench board printer_hub and the driver are installed from `sd:/apps/printer-pex/` and started by op after each boot (parent B3 stops autostart; module memory is parent R11).
  - Done when: from the app, a calibrated 50×30 label with text, QR and barcode prints on the PM220, placed where the old pm220 app put it; a 3-row CSV batch prints; then a 4×6 label prints on the PL70e-BT, and a 3×1 when that roll is in.
  - 2026-10-27: PM220 printed a 384×239 test page through printer_hub + nelko_tspl from tools/bench_print.py (job file on the card). The owner confirmed the label.

- [ ] **F2 — Lazy loading**
  - Area: web.
  - Why: The board's `/sd` route serves up to 6 files at once (raised from 2) and answers `503 card busy` beyond that. Too many requests at once may also be dropped. Today app.js imports about 25 modules at boot.
  - Needs: a small boot graph; dialogs, QR, barcode, image and print code loaded on first use; one request queue with at most 2 in flight (module loads, presets, settings, files) and backoff on `409 busy` / `503`; an idle prefetch one file at a time, if it's worth it. The mock server refuses a third parallel static request like the board does, and the whole e2e suite runs under it.
  - Done when: `/sd/apps/printer/` on the bench board loads with zero failed requests in Chromium and never more than 2 requests in flight, and every dialog opens by keyboard.

## Next

- [ ] **F3 — Switching printers checks the label size**
  - Area: web.
  - Why: A label size is made for some models. Switching the printer to another model today swaps the label size without telling the user when it doesn't fit (`setModel` in web/app.js), and the document is re-laid out on the new page.
  - Needs: on a printer switch, check the current label size against the new printer's model (`mediaFor`). If it fits, keep it and its calibration. If not, choose the model's preferred preset (or the label size this printer used last), and say on the status line what changed and why. F4's prompt runs first.
  - Done when: e2e: PM220 with 50×30 → PL70e-BT switches to a 4×6-family size and says so; PM220 → another PM220 keeps 50×30; a size marked for both models is kept.

- [ ] **F4 — Ask before a switch throws away unsaved changes**
  - Area: web.
  - Why: Changing the printer type or the label size re-lays out the label. The user should get the chance to save their work first.
  - Needs: unsaved-change tracking for the open label (compared with the last save or open, not a flag set by every keystroke; undo back to the saved state makes it clean again). Before a printer-type or label-size switch with unsaved changes: a dialog with Save (or Save as…, when the label has no name yet), Discard and Cancel. Save then switch; Discard drops the edits and switches; Cancel leaves everything as it was. Switching to a printer whose model can't use the current label size also switches the label size (F3), after the prompt. Keyboard-reachable, with the default button on Enter and Esc as Cancel. The same tracking can mark the title (for example "•") and guard closing the tab (`beforeunload`).
  - Done when: e2e for each answer (Save, Discard, Cancel) on both a printer-type switch and a label-size switch; a clean label switches without asking.

- [ ] **F5 — The label dialog lists only compatible sizes, and custom sizes are easy to add**
  - Area: web.
  - Why: The dialog already filters by the current model (`mediaFor`, web/media.js), and a custom-size editor exists. But a custom size and "made for which printers" aren't obvious, and a size made for another model never shows.
  - Needs: the list shows only sizes compatible with the selected printer's model, grouped (this printer's presets, your sizes, others marked "any printer"). A visible "New custom size…" entry opens the editor with this model ticked; edit, duplicate and delete for user sizes (presets read-only; "duplicate" makes an editable copy). A size can be marked for several models or for any. Custom sizes save to `media.json` in the settings store (card when flash can't be written). Checked against the model's head width and length range.
  - Done when: e2e: the PM220 sees only PM220 and "any" sizes; a new 60×40 custom size is listed, survives a reload, and appears for the PL70e only when ticked for it; a size wider than the head is refused with a message.

- [ ] **F6 — Vim-style keys match pm220_rp2_webapp**
  - Area: web.
  - Why: The keymap was ported from `~/Projects/pm220-pico2w/web` (n/N/Space browse, m/r/d/Del/Esc/q, hjkl/HJKL nudge, Tab order, help overlay, dialog navigation, number-field steps) but never checked key by key against the old app. It may be refined after that.
  - Needs: a table in `docs/keys.md`: every binding in the old app (from its source, and by trying it in the old app under the mock), what ours does, and the difference. Fix the ones that differ unless the difference is deliberate (new dialogs, the clash fixes listed in docs/notes/web-port.md); write the deliberate ones down. An e2e per row of the table where it can be tested.
  - Done when: the table is written, every row matches or has a reason, and the e2e covers it. Refinements after that are new items.

## Later

- [ ] **F7 — Menus: a traditional menu bar on desktop, a collapsible menu sheet on touch**
  - Area: web.
  - Why: The header and sidebar from pm220 don't scale to printers, label sizes, templates, variables, batch, network and calibration. A menu bar (File, Edit, View, Printer, Label, Network, Calibration, Help) with submenus and tick/untick items is easier to find things in on a desktop. On a touchscreen, small drop-downs and hover submenus work badly. A full-height sheet with collapsible sections is likely better there.
  - Needs, planning first (write `docs/menus.md` before any code):
    - One menu model, two renderers. A single description of the menus (items, submenus, checkable and radio items, enabled state, shortcuts) drives both the desktop menu bar and the touch sheet, so they can't drift apart.
    - Which renderer: decide by input type (`pointer: coarse` / `any-hover`), not width alone; allow a setting to override; a tablet with a keyboard should get the menu bar.
    - Desktop: click or Alt+letter opens a menu; arrows move, Right/Left open and close submenus, Enter activates, Esc closes; ticks for toggles (show safe area, show grid, invert preview…) and radio groups (label size, orientation); shortcuts shown on the right; ARIA `menubar`/`menu`/`menuitemcheckbox`/`menuitemradio`.
    - Touch: one button opens a sheet; sections collapse and expand (accordion), submenus become nested sections; toggles are switches; rows at least 44 px high; the sheet closes on choose for actions but stays open for toggles.
    - Keyboard map: the vim-style keys (F6) keep working with menus closed; a key opens each top-level menu; the help overlay and the clash test include menu keys.
    - What moves where: a list of every current button, dialog and setting and its menu path, so nothing is lost.
    - The user decides: menu names and order, which items are toggles, and whether the sidebar stays on wide screens.
  - Done when: `docs/menus.md` is agreed with the owner, both renderers are built from one model, every current action has a menu path, and e2e covers both renderers (desktop by keyboard and mouse, touch by tap in a mobile viewport).

## Parked

## Done

## New item

```
- [ ] **F# — Title**
  - Area:
  - Why:
  - Needs:
  - Done when:
```
