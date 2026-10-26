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

### R3 (Should): a launcher API for extensions

Add `app.addLauncher({id, label, href | onClick})`, which puts a button in the toolbar or main menu. It is for `js/ext/<name>/index.js` extensions. Today the only hook is `app.addWindow`, so our button has to live inside a window.

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
