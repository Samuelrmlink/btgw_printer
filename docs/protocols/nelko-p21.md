# Nelko P21 protocol

Status: researched from public sources only. **Not yet tried against a printer** (the P21 is
due on 2026-10-28). The confidence column says how sure each fact is. Sources are listed in
`docs/references.md`; facts marked [M] come from merlinschumacher/nelko-p21-print (AGPL, read
for facts only, no code taken), [J] from julienvincent/nelko-p21-rs (MIT), [T] from
TylerCode/Fyne-P21-Print (MIT), [A] from the Nelko Android app (facts only), and [P] from our own
PM220 work (pm220_rp2_webapp / nelko-pm220).

**Summary:** the P21 is the same printer firmware family as the PM220. It uses the same TSPL
subset, the same `ESC ! ?` / `ESC ! o` status bytes and the same `BATTERY?` / `CONFIG?` queries.
The differences are a 96-dot head, a 14 mm wide label, and SSP without a PIN.

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

**Our choice:** the driver uses **Classic SPP** (`pex_bt_classic_open`, uuid 0x1101, fallback
channel 1, no PIN list needed but `"0000"` is harmless, `drop_key` on). It's faster than 20-byte
GATT writes and matches the PM220 driver. A BLE path (`ff00`/`ff02`/`ff01`) can be added later
if a firmware turns out to be BLE-only; the protocol bytes are identical.

## Framing

- **Text commands:** ASCII, end with CRLF. The printer acts on `\n`. Replies, where there are
  any, end with CRLF. [M] [J]
- **Status commands:** raw `ESC` sequences with no terminator. The reply is raw bytes with no CRLF.
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

## Open questions for the bench (P21 arriving)

1. Does SSP really pair with no prompt from our IO_NONE host? Does a legacy PIN ever get asked for?
2. Which of `TIMER` and `TIMEOUT` is right? (We don't send either by default.)
3. Is the full-length 320 rows clean on the user's roll? (Calibration preset: 284 vs 320.)
4. What is the DENSITY default and its useful range on this unit?
5. Pacing: a 3.4 KB job in one RFCOMM stream; is there any flow-control trouble (the BLE path
   has `ff03` credits)?
