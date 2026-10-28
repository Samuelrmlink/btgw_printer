# Nelko P21 protocol

Status: researched from public sources only. **Not yet tried against a printer** (the P21 is
due on 2026-10-28). The confidence column says how sure each fact is. Sources are listed in
`docs/references.md`; facts marked [M] come from merlinschumacher/nelko-p21-print (AGPL, read
for facts only, no code taken), [J] from julienvincent/nelko-p21-rs (MIT), [T] from
TylerCode/Fyne-P21-Print (MIT), [A] from the Nelko Android app (facts only), and [P] from our own
PM220 work (pm220_rp2_webapp / nelko-pm220).

**Summary:** the P21 is the same printer firmware family as the PM220. It uses the same TSPL
subset, the same `ESC ! ?` / `ESC ! o` status bytes and the same `BATTERY?` / `CONFIG?` queries.
The differences are a 96-dot head, a 14 mm wide label, and the link: we drive it over BLE
(GATT `ff00`, the `nelko_ble` driver), though it talks SPP too.

## Transport

| Fact | Confidence | Source |
|---|---|---|
| Advertises and pages as name `P21` (P22 same firmware) | high | [M] [A: `DevicesType.P21`] |
| Bluetooth Classic SPP, RFCOMM **channel 1**, one SPP link at a time | high | [M] [J] [T] |
| Pairing needs no PIN (SSP just-works) | medium (one source) | [M] |
| The same command stream is also served over BLE GATT service `ff00`: write `ff02` (write without response), answers notify on `ff01`, credits notify on `ff03`; server MTU 23 | high | [M]; the app's Jieli service set is `1800, ff00, ae3a, ae00` [A] |
| LE advertising only while no Classic link is up | medium | [M] |
| The current Android app scans BLE for `P21` / `P31S` and uses MTU 247 (23 on some Android 14 boards) | medium | [A] |
| SoC: Jieli AC6951C (BR23); NFC reader YC5018 for the roll tag | high | [M] |
| Dual mode with **one address**: the LE address is the Classic one | medium (one source) | [M] |
| The Android app's default scan is Classic SPP for every model; BLE is a toggle (names `P21…`/`P31S…`, not ending `_BLE`/`-LE`/`_LE`) and its advice after a failed SPP connect on the "JieLi 9PF" variant | high | [A] (nelko-states notes §4.1, §7) |

**Our choice (2026-10-27):** the P21 is driven over **BLE**, by the `p21` model of the
`nelko_ble` driver (`drivers/vendors/nelko/ble/`). The owner's P21 uses BLE, and BLE has no
pairing, no PIN and no paging: connecting to a switched-off BLE printer doesn't stall the
gateway's Wi-Fi the way a Classic page does (parent B4). The command bytes are the same as
over SPP.

There is no Classic P21 row any more. The P21 does talk SPP too ([M] [J] [T] printed over
RFCOMM channel 1, and the app defaults to it), but it is one printer at one address: a
Classic row in `nelko_tspl` would have a second driver claim the same P21 by its inquiry
name, and two P21 models would sit in the app's lists. If a P21 turns up that won't do BLE,
the row comes back from git (`nelko_tspl.c` before 8677c42), under its own model id.

## BLE

| Fact | Confidence | Source |
|---|---|---|
| Service `ff00`: `ff02` write without response (commands in), `ff01` notify (replies out), `ff03` notify (flow control), `ff04` write without response (same as `ff02`), `ff05` indicate, `ff10` read/write; GAP `1800` | high | [M] (GATT dump); `ff02`/`ff01` also [yowhatscookingkai/NelkoP21LabelMaker]; the app binds write `ff02`, read `ff01`, notify `ff03` [A] |
| Server MTU 23: 20 bytes per write | high | [M] |
| A job in 20-byte writes about 12 ms apart prints | high | [M] |
| `ff03` notifications once enabled: `01 NN` = NN more packets may be sent (starts at 7), `02 LL LL` = packet size, little endian (127) | medium | [M]; the app doesn't use them [A] |
| Replies on `ff01` are byte for byte the SPP replies (text ends CRLF, status bytes don't) | high | [M] |
| The app asks MTU 247 for the P21/P22 and writes 244-byte packets 5 ms apart; MTU 23 with 20-byte packets 1 ms apart on a "BaiRui" module under Android 14 | high | [A] |
| `AT+DISCOVERABLE=0\r` arrives as a notification when the printer powers off | high | [A] ("P21 Shut Down") |
| Some host stacks (bleak on BlueZ) don't list it as an LE peripheral, since it is also a Classic device | medium | [M] |

## Framing

- **Text commands:** ASCII, end with CRLF. The printer acts on `\n`. Replies, where there are
  any, end with CRLF. [M] [J]
- **Status commands:** raw `ESC` sequences. The reply is raw bytes with no CRLF. **Over BLE the
  bench P21 (fw 4.2.14) answers `ESC ! ?` only when CRLF follows**; a bare `ESC ! ?` is never
  answered (2026-10-28, [bench]). nelko_ble sends `ESC ! ?` + CRLF.
  [M] Note: [J] sends `ESC ! o` followed by CRLF and still gets the 16-byte block, so a trailing
  CRLF is tolerated. high
- **Pacing:** send one query at a time and wait for its answer; pipelining queries is unreliable.
  A print job is one stream with no replies. [M] medium

## Commands

| Command | Reply | Notes |
|---|---|---|
| `SIZE 14.0 mm,40.0 mm` | – | label size |
| `GAP 5.0 mm,0 mm` | – | 5 mm gap on the stock roll [J] [T]; the roll tag reports its own gap in byte 8 (format ≥2) |
| `DIRECTION d,m` | – | no visible effect seen on the P21 [M]; [J] sends `1,1`, [T] sends `0,0` |
| `DENSITY n` | – | 1..15 [M] [J]; [T] allows 0..15. The app default is unknown; [J] defaults to 15 |
| `CLS` | – | clear the image buffer |
| `BITMAP x,y,wbytes,h,1,<data>` | – | `wbytes = 12`; mode 1 |
| `PRINT n` | none | prints n copies |
| `\x1b!o` (a CRLF may follow) | 16 bytes | label block (below) |
| `\x1b!?` | 1 byte | status byte |
| `CONFIG?` | `CONFIG ` + 10 bytes + CRLF | |
| `BATTERY?` | `BATTERY ` + 2 bytes + CRLF | |
| `BEEP <raw byte 0/1>` | – | the value is a raw byte, not an ASCII digit |
| `TIMER <raw byte 0..3>` / `TIMEOUT <raw byte>` | – | auto power-off: never/15/30/60 min. [M] names it `TIMER`, [J] `TIMEOUT`; unresolved |
| `SELFTEST` | – | prints a self-test page |
| `INITIALPRINTER` | – | factory reset. **The driver never sends this.** |

A job, as both references send it:

```
\x1b!o\r\n                       (status first; the reply is read and parsed)
SIZE 14.0 mm,40.0 mm\r\n
GAP 5.0 mm,0 mm\r\n
DIRECTION 1,1\r\n
DENSITY <n>\r\n
CLS\r\n
BITMAP 0,0,12,<rows>,1,<12*rows bytes>\r\n
PRINT <copies>\r\n
```

[J] reads a 16-byte status after the job. **The same shape as the PM220 job**
(`tspl_build_job` in pm220_rp2_webapp), except for the PM220's `SIZE 50 mm,30 mm`, `GAP 2 mm` and
48-byte rows. high

## Image

- 1 bit per dot, MSB first, rows padded to bytes. **A 0 bit prints black**, 1 is white
  (`black_bit = 0`, the same as the PM220). high [M] [J]
- **Head: 96 dots** (12 bytes) at 203 dpi = 12 mm. A wider bitmap is clipped to the head. high [M]
  [J] [T]
- The bitmap's long axis is the label's length (along feed), and the 96 dots run across the
  14 mm tape. Text that reads along the label is therefore rotated 90° in the bitmap. The web app
  does this; the driver doesn't rotate.
- **Rows:** a 40 mm label is printable end to end as 320 rows; the app uses 284 rows (35.5 mm) to
  be safe. medium [M] (confirmed by a full-border print there) / [J] [T] use 284

| Label (w × l mm) | Rows the app uses | Full length | Source |
|---|---|---|---|
| 12 × 40 | 284 | 320 | [T] |
| 14 × 40 (stock roll, the user's) | 284 | 320 | [T] [J] [M] |
| 14 × 50 | 355 | 400 | [T] |
| 14 × 75 | 532 | 600 | [T] |
| 15 × 30 | 213 | 240 | [T] |

Default for presets: rows = round((len − 4.5 mm) × 203 / 25.4) for the app's margin, with the
full length offered as a calibration option. Byte count = 12 × rows (3408 for 284 rows).

## Replies

**Status byte** (`ESC ! ?`, and byte 0 of the label block): 0 ready, 1 lid open, 2 feeding,
3 paper error, 4 no paper, 0x10 paused, 0x20 busy, 0x24 out of paper, 0x30 low battery,
0x80 overheated, 0x90 no power. [M] (0 and 4 seen on hardware). The PM220 values 0/1/4/32 agree.

**Label block** (`ESC ! o`, 16 bytes):

| Byte | Meaning |
|---|---|
| 0 | status byte |
| 1 | tag data length, 0x0c |
| 2–13 | roll "paper code" (all zero when there's no readable tag) |
| 4 | colour: 2 transparent, 3 white, 4 pink, 5 blue, 6 yellow |
| 7 | paper type: 0 continuous, 1 gapped, 2 black mark |
| 8 | gap mm (tag format ≥ 2) |
| 11 | label length mm |
| 12 | usable width mm |
| 13 | label width mm |
| 14–15 | CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF) over bytes 0–13, **big-endian** |

Test vectors: [J] `200c011203000301121215280f0e` → CRC `0d 22`, which is busy, 14 × 40 mm;
[M] `000c011203000301121215280f0e` → `ed 03`. high

The printer doesn't require a tag: covered tags still print. The "DRM" is app-side only. [M]

**CONFIG payload** (10 bytes): DPI u16 BE (`00 cb` = 203), hardware x.y.z, firmware x.y.z
(`04 02 04`), timer byte, beep byte. A 20-byte variant also exists. [M] [J]

**BATTERY payload**: BCD percent (`0x99` = 99 %, always 99 while charging), charging flag. [M] [J]

## Bench results (2026-10-28, gateway fw 1.281.0, P21 fw 4.2.14 hw 0.2.2)

Raw GATT writes from the gateway, then through printer_hub + nelko_ble (model B, installed
after boot, run in place):

- **Found by LE scan** as `P21` at `8C:D0:0D:AB:11:9A`, advertising service `af30` (not `ff00`;
  `ff00` is in the GATT table). Visible only with no phone holding it. Note: `scan.list` is
  paged (`offset`, `next`); a tool that reads only the first page misses it in a busy room.
- **GATT:** `1800`, `ff00` (`ff02` write w/o response, `ff01` notify, `ff04` w/o response,
  `ff03` notify, `ff05` indicate, `ff10` read/write), `ae3a`, `ae00`. **MTU 255.**
- **`ff03` sends no credits**, before or during a job: the driver's gap alone paces it.
- `CONFIG?` → `CONFIG 00 cb 00 02 02 04 02 0e 02 01` (0x00cb = 203 dpi). `ESC ! o` → 16 bytes,
  CRC ok, 14×40 gap media from the tag. Battery 75 %.
- **A 3,514 B job** (golden `golden_p21_14x40_d8.bin`) in 244 B writes: 2.2 s, no failed write,
  one clean label, image centred on the 14 mm tape.
- **Status while printing:** `0x20` (busy) for about 4 s after the last byte of a 14×40 label,
  then `00`. `ESC ! ?` + CRLF every 0.7 s during and after the print does **not** reprint; a lone
  CRLF after the print does not either; `GAP 2.0 mm` on 5 mm media prints one label too.
- **Open: one job through printer_hub + nelko_ble printed 14 identical labels** (a 15th when the
  lid was opened), with `PRINT 1` and copies 1 all the way (`bench_print.py`, 96×284, density 8,
  `GAP 2.0 mm`). The driver saw `0x20` for its whole 30 s wait. Every raw job since, including
  that gap and that polling, printed once, and mcusim's P21 prints once with the same modules.
  The difference is still unknown: next, capture the driver's exact bytes (host harness with
  bench_print's parameters) and diff them against the golden; the pacing (3 writes in flight,
  10 ms apart, vs one at a time) and the readiness query right before the job are the other
  differences.
- nelko_ble disconnects a raw `dev.connect` link to a printer it knows (connect, then
  `dev.disconnected` at once); stop the module for raw tests.

## Open questions for the bench (P21 arriving)

Answered above: 1 (yes, `P21`), 2 (255), 3 (no credits). Still open: 4–8.


1. Does the gateway's LE scan see the P21 at all (it is dual mode; [M] saw BlueZ hide it), and
   under which name (`P21`, `P21_xxxx`, `P21 Lite`)? Is the LE address the Classic one?
2. Which MTU does the gateway get (`"mtu"` in `printer.status`): 23 as [M] says, or more?
3. Does `ff03` send `01 NN` / `02 LL LL` to us, and do the credits keep coming during a job
   (`"credits"` in the status, `"credit_waits"` if they stop)? Is NN additive or a fresh
   allowance (we take it as fresh: the safe reading)?
4. Is a 10 ms gap between writes enough at the negotiated MTU, or does a long label lose bytes
   (`"writes_failed"`, a label cut short)? `gap_ms` in the printer config changes it.
5. Does polling `ESC !?` every 10 s keep the printer from its auto power-off? Does it send
   `AT+DISCOVERABLE=0` when it does switch off?
6. Which of `TIMER` and `TIMEOUT` is right? (We don't send either by default.)
7. Is the full-length 320 rows clean on the user's roll? (Calibration preset: 284 vs 320.)
8. What is the DENSITY default and its useful range on this unit (the app: 15 with a tag, 4
   without)?

## As implemented (`nelko_ble`, model `p21`)

`drivers/vendors/nelko/ble/nelko_ble.c`; the reply parsing (`nelko_proto.c`) and the job text
(`proto/tspl`) are nelko_tspl's. Tests: `drivers/test/test_p21.c` (fake-host BLE peer, and a
job through the real printer_hub), `tools/e2e_sim.py p21` (mcusim's BLE P21: 0 dots differ,
2026-10-27).

- **Find:** an LE advertisement whose name starts with `P21` (as the app's BLE scan). The
  PL70e-BT (`PL70e-BT_xxxx`, `…-LE`) and names with `P21` further in are never claimed.
  `printer.found` once per scan, `"transport":"ble"`.
- **Connect:** `pex_bt_connect` (a scan of 5 s first when the address isn't in the scan
  table), then `ff00`/`ff02` and `ff00`/`ff01` must be there, `ff03` is used when it is.
  Subscribes to `ff01` and `ff03`. Writes without response when `ff02` has that property
  (`"write_response":true` in the config forces with-response). Reconnects with a back-off
  (8 s doubling to 60 s) unless `auto_reconnect` is false or the user disconnected.
- **Writes:** min(MTU − 3, 244, the size `ff03` announced) bytes, at least `gap_ms` (10 ms,
  config `"gap_ms"` 0..200) apart, at most 3 in flight, and no more packets than the last
  `01 NN` allowed; with none for 500 ms it sends anyway (`"credit_waits"`). Every write asks
  for its done event; `ev->status != 0` means the bytes are lost.
- **Queries**, one at a time, 1.8 s for an answer: after connecting `CONFIG?`, `BATTERY?`,
  `ESC !o\r\n`; then `ESC !?` (no CRLF) every 10 s and `BATTERY?` every 30 s while idle. A
  lone status byte nobody asked for is taken as a status (the low-battery push). Replies on
  `ff03` are never read as data.
- **Status:** `printer` (the code's name), `lid`, `paper`, `busy`, `battery`, `charging`,
  `media` (from the roll tag), `dpi`, `fw`, `hw`, `mtu`, `credits`; and once they count,
  `writes_failed`, `writes_lost`, `credit_waits`, `unanswered` + `quiet_ms`.
- **Before a job:** an `ESC !?` asked by JOB_BEGIN itself (an answer from the idle poll is
  not enough): until it answers, `PEX_EAGAIN` (the hub calls again for 5 s). 00 and 30 print;
  20 (busy) and 02 (feeding) ask again; any other code is `PRN_DRV_ENOTREADY` and the status
  says which.
- **The job:** nelko-p21-rs's text byte for byte (`t_job_golden`, at MTU 23 and 247):
  `ESC !o\r\n`, `SIZE`/`GAP` in mm with one decimal (`GAP 5.0 mm` when the media gives none),
  `DIRECTION 1,1`, `DENSITY` (clamped to 1..15), `CLS`, `BITMAP 0,0,12,<rows>,1,`, the rows,
  `\r\nPRINT 1\r\n`. The bitmap is 96 dots wide (`placement` false).
- **Copies:** the hub feeds the rows once per copy; each copy is a whole job with `PRINT 1`.
  The next copy's head waits until the copy before has gone out whole, so the ring never
  holds two bitmaps (see lost bytes).
- **After the job:** `ESC !?` every 0.7 s. 00/30 is done; an error code fails the job with
  its reason ("out of paper"); busy keeps asking. No answer at all in 15 s fails the job
  ("the printer isn't answering (job sent; it may not have printed)"); answers that never say
  ready count as done after 30 s ("sent; printer did not confirm").
- **Lost bytes** (a failed write): the job fails, and the lost bytes plus the rows not yet sent
  go out as white (0xFF), then a CRLF, so the printer leaves the BITMAP and prints nothing; the
  next job's `CLS` clears it. A cancel does the same for the rows not yet fed. A write-done
  event that never comes (parent B6) is given up on after 6 s (`writes_lost`).
- **Power off:** `AT+DISCOVERABLE=0` on `ff01` drops the link ("printer switched off").

## Bench plan (P21)

The board is 10.255.255.23. Our modules live in `sd:/apps/printer/pex/` (the old
`sd:/apps/printer-pex/` folder is gone, docs/notes/integration.md). No reboots: if an install
fails for pool memory, stop and write it down (R11), don't reboot around it.

1. **Build and copy.** `tools/build.py drivers`, then
   `tools/deploy.py companion --target sd --base http://10.255.255.23` (copies the app and
   `nelko_ble.m33.pex` to the card; `--dry-run` first to see the list).
2. **Room for it.** `tools/btgw_op.py pex.list` and look at the pool's largest free block.
   `nelko_ble.m33.pex` is 14,844 bytes and `pex.install` from the card needs it in one piece
   (R11 item 1). If nelko_tspl or nelko_pl70e is loaded with no printer on it:
   `tools/btgw_op.py pex.stop '{"name":"nelko_tspl"}'` and
   `tools/btgw_op.py pex.unload '{"name":"nelko_tspl"}'`.
3. **Install and start.**
   `tools/btgw_op.py pex.install '{"path":"sd:/apps/printer/pex/nelko_ble.m33.pex"}'`,
   `tools/btgw_op.py pex.load '{"name":"nelko_ble"}'`,
   `tools/btgw_op.py pex.start '{"name":"nelko_ble"}'`
   (printer_hub must be running first: `tools/btgw_op.py printer.drivers` lists `nelko_ble`).
4. **Scan.** P21 on, not connected to a phone (a Classic link stops its LE advertising):
   `tools/btgw_op.py printer.scan '{"ms":8000,"driver":"nelko_ble"}' --listen 9 --evt printer.found`.
   Expect `"name":"P21…","model":"p21","transport":"ble"`. Nothing: try
   `tools/btgw_op.py scan.start '{}' --listen 10 --evt scan.device` and check whether the
   gateway sees it at all (question 1).
5. **Connect and status.**
   `tools/btgw_op.py printer.connect '{"driver":"nelko_ble","addr":"<addr>","config":{"model":"p21"}}' --listen 10 --addr <addr>`,
   then `tools/btgw_op.py printer.status '{"driver":"nelko_ble"}'`. Expect `"state":"connected"`,
   `"printer":"ready"`, `battery`, `fw`, `media` 14 × 40 with `crc_ok`, and `mtu`. Note `mtu`,
   `credits`. Open the lid / take the roll out and ask again: `lid_open` / `no_paper`.
6. **First label, from a script.**
   `tools/bench_print.py --driver nelko_ble --model p21 --addr <addr> --media 14x40 --dots 96 --rows 284 --density 8 --path sd/printer/var/job.bin`
   (the job file goes on the card: LittleFS writes fail on this board, B3). Expect
   `printer.job` queued → sending → printing → done, and a full label. Then the status again:
   no `writes_failed`, no `credit_waits`.
7. **First label, from the app.** `/sd/apps/printer/`, Printers → Scan for "Nelko P21", add it,
   label size `nelko-p21-14x40`, a text label, Print. Then the calibration test page.
8. **Stress the link.** A 14 × 75 label (532 rows), 3 copies. Then `"gap_ms":0` in the
   printer's config (a quicker job; does it still print whole?).
9. **Error paths.** Print with the lid open (refused: "printer not ready"), take the roll out
   mid-job (failed "out of paper"), switch the printer off while connected (state `why`
   "printer switched off" or "link lost", then it reconnects once on).

Write the answers to the open questions above and the numbers (MTU, credits, time per label)
in this file and in docs/notes/integration.md.
