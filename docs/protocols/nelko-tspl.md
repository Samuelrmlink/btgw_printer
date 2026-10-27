# Nelko PM220: protocol as implemented

Driver: `drivers/vendors/nelko/tspl/` (module `nelko_tspl`, model `pm220`).
The same driver runs the P21 (`docs/protocols/nelko-p21.md`): one firmware
family, one model row each in `MODELS[]`.

## Sources

All the owner's own MIT code:
- `pm220_rp2_webapp` firmware (`~/Projects/pm220-pico2w`): `src/bt/bt_core.c` (link), `src/printer/tspl.c` and `job.c` (job and layout).
- `nelko-pm220` Python reference (`~/Projects/nelko-pm220/src/nelko_pm220/protocol.py`): status replies, checked against a live PM220 (fw 1.39, RFCOMM channel 1, 203 dpi).

## Link

Bluetooth Classic SPP. The printer's USB-C only charges.

| Step | What the driver does | Origin |
|---|---|---|
| Find | Inquiry. Claims a device named `PM220` (anywhere in the name, any case) as a PM220, or one whose name starts with `P21` as a P21. A device with no name yet and Class of Device major class 6 (imaging) is held until the inquiry ends, plus 2 s for late names; if it still has no name, it is reported with `"model":null`. A name that matches no model (`PL70e-BT_6CC5`) is never claimed. | `bt_core.c` `looks_like_printer()`, narrowed to the name |
| Model | The connect config's `model`, else the inquiry name, else `pm220` with a `printer.log` line. A job's `model` overrides it. | |
| Service | SDP for the Serial Port UUID 0x1101. Falls back to channel 1. A channel that worked before (from the config) skips SDP. | `bt_core.c` |
| Pairing | Legacy PIN, tried in this order: a PIN that worked before (config `pin`), then `0000`, then `1234`. SSP just-works if the printer asks for SSP. | `bt_core.c` (`0000`, retried with `1234`) |
| Stale key | When the printer has forgotten the bond, the host drops its key and pages again once (`drop_key`). | `bt_core.c` `connect_failed()` |
| Reconnect | 8 s, doubling to 60 s, while the user wants the printer. Three rounds of pairing failures stop it. | `bt_core.c` backoff |
| Keepalive | `BATTERY?` every 25 s when no job runs. | `bt_core.c` |

How the PINs are given matters. The parent's classic group answers a PIN list
given at open in order. Its one drop-key retry starts that list again, so a
`["0000","1234"]` list never reaches `1234` when `0000` is refused (the fake
host's `t_classic` test shows it). The driver therefore opens with an empty
list, answers each `PEX_CL_PIN` itself with the next PIN, and reports the
one that worked in `printer.state` (`pin`).

## Commands and replies

All lines end in CRLF.

| Sent | Reply | Parsed into |
|---|---|---|
| `BATTERY?` | `BATTERY ` + 1 byte BCD percent + 1 byte charging + CRLF | `battery`, `charging` |
| `CONFIG?` | `CONFIG ` + dpi (u16 BE) + hw (3 bytes) + fw (3 bytes) + [settings] + CRLF | `dpi`, `hw`, `fw` (`x.y.z`) |
| `ESC !?` CRLF | 1 byte: bit 0 lid open, bit 2 out of paper, bit 5 busy (0 = ready) | `lid`, `paper`, `busy` |
| `ESC !o` | 16 bytes: [0] readiness, [7] paper type (0 continuous, 1 gap, 2 black mark), [11] label length mm, [13] label width mm (RFID), [14..15] CRC-16/MODBUS of [0..13], big-endian | `media` |

After connecting the driver asks `CONFIG?`, `BATTERY?` and `ESC !?`, one at a
time, waiting up to 1.8 s for each. A reply with no terminator is closed by
200 ms of silence. Parsers: `nelko_proto.c`, with vectors from the Python
tests in `test_pm220.c` (`t_proto_vectors`). `BATTERY ` is followed by one
space only: 20 % is the BCD byte 0x20, a space.

## Print job

The web app sends the rows (1 bit per dot, MSB first, 0 = burn, 48 bytes
for the 384-dot head). The driver wraps them in TSPL (`drivers/proto/tspl/`):

```
ESC !o CRLF                    status query; the 16-byte reply comes back
SIZE <w> mm,<h> mm CRLF        the label (continuous: the bitmap's length)
GAP <g> mm,0 mm CRLF           (BLINE for black-mark media; GAP 0 for continuous)
DIRECTION <0|1>,0 CRLF         1 when the job asks for 180 degrees
DENSITY <0..15> CRLF           default 8
CLS CRLF
BITMAP <x>,<y>,<wb>,<h>,1,<rows>
CRLF PRINT <copies> CRLF
```

Numbers in mm print like C's `%g`: `50`, `2.5`, `101.6`. With the old
firmware's settings the bytes equal its `tspl_build_job()` (checked by
`test_tspl` and the end-to-end `t_golden_print`). `SPEED` is never sent, as
before. Copies use `PRINT n`, so the rows go once.

Before a job the driver sends `ESC !?`:
- lid open or out of paper: the job fails with that message;
- busy: it asks again every 0.5 s for up to 10 s;
- no answer within 1.5 s: it prints anyway.

`options.preflight:false` skips the check. The job is done once every byte
has left the RFCOMM TX buffer. The PM220 reports no "printed" event.

## Not done / unknown

- Speed. The old firmware never set it, so the caps say `"speed":null`.
- Whether the PM220 prints black-mark media. `BLINE` is standard TSPL and is sent if asked; it is not tried on hardware.
- Hardware validation of this module. The PM220 was off. Everything here is checked against the simulated printer in `drivers/test/test_pm220.c` and mcusim (`tools/e2e_sim.py pm220`).
