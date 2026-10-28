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
- **One driver per protocol family and transport; a model row per printer.** A driver may cover several printers (a model table). A printer that differs a lot gets its own driver. Today: `nelko_tspl` (PM220; Classic SPP), `nelko_ble` (P21; BLE), `nelko_pl70e` (PL70e-BT; BLE), `phomemo_d30` (D30; Classic SPP, ESC/POS), `printer_virtual`. See [adding-a-printer.md](adding-a-printer.md).
- **The web app renders.** It lays out the label, applies calibration and packs the 1-bit bitmap. Drivers only encode and transport it.
- **Plain ES modules, gzipped, never minified or bundled.** Modules load lazily, so the board's SD route is never asked for many files at once.
- **Be gentle with the board.** Other clients and WebSockets share it. The app keeps at most 2 HTTP requests in flight (one queue for module loads, presets, settings and files), backs off on `409 busy` / `503`, and our tools never load-test it.
- **Two modes.** Companion: the parent UI plus our app on the SD card at `/sd/apps/printer/`. Standalone: our app is the only web app. [architecture.md](architecture.md).
- **We never change the firmware on a board.** Printer support is PEX modules plus our own folder on the card (`sd:/apps/printer/`). Building the parent firmware here is a compile check. A need on the firmware side goes into [parent-requirements.md](parent-requirements.md).
- **Calibration is per printer × label size** and keeps the pm220 fields, names and limits. Old pm220 `settings/print.json` files load in the calibration dialog.

## Now

- [ ] **F1 — First labels from the browser on real printers**
  - Area: web, hub, driver.
  - Why: Every printer prints in mcusim, and the PM220 has printed a test page sent from a PC script. Nothing has printed from the app's own UI yet.
  - Needs: F2 for loading from the card (or `tools/serve_dist.py` meanwhile). On the bench board printer_hub and the driver are installed from `sd:/apps/printer/pex/` and started by op after a boot (parent B3 stops autostart). Replacing a module there needs a reboot until parent B5/R11 are fixed; we don't reboot to get around firmware bugs ([notes/integration.md](notes/integration.md)).
  - Done when: from the app, a calibrated 50×30 label with text, QR and barcode prints on the PM220, placed where the old pm220 app put it; a 3-row CSV batch prints; then a 4×6 label prints on the PL70e-BT, and a 3×1 when that roll is in.
  - 2026-10-27: PM220 printed a 384×239 test page through printer_hub + nelko_tspl from tools/bench_print.py (job file on the card). The owner confirmed the label.
  - 2026-10-27: from the app on the card (`/sd/apps/printer/`, lazy build, peak 2 requests): the saved PM220 auto-connected and a 50×30 label with text, QR and barcode printed ("printed 1 label"). Sharp, inside the label, placed as in the preview (photo from the owner). Still open: a deliberate layout compared with the old pm220 app, a CSV batch, then the PL70e-BT.
  - 2026-10-27, later: the PM220's calibration test print (factory −1.25/+3 mm, BITMAP 0,24 384×216) printed from the app ("test print done"). The PL70e-BT, found by the app's "Scan for", connected over BLE and printed the 3×1 calibration test page and a 3×1 text label ("printed 1 label") after two driver fixes (a lost write event, parent B6; the PRINT line not sent when JOB_END found the ring full). The owner's photo of both 3×1 labels shows the placement is right with no offset: the test page's dashed outline sits inside the label edges (under 1 mm off), and the text sits at the label's top-left as read. Still open: a layout compared with the old pm220 app, a CSV batch, a 4×6 on the PL70e.

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
  - Done so far (6938af0): each saved printer remembers its label size and orientation (`media`, `orient` in printers.json), and switching to it, or a page load in a clean browser, brings them back. Calibration was already saved per printer and label size.

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

- [ ] **F9 — Don't knock the board off Wi-Fi when a printer is off**
  - Area: web, hub, driver, parent.
  - Why: Paging a switched-off Classic printer stalls the gateway's Wi-Fi for ~35 s per attempt (parent B4). The app auto-connected to the saved printer on every page load, so opening the app with the printer off made the board unreachable, the app's own loading included. BLE does not do this.
  - Needs: nothing pages a known (saved in `printers.json`) Classic printer without the user. Web: every connect goes through `hub.js` `requestConnect`, which for a saved Classic printer first shows "Turn on the printer" (Connect / Cancel) and only Connect sends `printer.connect`; at load that prompt comes once, after boot; Cancel leaves a Connect action on the status line; Print with the printer down asks the same and goes on after "connected". No prompt for BLE, for a printer being added from a scan, or for one a scan in this page found, until the driver reports it lost again (`why` "link lost" / "not reachable" / "stopped retrying"). Hub and driver: no connect of a Classic printer on their own (on driver registration, or retries after an unreachable page). Revisit once B4 is fixed.
  - Done when: e2e on the mock: a saved Classic printer gets no `printer.connect` at boot, the prompt shows, Cancel sends none and leaves Connect, Connect sends one; a BLE printer connects with no prompt; Print with a disconnected Classic printer asks, prints after Connect and prints nothing after Cancel; a printer picked from a scan, or found by one, connects without the prompt (tests/e2e/connect.spec.js). And the hub and driver side checked in mcusim or on the bench.
  - Web side done 2026-10-27 (browser tests, all three mock projects). Open: the hub/driver side.
  - 2026-10-27 bench: the prompt showed for the saved PM220 and Connect connected it; the hub and nelko_tspl with the one-attempt rule (fcf49a7) are on the board. Not checked on hardware: a connect to a PM220 that is off (that costs the board ~35 s of Wi-Fi, B4), so the driver side rests on the fake-host tests until it can be checked in mcusim.

- [ ] **F10 — New objects don't land on top of each other**
  - Area: web.
  - Why: Adding a text box, a QR code and a barcode in a row puts all three at the same spot (seen on the first browser print). Check what pm220_rp2_webapp did and match it, or place each new object in free space inside the safe area.
  - Done when: e2e: three objects added in a row don't overlap on a 50×30 label.
  - Also (2026-10-27, PL70e 4 × 6): new objects keep their small-label sizes (text 24 px, QR 80 dots, barcode 200 dots) on a 812 × 1218 page, so they come out tiny in a corner. Size new objects in proportion to the safe area. Done when also: on a 4 × 6 a new text box, QR code and barcode are readable sizes (e2e checks a minimum size against the page).

- [ ] **F12 — A printer that stops answering fails the job instead of hanging**
  - Area: drivers (nelko_pl70e first, then nelko_tspl).
  - Why: On 2026-10-27 the PL70e stopped answering READSTA after its roll was reloaded. A 3 × 1 job went out in full and never printed; the hub kept it "printing" until the app gave up after 60 s.
  - Needs: during a job, READSTA (or the model's status query) with no answer for a set time ends the job as failed with "the printer isn't answering", and the status shows that. Use the Nelko app's own timeouts and states (docs/protocols/nelko-states.md). Find out with the owner what the printer needs after a paper load.
  - Done when: a fakehost test where the printer never answers ends the job failed within the timeout, and the same on the bench with the printer silenced.
  - Done so far: 442b039 (a silent printer fails the job in 30 s; a quiet one refuses a new job; status shows unanswered/quiet_ms/writes_failed) and ca06db1 (failed writes are seen at last and resync the printer; a fresh ready answer before every job, as the Nelko app does; SET GAP ON; `recover` connect option). Left: the bench check, and finding the cause with the plan in docs/protocols/nelko-states.md §6.

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

- [ ] **F13 — The P21 prints over BLE**
  - Area: driver, web, parent.
  - Why: The owner's P21 (arriving 2026-10-28) uses BLE. It was a Classic model row of `nelko_tspl`; the P21 is dual mode at one address, so it is now the `p21` row of a new BLE driver, `nelko_ble` (feature/p21-ble), and the Classic row is gone.
  - Needs: the bench plan in [protocols/nelko-p21.md](protocols/nelko-p21.md) ("Bench plan (P21)"): install nelko_ble from `sd:/apps/printer/pex/` (14,844 B: it needs parent R11 item 1, see R13), scan, status, a first label from a script and from the app; answers to that file's open questions (does the gateway see the P21 over LE, the MTU, `ff03` credits, the write gap).
  - Done when: a 14 × 40 label printed from the app on the bench P21, placed as in the preview, and a 3-copy 14 × 75 job with no `writes_failed`.
  - 2026-10-27 (feature/p21-ble): fake-host tests (test_p21.c: scan, connect, codes, the golden job at MTU 23 and 247, copies, credits, a failed write and the resync, a silent printer, paper out, power-off notice, the real hub), browser tests, and mcusim (`tools/e2e_sim.py p21`: 96×320 → 0 dots differ). Not on hardware yet.

## Parked

- [ ] **F8 — What standalone mode ships**
  - Area: parent, web.
  - Why: Standalone was planned as a firmware image built from the parent with our config (no parent web UI, our app at `/`). We don't change firmware on a board, so either that image is only a release artifact for dedicated boards (one inside a printer), never flashed onto an existing gateway, or standalone becomes the same unmodified firmware with our app as the start page, which needs a parent setting.
  - Needs: the owner's decision. The second option is a parent requirement.
  - Done when: the decision is recorded here and in architecture.md, and `tools/build.py standalone` / the release notes match it.

## Done

- [x] **F11 — The app starts the printer service it needs** (2026-10-27)
  - Area: web.
  - Why: Nothing starts our modules after a boot (B3, and the parent's launchers don't read our `pex` list). After a reboot the app said "no printer_hub · designing only" though both modules sat installed in the PEX partition.
  - Needs: at boot, when `printer.drivers` is `unknown_op`: `pex.load` + `pex.start` printer_hub, then wait for it (≤ 5 s, once a second). A driver only when it is needed: before connecting a printer of it, and before a scan for its printer family ("Scan for" in the Printers dialog; our drivers are app.json's `pex` list, else nelko_tspl). Before loading a driver, unload our other drivers that have no printer in use (only the driver in hand stays loaded). "Already loaded / running" is success; ops one at a time; a failure shows the board's message and Retry, never a retry loop.
  - Done when: e2e on the mock (tests/e2e/service.spec.js: boot starts only the hub; a saved PM220's connect loads nelko_tspl first; loading a driver first unloads an idle one, not one with a printer connected; pool full with a printer on the other driver shows the message and Retry; a scan for a family loads its driver; all running sends no pex ops; a hub that won't start shows the message and Retry), and on the bench board after a reboot the app starts hub + driver by itself.
  - 2026-10-27 bench: with our modules unloaded (the state after a boot), the app sent pex.load + pex.start for printer_hub only, then loaded nelko_pl70e for the saved PL70e and connected; after a real board reboot it did the same. Open: the app loaded nelko_pl70e next to an idle nelko_tspl and the pool fell to 32 bytes, so every print failed ("open failed"); it must unload idle drivers first (in progress).

## Later
  - 2026-10-27, after unload-first (a1235c2): the browser tests pass (90/90). On the bench the app auto-connected the BLE PL70e at boot and, asked to scan for PM220s, kept nelko_pl70e (a printer was connected on it) and loaded nelko_tspl beside it (pool 9,440 B left). The unload-an-idle-driver path is covered by the browser tests only.

## New item

```
- [ ] **F# — Title**
  - Area:
  - Why:
  - Needs:
  - Done when:
```
