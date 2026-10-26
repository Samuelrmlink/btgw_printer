# PL70e-BT work notes

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
