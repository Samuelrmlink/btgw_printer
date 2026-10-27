# Nelko printers: states, sequences and recovery, from the Nelko app

What the Nelko Android app does with the PL70e-BT, PM220 and P21: every
status a printer can report, when the app asks, what it sends around a job,
the settings and firmware-update commands, and what it does when something
goes wrong. Written 2026-10-27 from the decompiled app (jadx), to work out
why the bench PL70e goes silent and how to get it back without a power
cycle.

Rules (docs/references.md): the app is read for facts only. Nothing here is
app code; class names are given so a fact can be re-checked (jadx names,
obfuscated ones as shown, e.g. `p170.O8`). Tags: **[seen]** read in the
code or config, **[inferred]** our reading, **[bench]** our own hardware.
The working notes behind this file (longer, with every class reference)
were kept outside the repo.

## 1. The big picture

- **The app is three stacks.** Nelko's own Classic/BLE stack
  (`com.print.android.base_lib.print`, facade `p170.O8`) drives every Nelko
  label printer here. The bundled LuckPrinter SDK (`com.luckprinter.sdk_new`,
  a binary `10 FF ..` protocol) and the YX SDK serve other brands' models;
  the PL70e, PM220 and P21 never touch them. [seen]
- **Models are rows in a cloud config** (`assets/release/cloud_config_nelko.json`,
  replaceable by the server). The row's `command` picks the protocol family:
  0 = TSPL "print after send" (P21, P22, P24, P31S, PM220, PM220S, PL80E),
  1 = PM230-style "print while sending" (PM230, PM290, PM290C),
  3 = PL70e-BT (the only model with 3). A device name matches the longest
  model name it starts with, case-sensitive. [seen]
- **Classic Bluetooth (SPP) is the default transport for every model,
  the PL70e included.** The scan screen starts in SPP mode; its BLE mode
  lists only names starting `P21` or `P31S`. The app's BLE profile is
  service `ff00` (write `ff02`, read `ff01`, notify `ff03`). **The app never
  writes to the PL70e's `18f0`/`2af1` GATT pipe that our driver uses.**
  [seen] The model row has `iosFlowControl: 0`, so the iOS app (which must
  use BLE) may write without any flow control. [inferred]
- **SPP link:** the device is bonded first by Android (30 s; the app never
  supplies a PIN itself), then RFCOMM to UUID `0x1101`: secure, then
  channel 1 by reflection, then insecure; 30 s socket timeout, one reconnect
  attempt. [seen]
- **Writes:** 1024-byte chunks with a 3 ms sleep after each, flushing every
  64 chunks. No ACK, credit or heartbeat at the app level; RFCOMM's own
  flow control provides the back-pressure. [seen] A byte can't be lost
  silently on RFCOMM; over our BLE path it can (section 6).
- **Request/response:** a query is written **once**; the two numbers in
  each call multiply into one timeout. A second query of the same type
  replaces the first, whose callback is dropped. No reply in time gives
  error **1005**, shown as "The printer is not responding, please try again
  later". [seen]
- **No keep-alive** for these printers, except the battery poll on models
  that report a battery (section 3.4). [seen]

## 2. PL70e-BT

### 2.1 Two firmware dialects [seen]

`util.o0o8.getPrinterDataByName`, `Devices.isPrintModelPl70eModelOEM/Normal`:

| Dialect | How the app tells | Status query | Job bitmap | Version info |
|---|---|---|---|---|
| **OEM** ("QR-PRT") | Classic name exactly 13 characters, starting `PL70e-BT_` (e.g. `PL70e-BT_6CC5`) | `READSTA \r\n` | uncompressed, threshold 190 | not used |
| **Normal** | any other `PL70e-BT…` name | `ESC ! ?` + CRLF (1-byte code, table 3.1) | BITMAP mode 3, zlib (1 KB window) | `CONFIG?`; hardware ≥ 4.0.1 is "cost-reduced" |

**The bench unit (`PL70e-BT_6CC5`, answers READSTA) is the OEM dialect.**
[bench] The rest of this section is about that dialect unless it says
otherwise.

### 2.2 Status: READSTA [seen]

Query: the ASCII bytes `READSTA \r\n` (with the space). The reply must
arrive whole in one read and match exactly, CRLF included; there is no
line reassembly.

| Reply | App code | App reaction | Message (title / text) |
|---|---|---|---|
| `READSTA PAPER,\r\n` | 0 normal | prints | – |
| `READSTA NOPAPER,\r\n` | 4 out of paper | refuses; **after the last page of a multi-page job it counts as success** | "Out of paper" / "Install the label paper correctly and try again." |
| `READSTA LIBOPEN,\r\n` (sic) | 1 cover open | refuses | "Close the cover and try again." |
| `READSTA OVERHEAT,\r\n` | 128 overheated | refuses | "Printer is overheating" / "…overheating protection. Please wait and try again later." |
| any other `READSTA …` | 82 | refuses | "printer error" / "Please try to restart the printer and print again after the fault is recovered." |
| nothing in time | 1005 | refuses | "printer busy" / "The printer is not responding, please try again later" |

There is **no busy, printing or paused word** in this dialect, so READSTA
can't tell "printing" from "ready". Unused constants next to the four
replies: `SN_Name = "YY01232954"`, `VERSION = "Ver4.24EP"`. [seen]

### 2.3 When the app asks [seen]

| Moment | Sent | Timeout | On silence |
|---|---|---|---|
| ~1 s after the link is up | `CONFIG?\r\n` | 1.5 s | ignored (the OEM unit doesn't answer it [bench]) |
| user taps Print (link up ≥ 1.5 s, else it waits) | `READSTA \r\n` | 1.5 s, one send | job refused, 1005 |
| reprint after an error | `READSTA \r\n` | 1.0 s | 1005 |
| **while a single label (any copies) prints** | **nothing** | | |
| after each page of a serial/Excel job | `READSTA \r\n`, first after the estimated print time, then every 1 s with one query outstanding | window ≥ 30 s | 1005, job stops |
| self-test (settings) | `ESC ! ?` + CRLF (not READSTA) | 1.5 s | self-test refused |

Estimated print time = max(1 s, L × 1000 / k) with L = the longer label side
in mm and k = 122 / 16 / 7 at density 4 / 10 / other: about 22 s for a 4×6
at density 8. Window = max(30 s, L × 1000 / k′), k′ = 11 / 5.7 / 4.2.

### 2.4 The job (OEM) [seen]

One TSPL stream, every line ending CRLF:

```
SIZE 102 mm,152 mm        integers (label size cast to int)
GAP 3 mm,0 mm             only when gap > 0
DENSITY d
DIRECTION 0,0
SET GAP ON                SET GAP OFF for continuous media
SET CUTTER 1              always
CLS
BITMAP x,y,wbytes,h,0,<wbytes*h raw bytes>
PRINT 1,copies
```

No SPEED, REFERENCE, HOME, FORMFEED, GAPDETECT, AUTODETECT or end marker.
Bits: 0 = black. A single-template job counts as **done when the last byte
is written**; the printer sends nothing when it finishes. Copies of one
label are one job (`PRINT 1,n`); a serial/Excel job sends one label per job
and polls between them (2.3). The Normal dialect's job differs: float sizes
(`SIZE 102.0 mm,152.0 mm`), DIRECTION before DENSITY, no SET lines,
`BITMAP …,3,<len>,<zlib>` and `PRINT n` plus an extra CRLF.

### 2.5 Cancel and recovery [seen]

- **Cancel:** stop writing chunks, then send the three ASCII letters
  `ESC` + CRLF (not byte 0x1B). The app sends no filler, so a cancel in the
  middle of a BITMAP leaves the printer waiting for the rest. [inferred]
- **No reset, clear, wake or calibration command is ever sent to a PL70e**
  during printing or error handling. Every error ends in a dialog; the
  fallback text tells the user to restart the printer.
- Settings screen items for the PL70e (`setting "1,2,3,5,6,7"`): resolution,
  hardware and firmware version, sound (`BEEP n`), self-test page
  (`SELFTEST`), factory reset (`INITIALPRINTER`). Not offered: auto
  power-off (`TIMER`), paper learning (`CALIBRATE`).
- Defined in the app's libraries but never called: `ESC ! R` (`1B 21 52`,
  soft reset), `RESET FACTORY`, `GAPDETECT`, `~!T/~!I/~!F/~!@/~!A`, `HOME`,
  `FORMFEED`. Not present at all: `KILL`, `ESC ! C`, `AUTODETECT`, any
  Feasycom `AT+` command to the printer.

### 2.6 Lights

Nothing in the app documents the LEDs: no strings, assets or manuals. The
in-app help is online (`upload.nelko.net/faq/guide.html`, rendered by
JavaScript) and the troubleshooting list from
`app.nelko.net/api/qa/getTroubleRemovalList` has entries only for photo
printers (fetched once on 2026-10-27). What we've seen [bench]:

| Lights | When |
|---|---|
| solid blue, power-key light off | the silent state: connected, no READSTA answer, jobs ignored (2026-10-27 after a paper reload) |
| solid blue + flashing red | out of paper (4×6 roll ran out) |

## 3. PM220 and P21 (command 0)

### 3.1 Status: `ESC ! ?` is a code [seen]

Query `1B 21 3F 0D 0A` (the app always adds CRLF). The reply is **one
byte, compared as a code with `==`**, not as bits
(`PrinterInfo.PrinterErrorType`):

| Byte | Meaning | Before a job | While polling a serial job | Message (title / text) |
|---|---|---|---|---|
| 00 | ready | prints | done | – |
| 01 | cover open | refuses | stops | "The cover is open. Close the cover and try again." |
| 02 | feeding | refuses | keeps polling | "The printer is printing or feeding paper" |
| 03 | paper error | refuses | stops | "printer error" / restart the printer |
| 04 | out of paper | refuses | stops (success on the last label) | "Printer is out of paper. Install paper correctly then try again." |
| 08 | "no ribbon" | refuses | stops | "Printer without ribbon" |
| 10 | paused | refuses | stops | "The printer has paused printing, please resume printing and try again" |
| 20 | busy | **refuses, no wait** | keeps polling | "The printer is printing, please try again later" |
| 24 | ran out of paper | refuses | stops | out of paper |
| 30 | low battery | **prints anyway** | done | warning only: "Low battery, please charge before use." |
| 80 | overheated | refuses | stops | overheating |
| 90 | no power | refuses | keeps polling | generic error |

- **Unsolicited bytes:** a lone `30` (or `FF 30`) with no query pending is
  a low-battery push. Any other error byte with no query pending makes the
  app drop the job's callbacks, show the error and **answer `ACK`** (ASCII,
  3 bytes, no CRLF). [seen]
- A reply that isn't exactly one byte fails with 1004. [seen]

### 3.2 Media (RFID): `ESC ! o` [seen]

`1B 21 6F 0D 0A`, 3 s timeout, for models with RFID paper (P21, P22, P31S,
PM220, PM220S): 0.5 s after connect and **before every job, as its own
query** (never inside the job). Reply: 16 bytes, valid if byte 1 or byte 4
is `0C` and the last two bytes are CRC-16/MODBUS (init FFFF, poly A001) of
bytes 0–13, big-endian.

| Byte | Field |
|---|---|
| 0 | status code (3.1), stored, not acted on |
| 1 | `0C` |
| 3 | encoding: high nibble = version; low nibble = company flag (v3) |
| 4 | colour |
| 5–6 | batch |
| 7 | paper type (0 continuous, 1 gap, 2 black mark); v2/v3: low nibble |
| 8 | gap in mm (v2/v3 only) |
| 11 | label length in mm |
| 12 / 13 | label width in mm (12 when byte 3 is `02`, else 13) |
| 14–15 | CRC |

The PM220's stock 50×30 tag is version 3, gap 6 mm. A size mismatch shows
"Label paper specifications do not match"; no tag on a PM220 shows "Cannot
recognize genuine paper" (the P21 skips that). Density on the P21/P22/P31S
is automatic: 15 with a valid tag, 4 without.

### 3.3 Sequences [seen]

- **After connect:** +0.5 s `ESC ! o` (RFID models); then `CONFIG?`; after
  its reply, `ESC ! ?` (0x30 shows the battery warning); `BATTERY?` from
  +1.5 s every 30 s, paused while a print or printer-detail page is open.
- **Before a job:** wait until the link is 1.5 s old; `ESC ! ?` with a 2 s
  timeout, only 00 or 30 go ahead (busy included refuses; a timeout is
  1005); then `ESC ! o` and the media checks; then the job.
- **The job:**
  `SIZE 50.0 mm,30.0 mm` / `GAP 6.0 mm,0 mm` (also for black-mark paper; no
  BLINE anywhere) / `DIRECTION 0,0` / `DENSITY d` (4, 10 or 15) / `CLS` /
  `BITMAP x,y,wb,h,m,<data>` / `PRINT n` / an extra CRLF. m = 1 raw, or
  **3 = QuickLZ** (level 1, 512-byte blocks, each prefixed by a 4-byte
  big-endian length) on the PM220 when CONFIG is the 20-byte form or the
  firmware is ≥ 0.1.24 (PM220S ≥ 0.1.9). PM220 bitmaps start at 0,0 for the
  full paper; P21 bitmaps are 96 dots wide at y = 18 dots for 40 mm labels
  (284 rows, 35.5 mm).
- **Done:** a single job is done when its bytes are written; serial/Excel
  jobs poll `ESC ! ?` every 1 s after the estimated print time until 00/30.
- **Cancel:** stop writing, then ASCII `ESC` + CRLF.

### 3.4 Info and settings [seen]

| Command | Reply / effect |
|---|---|
| `CONFIG?\r\n` | `CONFIG ` + dpi (u16 BE) + hardware ×3 + firmware ×3 + power-off level (0 never, 1 = 15, 2 = 30, 3 = 60 min) + beep (0/1) [+ config version, 20-byte form] + CRLF; 19 or 20 bytes |
| `BATTERY?\r\n` | `BATTERY ` + BCD percent + BCD charging flag + CRLF (12 bytes), or without the flag (11 bytes) |
| `TIMER ` + raw byte 0..3 + CRLF | auto power-off (the command is TIMER, not TIMEOUT) |
| `BEEP ` + raw byte 0/1 + CRLF | sound |
| `SELFTEST\r\n` | self-test page |
| `INITIALPRINTER\r\n` | factory reset |
| `CALIBRATE\r\n` | paper learning (not offered for these models) |
| `GETPRINTEDCOUNT\r\n` | `PRINTEDCOUNT ` + 2-byte LE count |

### 3.5 P21 extras [seen]

Classic SPP by default too; BLE is a fallback (suggested after a failed
Classic connect on the "JieLi 9PF" variant, Class of Device `0x1F00`).
BLE: service `ff00`, write `ff02`, read `ff01`, notify `ff03`, MTU 247 with
244-byte packets every 5 ms (MTU 23, 20 bytes every 1 ms on "BaiRui"
hardware under Android 14); no credit scheme. Before powering off the P21
sends `AT+DISCOVERABLE=0\r`; the app treats it as a disconnect.

## 4. Firmware updates [seen]

- The PL70e and PM220 are `upgradeType 0` ("normal"), over the same SPP
  link. Jieli RCSP OTA (`BleMainActivity`, `libjl_ota_auth.so`) is for
  other models; its scan filter merely allows `PL70e-BT` names.
- Protocol: `1F 1B 1F 00 00 01 F0 00` + file bytes 0–3, the printer echoes
  those 4 bytes (5 s); `… F0 01` + bytes 4–7, echoed (2.5 s); then the whole
  file with its first 8 bytes replaced by `… F0 02`, and the printer answers
  with the file's last 4 bytes (60 s).
- Version check: `POST https://app.nelko.net/api/firmware/verify2` with JSON
  `{dev, firmwareName, hardwareName, mac}` and app headers including
  `accessToken`; the reply has `hasNewVersion`, `firmwareDownloadUrl`,
  `md5`, `versionAttachment`. Downloading is a plain GET of that URL. We
  haven't called it: it needs the printer's version strings, and whether it
  needs an account token is unknown. A firmware image would show the real
  parser states; that is a decision for the owner.

## 5. Our drivers against the app

| | Nelko app | nelko_pl70e (BLE) | nelko_tspl (Classic) |
|---|---|---|---|
| Transport | SPP, 1 KB chunks, RFCOMM flow control | GATT `2af1` write-with-response, MTU−3 chunks, ≤ 3 in flight | SPP |
| Status before a job | fresh query, refuse on silence | **since ca06db1:** fresh READY (≤ 8 s) or it asks and answers `PEX_EAGAIN`; the hub retries for 5 s; 1.5 s settle after an error clears; an error is confirmed by that fresh answer before refusing | ESC !? with waits (busy waits; app refuses) |
| Status codes | `==` codes | **since ca06db1:** codes (bits as fallback) | **since a780610:** codes for the PM220 too (were bits) |
| Status while printing | none for a single label | READSTA right after the last byte, every 1.5 s; no answer in 30 s fails the job (F12, 442b039) | ESC !? until ready |
| `SET GAP ON` | yes (and `SET CUTTER 1`) | **since ca06db1:** yes (not CUTTER) | no (app doesn't either) |
| SIZE numbers | integers | decimals (`101.6`) | `%g` |
| Lost bytes | can't happen on RFCOMM | **since ca06db1:** a failed write fails the job and pads the BITMAP with white + CRLF (before, failures weren't even seen: the firmware reports them in the event's `status`, the driver read data bytes) | n/a |
| Cancel | `ESC` + CRLF, no filler | white filler for the rest of the BITMAP + CRLF, no PRINT | |
| Compression | PM220: QuickLZ mode 3; PL70e OEM: none | none | none (mode 1 prints) |

## 6. Why the PL70e goes silent, and what to try

**Most likely: it is stuck inside a BITMAP.** A TSPL `BITMAP` header fixes
how many image bytes follow. If any of them never arrive, the printer keeps
reading: READSTA, `ESC ! ?` and the next job are all taken as image data,
so it neither answers nor prints, and nothing looks wrong on the lights.
[inferred] It fits both bench incidents (2026-10-26 after a raw 20.9 KB job;
2026-10-27 after a paper reload) and the power cycle being the cure.
Ways bytes went missing on our side:

- **Failed writes were invisible.** The gateway reports a write's outcome
  in the event's `status` field (`att_error | reason << 8`, e.g. its 5 s op
  timeout); nelko_pl70e looked at the data bytes, so it never saw one and
  carried on without the lost chunk. Fixed in ca06db1. [seen in the parent
  source]
- A dropped link or a gateway reboot in the middle of a job (on 2026-10-27
  the board was rebooted between jobs; whether a job was in flight then is
  not known).
- The app's own cancel stops mid-BITMAP without filler, so the vendor app
  can wedge it the same way. [inferred]

**Other candidates:** a job sent while the printer was still busy after the
paper reload (it searches for the gap when the lid closes); the app never
does that, because it asks READSTA first (now ours does too). Or a media
mode the firmware lost (now covered by `SET GAP ON` in every job).

### Bench plan (next session)

Before anything, with the printer silent: note the lights, and read
`printer.status` (`unanswered`, `quiet_ms`, `writes_failed`, `writes_lost`
need the new nelko_pl70e on the board; it is 11.5 KB, so installing it
from the card may need a reboot for R11 item 1).

Recover, gentlest first, one step at a time, checking for a READSTA answer
after each:

1. `printer.connect {"driver":"nelko_pl70e","addr":"DD:0D:30:89:6C:C5","config":{"model":"pl70e","recover":"status"}}`:
   sends `ESC ! ?`, which TSPL printers answer even when paused.
2. One press of the power/feed key (note what it does).
3. `"recover":"resync"`: white bytes for the largest BITMAP we send
   (126,736 B, about 30 s), then CRLF. Ends a BITMAP the printer is still
   reading, whatever its size; a printer that isn't stuck sees one junk
   line. The next job's CLS clears the image buffer.
4. `"recover":"reset"`: `ESC ! R`, the TSPL soft reset (never used by the
   app, so unproven on this firmware).
5. Power cycle.

Then reproduce it on purpose, so the cause is known rather than guessed:
with the printer healthy, send a BITMAP header and only part of its data
(`tools/pl70e_q.sh 3.1` writes raw bytes), then READSTA. If it goes silent,
the theory holds; then step 3 should bring it back without a power cycle.
Repeat once with a paper reload instead of a cut job, to see whether the
reload alone does it.

Worth a separate test: **Classic SPP to the PL70e**, the path the vendor
uses (bonded, RFCOMM). It failed on 2026-10-26 ("security level not met",
"RFCOMM create failed"), but the app bonds first; nelko_tspl's Classic code
could carry it as a model row if it works.
