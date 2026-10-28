# PL70e-BT work notes

## Resolved

- **2026-10-28: the silent / frozen printer was a brownout.**
  - It ran from a 24 V buck-boost module; on the factory adapter it no longer freezes.
  - Details and symptoms are in docs/protocols/nelko-states.md 6.0.
  - The quiet-while-printing change (fbdd4a4) was reverted (c1a5340): mid-print READSTA is safe.
  - Bench tools for one raw 3x1 OEM job with a timed READSTA lived in the session scratchpad (`rawprint.py`). Rebuild from 2.4 if needed.

## Open

- **2026-10-26: printer silent after the first test job.**
  - READSTA had answered `READSTA PAPER,` right before the job.
  - The job was a 20.9 kB TSPL BITMAP (832×200 dots), sent with tools/pl70e_probe_print.py.
  - Since then: no reply to READSTA or `ESC !?` on any of the four GATT pipes, including after a reconnect.
  - Nobody was at the bench, so it isn't known whether a label came out.
  - Next: look at the printer (paper, lid, light), power-cycle it, then rerun `tools/pl70e_q.sh 3.1 'READSTA \r\n'` after subscribing to 3.0.
- **Classic SPP to DC:0D:30:89:6C:C5 failed through `classic.connect`.**
  - Level 2: "security level not met".
  - Level 0: "RFCOMM create failed", whether or not the LE link was up.
  - BLE is the transport we use, so this isn't blocking. It may be worth a parent issue if the printer really is SPP-capable.
- **S10e capture:** not attempted. The APK and the live probes were enough for the job format.
- **2026-10-26: the module on the bench gateway (fw 1.277, Anker running).**
  - nelko_pl70e loads from SD (`pex.load {"path":"sd:/..."}`) only when linked with `pexlink --no-debug`: 9,280 bytes, 7,432 code, 1,284 RW.
  - With the DEBUG section the file is 11.3 KB. An SD load holds the whole file in the pool while it allocates the code, so that version doesn't fit.
  - The pool has 20,160 bytes free next to Anker (23.5 KB RW), so neither the real printer_hub (13.8 KB file, 11 KB bss) nor a 4.5 KB bench stand-in (tools/benchhub) fits beside it. An end-to-end module print on this board is therefore blocked.
  - Ways out, none taken without the owner: an image without Anker (the standalone image), stopping Anker for the test, installing both modules to the XIP partition plus a reboot, or a larger `PEX_POOL_KB`.
  - LittleFS on the bench board refuses any write over ~1 byte ("write failed" / "fs write busy or failed"), so job files and modules went to the SD card.
  - Module and SD files were removed afterwards. Only Anker is loaded, as before.
- **For printer_hub / pex.cmake (stream 2):**
  - tspl.c's continuous-length line divides in 64 bits, which pulls in `__udivmoddi4` (+900 B per module). `rows*2540` fits in 32 bits.
  - pex_add_module has no way to pass `--no-debug`.
  - The hub's 11 KB bss is a lot for the 48 KB pool.
