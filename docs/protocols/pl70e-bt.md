# Nelko PL70e-BT protocol

The PL70e-BT is a 4-inch (104 mm head) 203 dpi direct-thermal shipping-label
printer. It speaks **TSPL** over a Feasycom dual-mode Bluetooth module.
Over BLE, each GATT "transparent UART" service is a plain byte pipe to the
printer's TSPL parser. There is no vendor framing, no credits and no
encryption.

Sources:
- **[live]** probes of the bench unit through the gateway (2026-10-26).
- **[apk]** the Nelko Android app (`~/Downloads/Nelko_apk.zip`), decompiled for facts only; no code is copied.
- **[orbsa]** Orbsa/Nelko-PL70e-CUPS (MIT); see docs/references.md.

## Identity

| What | Value | Source |
|---|---|---|
| LE name | `PL70e-BT_6CC5-LE` (the suffix is the last 2 bytes of the MAC) | live |
| Classic name | `PL70e-BT_xxxx`. The app treats a 13-character `PL70e-BT_xxxx` name as the "OEM" variant | apk |
| LE address | DD:0D:30:89:6C:C5, random (addr_type 1) | live |
| BR/EDR address | DC:0D:30:89:6C:C5 (DIS serial string `DC0D30896CC5`) | live |
| Advertising | flags 06, 16-bit UUID 0x18F0, 128-bit UUID e7810a71-73ae-499d-8c15-faa9aef0c3f2, name in the scan response | live |
| DIS (0x180A) | model `FSC-BT986`, manufacturer `Feasycom`, firmware `9.8.6,20260701,22789,233`, hardware `1.3` | live |
| MTU | 185 negotiated with the gateway (the Android app doesn't use BLE for this printer) | live, apk |

Match a printer by an LE name prefix of `PL70e-BT`, or by service 0x18F0
plus a name starting `PL70e`.

## GATT

Every one of these pipes reaches the same TSPL parser. The driver uses
**0x18F0**.

| Service | Write (write, write-no-response) | Notify | Notes |
|---|---|---|---|
| `18f0` | `2af1` | `2af0` | READSTA replies arrived here [live] |
| `fff0` | `fff2` | `fff1`, `fff3` | Feasycom default; orbsa's default channel |
| `49535343-fe7d-4ae5-8fa9-9fafd205e455` (ISSC) | `49535343-8841-43f4-a8d4-ecbe34729bb3` | `49535343-1e4d-4bd9-ba61-23c647249616` | |
| `e7810a71-73ae-499d-8c15-faa9aef0c3f2` | `bef8d6c9-9c21-4c9e-b632-bd58c1009f9f` (also read and notify) | same characteristic | |

Orbsa's unit (firmware v3.0.1.407) also exposes `fec7`/`fec8` and returns the
1-byte `ESC !?` status on `fec8`. The bench unit has no `fec7`.

- **Chunking:** writes of up to MTU−3 bytes (182). Orbsa writes 180-byte chunks with response.
- **Pacing:** the Android app never uses BLE for this printer: it prints over Classic SPP in 1024-byte chunks with 3 ms between them [apk]. Its 512-byte BLE packs and MTU 512 belong to other models.
- **Flow control:** none at the GATT level. Pacing comes only from write-with-response, or from the link's write queue.

## Status

| Query | Reply | Source |
|---|---|---|
| `READSTA \r\n` | `READSTA PAPER,\r\n` when ready | live |
| | `READSTA NOPAPER,\r\n` | apk |
| | `READSTA LIBOPEN,\r\n` (lid open) | apk |
| | `READSTA OVERHEAT,\r\n` | apk |
| `ESC ! ?` (`1b 21 3f`) | one byte: 0x00 idle, 0x04 paper out (0x02 rides with it), 0x20 printing | orbsa |

`BATTERY?`, `CONFIG?`, `~!T` and `~!I` got no reply [live]. The app's
PL70e status call is READSTA, sent once with a 1.5 s timeout (no retries),
before a job and between the pages of a multi-page job; never while a
single label prints [apk]. Its self-test check uses `ESC ! ?`. The full
state map, sequences and recovery notes: [nelko-states.md](nelko-states.md).

## Print job

The app has two builders. Both produce standard TSPL [apk]:

```
SIZE 101.6 mm,152.4 mm\r\n      label w,h (the app uses its paper size, e.g. 102 x 152)
GAP 3 mm,0 mm\r\n               continuous media: GAP 0 mm,0 mm
DIRECTION 0,0\r\n
DENSITY 8\r\n                   0..15, settable (densityModel 1)
CLS\r\n
BITMAP x,y,wbytes,h,mode,<wbytes*h bytes>\r\n
PRINT 1,<copies>\r\n            "normal" builder: PRINT <copies>
```

- **"OEM" builder:** also sends `SET GAP ON` (gap media) or `SET GAP OFF` (continuous) and `SET CUTTER 1` before `CLS`; BITMAP mode 0, threshold 190. nelko_pl70e sends `SET GAP ON/OFF` since ca06db1.
- **SPEED:** orbsa sends `SPEED 4`. The app does not set speed for the PL70e.
- **Bitmap:**
  - 1 bit per dot, MSB first, each row `wbytes` long.
  - **bit 0 = black (burn), 1 = white**: luminance ≤ threshold gives 0 [apk].
  - Width is up to 832 dots (104 bytes); the app's `maxWidth` is 104 mm.
  - Mode 1 is uncompressed.
- **Compression:** mode 3 is QuickLZ/"TLV zip" compressed, as `BITMAP x,y,wb,h,3,<len>,<data>` (cloud config `isCompression 1`, `compressionParam 3`). It is not needed: the uncompressed form works without it.
- **Model data** (app cloud config):
  - 203 dpi, max width 104 mm, paper length 10–300 mm;
  - default template 102 × 152 mm, default gap 3 mm, default rotation 90;
  - "printing height" table: label 25 → 23 mm, label 32 → 30 mm.

## Bench notes (2026-10-26)

- **First job:** a 832 × 200 bitmap at y=40 on 4×6 stock, density 8, sent as 180-byte write-with-response chunks: 20,905 bytes in 11.6 s (1.8 kB/s through the gateway's `gatt.write` op, one op per chunk).
- **Did it print?** Nobody was there to see the label, so this is unconfirmed.
- **After the job:** the printer stopped answering READSTA and `ESC !?` on every pipe, including after a reconnect and after padding a possibly unfinished BITMAP with white bytes.
- **Status:** open. Check the paper, lid and light, power-cycle the printer, then query READSTA again. See docs/notes/nelko-pl70e.md.
- **Classic SPP** (`classic.connect` to DC:0D:30:89:6C:C5): with security level 2 it failed "security level not met". With level 0 it failed "RFCOMM create failed", with the LE link up and with it down.
