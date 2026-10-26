# References

Each entry gives its license and how this project may use it:
- **adapt:** permissive license; code may be adapted, with attribution in THIRD_PARTY.md.
- **facts only:** read for protocol facts; no code is copied.

| Reference | License | Use | What it gave us |
|---|---|---|---|
| Nelko Android app (user-supplied APK, decompiled with jadx locally; nothing decompiled is committed) | proprietary | facts only | PL70e TSPL job layout, READSTA status strings, bit polarity, model data (dpi, widths, gaps) |
| [Orbsa/Nelko-PL70e-CUPS](https://github.com/Orbsa/Nelko-PL70e-CUPS) | MIT | adapt | PL70e GATT write characteristics, `ESC !?` status byte meanings, 180-byte chunking |
| pm220_rp2_webapp, nelko-pm220 (owner's own projects) | MIT | adapt | TSPL subset, PM220 status queries |
| [skylot/jadx](https://github.com/skylot/jadx) (tool only, not shipped) | Apache-2.0 | tool | decompiler |
