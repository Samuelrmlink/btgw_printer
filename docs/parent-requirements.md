# Requirements for bluetooth_rp2_webapp

From: btgw_printer (a downstream project; private repo Samuelrmlink/btgw_printer)
To: the agent maintaining bluetooth_rp2_webapp
Written against: `origin/dev` at d81ecd6 ("Finish Bluetooth Classic BNEP in both directions"), 2026-10-25

## How btgw_printer uses the parent

btgw_printer never edits the parent. It depends on the parent like this:

- **Submodule:** the parent is pinned as a git submodule at `btgw/`.
- **Firmware:** built with `cmake -S btgw -B build/<mode> -DAPP_DEFCONFIG=<merged defconfig>`. The merged defconfig is one of the parent's `configs/*_defconfig` files plus a small fragment from btgw_printer.
- **Printer drivers:** PEX modules built out of tree with `include(btgw/pex/sdk/cmake/pex.cmake)` and `pex_add_module(...)`, then signed with the same OTA key.
- **Web app:** our own app, talking to the parent over the existing WS `/ws` and `POST /api/op` interface.
- **Two images:**
  - Companion: the parent's SD image and UI, plus a launcher for our app hosted on the SD card.
  - Standalone: no SD or W5500 by default, RP2350 first, RP2040 later. Our app is the only web app on the board.

Each item below says what we need, why, and how we'll know it's done. Priorities:

- **Must:** blocks a btgw_printer feature.
- **Should:** we can work around it, but the workaround is worse.
- **Nice:** useful, can wait.

The PEX function names are proposals. Rename freely, as long as the shape stays the same.

---

## A. Bluetooth Classic (SPP/RFCOMM) for printer drivers

### Why

The Nelko PM220 label printer only speaks Bluetooth Classic SPP. Its driver must be a PEX module. The standalone pm220_rp2_webapp firmware (`~/Projects/pm220-pico2w`, `src/bt/bt_core.c`) shows the full sequence the driver needs:

1. Inquiry, keeping devices whose Class of Device major class is 6 (imaging) or whose name matches.
2. SDP lookup of the RFCOMM channel for UUID 0x1101, falling back to channel 1.
3. Pairing: legacy PIN "0000", retried with "1234". SSP just-works. On an auth failure, drop the stale link key and retry once.
4. Open the RFCOMM channel.
5. Stream a 16–130 KB print job in MTU-sized chunks, paced by `RFCOMM_EVENT_CAN_SEND_NOW`.
6. Read the status replies (`BATTERY?`, `CONFIG?`, `ESC !?`, `ESC !o`, each a few to ~20 bytes).
7. Send a keepalive every 25 s.
8. Disconnect, and reconnect with backoff.

Other SPP printers (ESC/POS receipt printers, other TSPL label printers) follow the same sequence.

### What origin/dev has today

Classic is firmware-only:
- `src/bt/classic/spp.c` is one global state machine (one peer, one RFCOMM channel).
- Ops: `classic.scan`, `classic.connect`, `classic.state`, plus `classic.nap`.
- Only `configs/pico2w_sd_bnep_defconfig` turns it on, with TLS off so it fits.
- There is nothing in `pex/host_api.json` or Lua.
- Roadmap R16 says "Not tried against a PM220".

| Need | origin/dev d81ecd6 | Item |
|---|---|---|
| Inquiry with name and RSSI | Yes: `gap_inquiry_start(8)`, EIR name, `SCAN_MAX 8` | — |
| CoD reported and filtered (major 6, name prefix); more than 8 results; remote name request when there's no EIR name | No: CoD is stored in `scan_dev_t` but never reported; `HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE` is only logged | C3 |
| SDP 0x1101 → RFCOMM channel, fallback to channel 1 | Yes: `start_sdp_query()` / `handle_sdp()` | (expose through C1) |
| Legacy PIN list ("0000", then "1234") with retry on auth failure | Partly: PINs are hard-coded, and "1234" is used only if the peer asks a second time in the same attempt | C1, C2 |
| SSP just-works | Yes: `NO_INPUT_NO_OUTPUT`, auto-accept | — |
| Drop stale link key and retry once | Partly: only manually, via `bond.remove` | C2 |
| Open RFCOMM, learn the MTU | Yes: `open_rfcomm()`, MTU taken from `RFCOMM_EVENT_CHANNEL_OPENED` | — |
| General write: chunked to MTU, paced by CAN_SEND_NOW, 16–130 KB jobs | No: the only write is a fixed `"BATTERY?\r\n"`, sent once | C1, C2 |
| RX buffered and delivered | No: each packet overwrites the 80-byte `s_last_rx` | C1, C2 |
| Disconnect, connect timeout, link-loss notice | No | C1, C2 |
| Structured events (found, opened, closed, data) | No: only `bt_log` lines | C1, C7 |
| PEX API | No: there is no Classic group | **C1** |
| Wi-Fi coexistence during inquiry, paging and SDP | No: `le_radio_busy()` (`src/bt/le/central.c`) does not count Classic | **C4** |
| A Classic build without NAP or peripheral that fits the standalone (`pico2w`) and companion (SD) images, ideally with TLS on | Not measured; `HCI_ACL_PAYLOAD_SIZE` is hard-wired to 1691+4 for every Classic build | **C5** |
| A Classic-only image doesn't advertise itself | Probably: `nap_init()` (discoverable, CoD 0x020300, local name) runs only when `CONFIG_BT_CLASSIC_NAP` is on (`src/bt/bt.c:413`) | C6 (confirm) |
| One Classic link alongside BLE links (PM220 and PL70e-BT at once) | Not coordinated: the 4 HCI links and 3 ACL buffers are shared | C8 |

### C1 (Must): a PEX `classic` host-API group

Add a new group (id 14 is free) in `pex/host_api.json`. It should be weak-importable, so a module can test it with `pex_avail()` and still load on images without Classic.

How it should behave:
- Events reach the module from the main loop, never from inside BTstack, the same way the `bt` group works (`src/pex/pex_gw_bt.c`).
- Channels and listeners a module opens are released when it stops or unloads, using the same owner bits as ops.

Proposed shape:

```c
/* Inquiry */
int  pex_bt_classic_inquiry(uint32_t ms, uint32_t cod_mask, uint32_t cod_value,
                            const char *name_prefix);       /* 0 / PEX_EBUSY */
int  pex_bt_classic_inquiry_stop(void);
int  pex_bt_classic_devices(pex_cl_dev_t *out, int max);    /* {addr[6], name[32], rssi, cod} */
/* events: PEX_CL_FOUND{index}, PEX_CL_INQ_DONE */

/* SDP */
int  pex_bt_classic_sdp_rfcomm(const uint8_t *addr, uint16_t uuid16);
/* event: PEX_CL_SDP{channel[], name} or PEX_CL_SDP_FAILED */

/* Channels */
typedef struct {
    const char *pins[4];     /* legacy PINs tried in order, NULL-terminated */
    uint8_t     drop_key_retry; /* on auth failure: drop link key, retry once */
    uint32_t    timeout_ms;  /* whole connect, including SDP and pairing */
} pex_cl_opts_t;
int  pex_bt_classic_open(const uint8_t *addr, int channel /* 0 = look up SPP by SDP */,
                         const pex_cl_opts_t *o);           /* handle >= 0, or error */
/* events: PEX_CL_OPEN{handle, mtu}, PEX_CL_FAILED{handle, reason}, PEX_CL_CLOSED{handle, reason} */

int  pex_bt_classic_send(int h, const void *data, size_t len);
/* Copies into a host TX buffer of at least 2 x MTU and returns the bytes
 * accepted, or PEX_EBUSY. The host drains it on RFCOMM_EVENT_CAN_SEND_NOW.
 * event: PEX_CL_TX_ROOM{handle, free} once room comes back */
int  pex_bt_classic_tx_room(int h);
int  pex_bt_classic_listen(int h, pex_cl_fn fn, void *arg);
/* RX comes from a host ring buffer of at least 1 KB, delivered as data
 * events of up to 252 bytes, with an overflow counter in _state */
int  pex_bt_classic_close(int h);
int  pex_bt_classic_state(int h, pex_cl_state_t *out);     /* state, addr, channel, mtu, rx_drops, last error */
int  pex_bt_classic_forget(const uint8_t *addr);           /* drop the link key */
```

**Done when:** a test module (we will supply ours, `drivers/vendors/nelko/pm220`) can:
1. inquire and find the PM220 by CoD;
2. open the channel with the PIN list;
3. exchange `BATTERY?`;
4. stream a 16 KB TSPL job that prints;
5. close;
6. stop and unload with nothing left registered.

### C2 (Must): rework the SPP core behind C1

- **State:** keep per-channel state instead of the `s_*` globals in `spp.c`. One channel is enough for now; the count can be a Kconfig option (`MAX_NR_RFCOMM_CHANNELS` is 1 today).
- **Data path:** a generic write path (TX buffer drained on CAN_SEND_NOW) and RX path (ring buffer).
- **No built-in traffic:** remove the built-in `BATTERY?` on open. What to send is the driver's business.
- **Pairing:** try the PIN list in order and retry on an auth failure. Optionally drop the link key and retry once (the PM220 forgets bonds when it resets).
- **Connection:** a connect timeout covering SDP, paging, pairing and the RFCOMM open; a proper disconnect (`rfcomm_disconnect`, then the ACL); and a reason code on every failure or close.
- **Ops:** `classic.connect` and `classic.state` keep working, built on the same core.

### C3 (Should): better inquiry results

- `cod` in `classic.state` devices, and in the PEX device struct.
- CoD and name-prefix filters (C1 signature).
- `SCAN_MAX` of at least 16 (offices are full of Classic devices).
- A remote name request for results that came without an EIR name.

### C4 (Must): count Classic in Wi-Fi coexistence

Inquiry, paging, SDP and connection setup should count as "radio busy", the same way LE scan and connect do now. For example, one `bt_radio_busy()` that `wifi.c` calls in place of `le_radio_busy()` (`wifi.c:502, 751, 912`). The pm220 firmware held the SoftAP while paging and inquiring (`wifi_bt_radio_hold`). Without that, joins dropped out.

**Done when:** a Wi-Fi rejoin during an inquiry waits, and STA plus SoftAP stay up through a PM220 connect and print.

### C5 (Must): a Classic build that fits

- **Option:** add a Classic-only configuration (`BT_CLASSIC=y`, `BT_CLASSIC_NAP=n`, `BT_PERIPHERAL=n`).
- **ACL size:** don't force the BNEP-sized `HCI_ACL_PAYLOAD_SIZE (1691+4)` on it. Derive it from NAP, or make it a Kconfig value; RFCOMM alone is fine with around 1021 or less.
- **Measure:** for each of these, report slot use, RAM, and the gap between `__end__` and `__HeapLimit` (it must be at least 8192):
  - `pico2w_defconfig` + Classic: our standalone base;
  - `pico2w_sd_defconfig` + Classic: our companion base.
- **TLS:** keep TLS on if it fits. If not, say what has to give.
- **RP2040:** one figure for a Pico W build with Classic, even if it's just "does not fit, by N bytes".

### C6 (Should, mostly a check): a Classic-only image is central-only

With NAP off, the board must not be discoverable or connectable, and must publish no SDP records. The code suggests this is already so (`nap_init()` is gated by NAP), but please confirm on the board. Also set the Classic local name from `CONFIG_NET_HOSTNAME`, because the printer's pairing list shows it.

### C7 (Should): debug ops and events

These are for the bench and our UI's scanner:
- `classic.disconnect`;
- events `classic.device` (an inquiry result) and `classic.state` (connected, closed, failed with reason);
- optionally `classic.send {"hex":"..."}` plus `classic.rx` events, for manual protocol poking.

### C8 (Should): BLE and Classic together

Check that one SPP channel works alongside at least one BLE central link (`BT_MAX_LINKS=4`, 3 shared ACL buffers): stream a job over SPP while a BLE device is connected. Write down the limits.

### C9 (Should): validation and docs

- Close R16 using its done-when (a PM220 `BATTERY?` round trip), plus the streamed 16 KB job from C1. Record it in `docs/Hardware-Testing.md`.
- Fix the stale `BT_CLASSIC` Kconfig help (it says ACL 1536) and R16's "is not compiled" / link-failure text.

---

## B. Other requirements

### R2 (Must for companion mode): serve a static web app from the SD card

Partly done: parent `a4a104c` ("Serve SD card files at /sd", on
`feature/pinecil-pex`) adds `GET /sd/<path>` as a read-only alias of
`/api/sd/files` and adds MIME types for `.js`/`.mjs`/`.css`/`.wasm`/`.ico`.
That route deliberately has no `.gz` preference, and a folder answers with
the JSON listing. Our app is stored gzipped, the same way as on flash
(`tools/deploy.py companion --target sd` puts it in `apps/printer/` on the card,
`index.html.gz` included), so it still doesn't load. Measured on the bench board
(10.255.255.23, 2026-10-26):

| Request | Today | Needed |
|---|---|---|
| `GET /sd/apps/printer/` | 200, JSON listing | `index.html.gz` as `text/html`, `Content-Encoding: gzip` |
| `GET /sd/apps/printer/app.js` | 404 (only `app.js.gz` exists) | `app.js.gz` as `text/javascript`, `Content-Encoding: gzip` |
| `GET /sd/apps/printer/index.html.gz` | 200 `application/octet-stream`, no encoding | unchanged is fine |
| `GET /sd/apps/printer` (no slash) | 200, JSON listing | a redirect to `/sd/apps/printer/`, or `index.html` (our page adds the slash itself) |

The same rules as the LittleFS static files:
- for `<path>`, try `<path>.gz` first, sent with `Content-Encoding: gzip` and the MIME type of `<path>`, then plain `<path>`;
- `<dir>/` serves `<dir>/index.html` (so `index.html.gz` first);
- the listing JSON stays on `/api/sd/files/<dir>`. If the alias has to keep the listing too, a folder holding `index.html(.gz)` should serve the page instead.

**Done when:** `http://<board>/sd/apps/printer/` loads our app with all modules from the card.

**Update 2026-10-27, after the `.gz`/index firmware was flashed:** gzip and
index serving now work (every file comes back with its type and
`Content-Encoding: gzip`). Two problems remain, both measured in Chromium
and with curl against 10.255.255.23:

1. **Concurrent GETs fail with 503 (blocker).** The SD worker runs at most
   two transfers (`xfer_max: 2` in `/api/sd`). A third request gets
   `503 {"error":"card busy"}` with `Retry-After: 1` at once. A browser loads an
   ES-module app's imports in parallel (ours fetches about 25 at once) and never
   retries a failed module, so the page stops loading: 9 of the app's
   modules got 503 on the first load. With 8 parallel curl fetches of one file,
   2 got 200 and 6 got 503. Needed: hold a `/sd/` GET until a slot frees
   (a queue as deep as the HTTP connection pool, or a few seconds' wait)
   instead of refusing it. LittleFS static serving copes with the same load.
2. **No redirect for a folder without the trailing slash (minor).**
   `GET /sd/apps/printer` serves `index.html` straight away, so relative URLs
   resolve against `/sd/apps/`: the browser asked for `/sd/apps/app.css`
   (404) before our page's script added the slash. Needed: `301` to
   `/sd/apps/printer/`, as most static servers do.


### R3 (Should): a launcher API for extensions

Superseded by R12, which needs the same hook (`app.addLauncher`) and covers apps that live on the card.

### R4 (Should): PEX op budget

16 ops for all modules together is tight once there are several device drivers. We use one hub module with 8 `printer.*` ops for exactly this reason.

- Raise the cap, or make it per module.
- **Nice:** asynchronous op replies (spec Q11), so `printer.print` can reply when the job ends.

### R5 (Must): autostart order or module dependencies

Our driver modules register with a hub module (`pex_find("printer_hub")` + `pex_sym`), so the hub must start first. Please either:
- guarantee that `cfg/pex_autostart.json` starts modules in list order, each `APP_RUN` returning before the next load; or
- add `DEPS` (spec §16).

### R6 (Should): product identity in Kconfig

The standalone image has to identify as the printer product:
- SoftAP SSID and PSK, USB product strings, the BLE/Classic local name and mDNS name (`NET_HOSTNAME` already covers hostname and mDNS) should all be Kconfig strings;
- `sys.info` should report a product name and version string so our UI can show it.

### R7 (Should): parameterised web tools

- `tools/make_release.py --src DIR --out DIR`.
- `tools/upload_fw_assets.py --sync` limited to a prefix, with `--exclude`. Today `--sync` deletes every file that isn't in the local dist, which wipes other projects' files (as already happened to `js/pi-switch/`).

### R8 (Nice for now): RP2040 Pico W

A validated Pico W (RP2040, 2 MB flash) build. That means a 2 MB partitions, linker and defconfig set, plus a decision on OTA: either a scheme that fits, or an explicit "no OTA on RP2040".

### R9 (Check): feature discovery

`sys.info` has `modules` (from `ops_modules_json`) and the transports list (`le`, `classic`, `bnep`). Please confirm that `modules` always shows the wg, wifi, eth, sd, ota and pex namespaces exactly when they're compiled in. Our UI hides the panels for missing features based on it.

### R10 (Nice): defconfig fragments

Let `APP_DEFCONFIG` take a list (base, then fragments). Until then our `tools/build.py` does the merge itself.

---

## Added from feature/nelko-pm220 (2026-10-26, against b397a84)

### C10 (Nice): keep going through the PIN list after the drop-key retry

`cl_sess.c` `arm_retry()` sets `pin_i = 0`. An open with `pins ["0000","1234"]` and `drop_key` therefore answers `0000` on both attempts and fails, unless the peer asks twice within one attempt; it never reaches `1234`. `drivers/test/test_fakehost.c` `t_classic` shows this on the real `cl_sess.c`.

The PM220 driver doesn't need a fix: it opens with `npin 0` and answers each `PEX_CL_PIN` itself. Other modules would hit the same thing, though. Either option would do:
- resume at the next PIN after the retry;
- document that the list restarts.

---

## C. Bugs found in testing (2026-10-26, against b397a84)

Found running the real btgw image in mcusim (`feature/printer-sims`, simulated PM220 / P21 / D30 / PL70e-BT) and on the bench board. Workarounds on our side are noted. Each one should be fixed in the parent.

### B1 (Must): Classic security level is judged before it is known, so every session at level ≥ 2 fails

What happens:
- **Legacy-PIN printers (PM220):** on `HCI_EVENT_ENCRYPTION_CHANGE`, `src/bt/classic/spp.c` calls `gap_security_level(acl)`. Once the PIN pairing is over (`cl_io_held() < 0`), it treats the answer as final.
- **SSP Just Works printers (P21, D30):** `HCI_EVENT_SIMPLE_PAIRING_COMPLETE` calls `reassess(..., final = 1)` before the link is encrypted.
- **Why the answer is wrong:** BTstack only knows the level after Read Encryption Key Size completes. Until then `gap_security_level_for_connection()` returns LEVEL_0 (`encryption_key_size` is 0).
- **Effect:** the session fails with "security level not met" right after a successful pairing. Real controllers send Encryption Change before the key size, so real printers hit this too. The bench PL70e's Classic attempt failed the same way.

Fix: treat only `GAP_EVENT_SECURITY_LEVEL`, or the key-size completion, as final. Don't treat Simple Pairing Complete or Encryption Change as final.

Workaround in btgw_printer: Classic drivers open at level 0. A printer config can set `"security_level"` once this is fixed.

**Done when:** a session at level 2 opens to a legacy-PIN peer and to an SSP Just Works peer, both in mcusim and on a real PM220.

### B2 (Should): Classic stays "busy" for good after a failed session plus an immediate SDP reconnect

What happens:
- After an auth failure, an immediate `classic.connect` that needs SDP (`uuid`, no channel) timed out with "connect deadline" without paging.
- After that, every connect needing SDP returned `busy` ("the radio cannot start that now"). Wi-Fi then logged "deferring the AP, Bluetooth has the radio".
- A connect to a fixed `channel` still worked.

Suspected cause: `s_sdp_h` is left pointing at the timed-out session. `begin_sdp` registered a query callback that never ran. Not confirmed.

**Done when:** a timed-out or failed SDP lookup always clears the SDP owner, and the next SDP connect pages normally.

### B3 (Check): the bench board's LittleFS refuses any write over about a byte

On the bench board (fw 1.277, 1200 KB LittleFS at 0x250000 on the 1 MiB slot map):
- PUT `/api/fs/...` and module writes fail with "write failed" / "fs write busy or failed" for anything bigger than about a byte.
- `sys.info` shows `fs.kb 1200`, `migration none`, and listing works.

This may be the board's state after the re-partition (stale or corrupt blocks) rather than code. It needs looking at on the board before a standalone deploy, which writes the whole app to flash. Module files went to the SD card meanwhile.

**Done when:** the cause is known; the fix is either a reformat/migration procedure or a code fix.

C10 above (the PIN list restarting after the drop-key retry) is a fourth finding from the same runs.

### R11 (Must): PEX module memory on an image that carries other modules (2026-10-27, bench board)

On the bench board (Anker and Pinecil installed, 48 KB pool) the printer
modules only fit with a reboot and a fixed install order. What we measured:

1. **`pex.install {path:"sd:/..."}` needs the whole file in one piece of the
   pool.** nelko_tspl is 14,924 bytes; the pool had 12,800 bytes in one piece,
   so the install was refused ("the file is 14924 bytes; the PEX pool has
   12800 in one piece"). The installed copy runs in place, so its code never
   needs the pool. Needed: stream an SD install into the partition in chunks
   (a few KB of buffer), so the pool size bounds a module's RW, not its file.
2. **The RW reservation of every installed module is taken at boot, loaded or
   not** (`rw_area` 26,080 B for Anker + Pinecil, 33,264 B with printer_hub).
   With printer_hub installed the pool had 12,800 bytes free with nothing of
   ours loaded, so a 14.9 KB driver could not be installed. Uninstalling did
   not give the reservation back until a reboot, and a later install then
   failed with "PEX pool full (index update)". The working order was:
   uninstall, reboot, install the driver, install the hub. Needed: reserve RW
   when a module loads (or give it back on uninstall), and say in
   `pex.list` how much the next install needs.
3. **A module installed after boot loads into RAM** (`xip_ready:false`) until
   the next reboot. That is documented, but together with 1–2 it means a
   driver update on a full board always takes two reboots.
4. Autostart needs `cfg/launch.json` on LittleFS, which B3 makes unwritable
   on this board. Allowing the launch list on the card (`sd:/cfg/launch.json`)
   would also help boards without working flash.

**Done when:** on the bench board, with Anker and Pinecil installed, a new
printer_hub plus one driver can be installed from the card and started
without a reboot, in either order.

**Re-measured 2026-10-27, after the B5 fix was on the device:** `pex.uninstall` now works on a full pool (B5 is fixed). Items 1 and 2 still stand:
- With printer_hub (RW 7,184) and nelko_tspl running, `rw_area` 33,264 B, pool `free` 12,200. Stop, unload and uninstall of nelko_tspl all succeeded; after that the pool had 12,504 free and `rw_area` was still 33,264 (item 2). The new 14,868 B nelko_tspl could not be installed from the card (item 1), so the board was left with no Classic driver.
- **One reboot was necessary because of this** (owner allows a reboot only when necessary, and each one points at a firmware bug): uninstall printer_hub, reboot (`rw_area` 26,080, pool 19,984), install nelko_tspl, install printer_hub, load and start both. Afterwards printer_hub runs from RAM (`xip:false`, item 3) and the pool has 2,504 B free, so a second driver can't load until the next boot.
- Odd: nelko_pl70e, installed after boot, reported `xip_ready:true` while `rw_area` stayed at 26,080 / 33,264 (its 1,284 B RW not counted). Worth checking that its RW address is really reserved.

### R12 (Must for companion mode): installable apps — files on the card, registered with the parent UI (2026-10-27)

**The problem.** In companion mode there is no supported way to reach our app from the BTGW UI, or to add an app to a board without replacing the parent's files:
- Extensions load only from flash `js/ext/<name>/index.js` (web/js/extensions.js). On the bench board flash refuses writes (B3), so our "Printer" button (`js/ext/printer-switch/`) cannot be installed. A board without working flash, or one whose flash is full, can't take an add-on at all.
- An extension can only add a window (`app.addWindow`). Our button reaches into `#toolbar` and inserts a `.tool-btn` before `#settings-btn`. That breaks whenever the toolbar changes.
- An add-on's PEX modules can't start themselves on boot without writing `cfg/launch.json` on flash (B3 again), and that file is shared by every add-on.

**Wanted: an app is a folder with a manifest; the parent finds it, lists it and launches it.** We, and any later add-on, install by copying files. Nothing in the parent's tree or its flash files changes.

1. **Discovery.** At startup the UI (and `sys.info` or a new `apps.list` op) reads `sd:/apps/<id>/app.json` for every folder in `sd:/apps/`, and `apps/<id>/app.json` on LittleFS for boards without a card. No central registry file to edit: a folder is an app, as `js/ext/` already works.
2. **Manifest** (`app.json`, small, extra keys ignored):
   ```json
   {"id":"printer","name":"Label printer","version":"0.2.0",
    "description":"Design and print labels on Bluetooth label printers",
    "entry":"index.html","icon":"icon.svg",
    "ext":"ext/index.js",
    "pex":[{"name":"printer_hub","file":"pex/printer_hub.m33.pex"},
           {"name":"nelko_tspl","file":"pex/nelko_tspl.m33.pex"}],
    "autostart":true}
   ```
   - `id` matches the folder name (`[a-z0-9_-]+`).
   - `entry` is served at `/sd/apps/<id>/<entry>` (the R2 route). The launcher opens `/sd/apps/<id>/`.
   - `icon` (optional) is a small SVG or PNG in the folder.
   - `ext` (optional) is an extension module loaded from the app's folder, exactly like `js/ext/<name>/index.js` today, but from the card: `register(app)` with the same `app` object.
   - `pex` (optional) lists the app's modules in start order. `autostart` true means: at boot, install-or-load and start them in that order (that is R5, without `cfg/launch.json`). A module already installed with the same build id is just loaded. Failures go to the log and `pex.faults`, not a stopped boot.
3. **Launcher.** The parent UI shows every app as a toolbar or menu entry (name and icon) that opens its `entry`, plus `app.addLauncher({id,label,href|onClick,icon})` for extensions (the old R3). A window that lists the apps (name, version, description, whether its modules are running, Open, Remove) would be nice; Remove deletes the folder after stopping its modules.
4. **The way back.** The app needs to know where the parent UI is: `/` today. If that can change, put it in `sys.info` (`"ui":"/"`).
5. **Order and safety.** Apps load after the parent's own windows. An app that fails to load is skipped with a log line, as extensions are today. An app can't replace the parent's own files: `entry` and `ext` resolve inside its own folder.

**What we do on our side** (already, or once this lands): `tools/deploy.py --target sd` writes `sd:/apps/printer/` with `app.json`, `icon.svg`, the app and `ext/index.js` (the switch button, which becomes an `addLauncher` call), with the modules under `sd:/apps/printer/pex/`. Our app shows its "BTGW" button only when the parent says it is there (companion).

**Done when:** on the bench board, with no file of ours on flash, copying `sd:/apps/printer/` to the card and reloading the BTGW UI shows a "Label printer" launcher that opens `/sd/apps/printer/`; after a reboot printer_hub and nelko_tspl are running in manifest order; removing the folder removes the launcher.

**Status 2026-10-27 (parent dev 719b357, `ae55c4c` "Add a toolbar button for a web app that ships its own folder"):**
- Done: discovery of `apps/<id>/app.json` (or `.gz`) on flash and on the card; its `"label"` is a toolbar button that opens `/sd/apps/<id>/` (the folder is the link; a URL in the file is ignored); `app.addLauncher({id,label,href|onClick})` for code in the page. Seen on the bench board: our `sd:/apps/printer/app.json` gives a "Printer" button to `/sd/apps/printer/`, with nothing of ours on flash.
- Not part of it (still open, lower priority): `ext` loaded from the app's folder; `pex` start order and autostart from the manifest (R5; on this board `cfg/launch.json` can't be written, B3); an apps window with versions and Remove. We no longer need an extension for the button.

### R14 (Should): model B (`pic`) from the browser compiler, and load addresses in `pex.list` (2026-10-28)

**Why.** Under model A a module installed after boot runs from a RAM copy until the next reboot (R11 item 3): its flash copy holds absolute RW addresses, and the RW area is reserved only at mount (`pex/runtime/pex_xip.c:485,797`; pex-format.md "Code models"). On the bench board that RAM copy doesn't fit, so every driver update costs a reboot. Model B code reaches RW through r9 and code/ro PC-relative, so the flash copy runs in place right after install and takes pool SRAM only while loaded. We are moving our host-built modules to `MODEL pic` (clang, via the SDK's `pex_add_module`; branch `feature/pex-model-b` here). The editor's compiler should be able to do the same, or modules built in the browser keep the reboot problem.

**What pexcc does today** (checked 2026-10-28 against `~/Work/pexcc`, the source of `web/.pex-cc/pexcc.wasm` via `web/pex-cc/make_pexcc.sh`):
- Emits model A only: a global, a string or a function address is a literal word recorded as an absolute relocation (`src/cc.c` ~4985, `out->abs`), and each import is called through a model-A veneer (`ldr.w ip, [pc, #4]; ldr.w pc, [ip]`). `src/elf_out.c` writes an ELF for `pexlink.py link --model reloc`.
- Never allocates r9: code uses r0–r7 and r12 (frames are `push {r7, lr}` / `push {r4–r7, lr}`), so r9 is free to be the static base.
- It does compile our real drivers in model A: nelko_pl70e (4 files) compiles and links, 20,020 B against 11,540 B from gcc (not run on a board).

**Needed** (a `--model pic` switch or equivalent; model A stays the default until the owner decides):
1. **RW through r9.** A global or static's address is `r9 + offset` (offset known to pexcc, it lays out RW itself), not an absolute word. No `MOVW/MOVT` absolutes.
2. **Code and ro PC-relative.** A string literal, a `const` object or a function address is formed PC-relative (`adr`/`add rX, pc` + constant), so text+ro needs no patching: pexlink's `RO` reloc list must be empty for a `PIC` file.
3. **Imports through an r9-relative GOT in RW.** Veneer as in pex-format.md: `ldr r12, =slot(sbrel); add r12, r9; ldr pc, [r12]`; the GOT slots (`.pex.got.<name>`) at the start of RW. Soft-float helpers appended from libgcc must be built ROPI-safe too (or taken from a libgcc built with `-fropi -frwpi`).
4. **Pointer tables.** A `const` table of pointers can't live in ro: place it in RW and list each word in the `RW` relocs (what `PEX_PTR_TABLE` does for clang). Pointers in initialised RW (`int *p = &x;`, `fn = handler`) likewise go in the `RW` relocs. Our case: `static const pl70e_pipe_t pl70e_pipes[4] = {{"18f0", ...}}`.
5. **ELF/pexlink.** The output passes `pexlink.py link --target m33 --model pic` with the header `PIC` and `XIP_OK` flags; `R_ARM_SBREL32` (or whatever pexlink resolves) for RW references.
6. **r9 rule.** Every entry into the module goes through the host trampoline already (pex_main, exports, registered callbacks); pexcc must never clobber r9 and must not take the address of a module function in a way that bypasses the trampoline when the host calls it.
7. **Tests.** pexcc's own `make test` gains model-B cases (globals, strings, a pointer table, an import, a double helper), run under the Thumb interpreter with r9 set to a RW base that differs from the link base.

**Nice, same area:** `pex.list` reports, per loaded instance, the **code base and RW base addresses** (and `pex.installed` the flash address, not only the partition offset). A debugger or a probe script then maps globals from the module's ELF with one lookup per load, for both models. Today a module has to log `&some_static` itself.

**Found building our modules as model B with clang** (feature/pex-model-b, docs/notes/pex-model-b.md; host build, mcusim only):
- **The SDK's model B has no C library headers.** `pex_add_module(... MODEL pic)` calls clang with `--target=thumbv*-none-eabi` and no sysroot, so `#include <string.h>` fails; model A gets newlib's headers from gcc. We add `-isystem $(arm-none-eabi-gcc -print-sysroot)/include` in `prn_add_module()`. The SDK could do the same for every `pic` module (it already asks `PEX_GCC` for libgcc), or ship a small `string.h` over the host API's `memcpy`/`strlen`/....
- **The addresses are already there:** `pex.info` (`mod_json(..., full=true)` in `pex_load.c`) has `code_addr` and `rw_addr`; `pex.list` uses `full=false` and leaves them out. Moving those two fields into the short form is the whole change for the `pex.list` item above. Our modules log both once at start meanwhile (`PRN_LOG_BASES()`).

**Done when:** in the browser editor, a printer driver (e.g. nelko_pl70e's four sources) builds as model B, installs from the card on a running board with printer_hub loaded, reports `xip:true` without a reboot, and prints; and `pex.list` shows each instance's code and RW base.

### B4 (Must): paging a Classic device that doesn't answer stalls all Wi-Fi for ~35 s (2026-10-27, bench board, parent dev 719b357)

While a `pex_bt_classic_open` (via our `printer.connect`) pages a PM220 that is switched off, the gateway drops off the network. Measured from the PC, one request per second, during one connect attempt:
- idle: every request 0.20–0.24 s, ping up;
- during the attempt: flash static `/`, a card file `/sd/apps/printer/mode.json` and ping were all lost for about 35 s (6 s curl timeouts in a row, ping lost), then everything was back at 0.2 s;
- an earlier run with 12 s timeouts lost 7 requests in a row (~85 s) across the attempt and the driver's retry.
- `/api/sd` afterwards: `xfers 0`, last failure `499 client gone`. The card was fine; the requests never got through Wi-Fi.

Effect: a web app that auto-connects to a printer that is off makes the whole board unreachable, its own page load included (we saw module requests hang mid-boot). Other clients and WebSockets on the board stall too.

Needed: Classic paging (and inquiry) must leave Wi-Fi airtime, the same coexistence C4 asked for: a page window and a page timeout that interleave with Wi-Fi, never one long radio hold. A shorter page timeout for `pex_cl_open_t` (a field, e.g. `page_ms`) would let a driver probe cheaply.

Our side meanwhile: nelko_tspl stops retrying after two attempts in a row that find no printer (`UNREACHABLE_MAX`); a connect op starts over.

**Done when:** during a connect attempt to a switched-off Classic printer, the PC's 1-per-second requests to the board keep answering within 1 s.

### B5 (Must): removing a PEX module needs free PEX pool memory (2026-10-27)

`pex.uninstall` failed with `PEX pool full (index update)` on the bench board until a reboot: every partition change builds the new index in a 3 KB buffer borrowed from the pool's scratch gap. Details, source references and suggested fixes: [parent-issues/B5-pex-index-update-needs-pool.md](parent-issues/B5-pex-index-update-needs-pool.md).

### B6 (Should): the module BLE event ring drops events silently (2026-10-27)

`src/pex/pex_gw_bt.c`: BLE events for PEX modules (write completions, notifications, link changes) go into an 8-entry ring (`QDEPTH 8`) shared by every listener, filled from BTstack context and drained by `pex_gw_bt_poll()`. When it is full, `le_pex_event()` drops the **oldest** event (`s_qtail++; s_dropped++`). No module is told: `s_dropped` isn't exposed, and nothing marks the gap.

What we saw on the bench board (parent dev with the B5 fix): a 3x1 test print to the PL70e-BT (15,719 bytes, write-with-response, at most 3 writes in flight) stopped at `sent == total` and the driver stayed "printing"; the hub gave up after 120 s. The firmware itself reports every write's outcome within its 5 s op timeout (`op_pump` → `write_done(..., LE_WRITE_TIMEOUT)`), so a write-done that never reached the module is most likely a dropped ring entry. Not proven: there's no counter to read.

A lost write-done or link-down is worse than a late one: a driver that counts writes in flight waits forever. A lost notification loses printer status.

Needed:
- never drop a write-done or link event silently: make the ring bigger, or drop notifications before completions, or coalesce;
- when anything is dropped, deliver an event that says so (e.g. `PEX_BT_OVERFLOW` with the count) to each listener, and count drops in `pex.list` / `sys.info`;
- maybe a per-module ring, so one busy module can't push out another's events.

Our side: nelko_pl70e treats writes still in flight with no write event for 6 s as done (`WRITE_LOST_MS`), and reports `inflight` and `writes_lost` in its status.

**Done when:** a module can tell that an event was dropped, and a burst of notifications can't push out a write completion.

**Seen on hardware (2026-10-27):** after the driver guard went in, the next 3x1 job to the PL70e-BT printed and the driver reported `"writes_lost":1`: one write completion never reached the module during a 15.6 KB job.

### R13 (Check): a dual-mode printer over LE, and module size for the P21 driver (2026-10-27)

The P21 driver `nelko_ble` (feature/p21-ble) talks to the printer over LE GATT `ff00`. The P21
is dual mode: it also pages over Classic, and its LE address is its Classic one
(merlinschumacher/nelko-p21-print). That author found BlueZ (bleak) would not list it as an
LE peripheral. Nothing of this is tried on our gateway yet (the P21 arrives 2026-10-28).

To check on the parent (no change asked for yet):
1. **The LE scan table lists a dual-mode advertiser.** `pex_bt_devices` must return a device
   whose advertising flags don't say "BR/EDR not supported", and `pex_bt_connect` must make an
   LE link to it, also when the Classic side knows the same address (inquiry result, bond).
2. **ATT MTU.** Does the central start an MTU exchange? The P21's server MTU is 23 per that
   source; the Android app asks 247. `nelko_ble` writes `pex_bt_mtu() - 3` bytes, so whatever
   the gateway agrees is used.
3. **Module size (R11 item 1 again).** `nelko_ble.m33.pex` is 14,844 bytes (text 9,672, ro
   2,412; nelko_tspl went from 15,032 to 14,376 bytes without its P21 row). The largest free
   pool block on the bench board was about 9.7 KB, so it can't be installed from the card
   until an SD install streams into the partition (R11 item 1), or after the reboot-and-order
   workaround we no longer use.

**Done when:** the bench P21 is found by `printer.scan` with `nelko_ble`, connects, and its
`printer.status` shows an `mtu`; and nelko_ble installs from `sd:/apps/printer/pex/` without a
reboot.
