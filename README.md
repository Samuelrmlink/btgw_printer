# btgw_printer

A label and thermal printer app built on top of
[bluetooth_rp2_webapp](https://github.com/Samuelrmlink/bluetooth_rp2_webapp)
(BTGW). The parent is included as a git submodule (`btgw/`) and is never
modified here. To take parent changes, move the submodule pin forward.

Printer drivers are PEX native modules. The web app is a vendor-neutral
successor of [pm220_rp2_webapp](https://github.com/Samuelrmlink/pm220_rp2_webapp).

## Modes

- **Companion**: the normal BTGW image and UI (SD build), plus a launcher
  button that opens this app from the micro SD card.
- **Standalone**: BTGW firmware built from this repo's config (no SD or
  W5500 by default; RP2350 first). This app is the only web app on the
  board. Wi-Fi, WireGuard and other enabled features are set up from it.

## Printers

One driver per protocol family and transport; a driver covers several
models through a model table.

| Vendor | Model | Transport | Driver | Status |
|---|---|---|---|---|
| Nelko | PM220 | Bluetooth Classic SPP | `nelko_tspl` | tested in simulation (mcusim) |
| Nelko | P21 | BLE | `nelko_ble` | tested in simulation (mcusim) |
| Nelko | PL70e-BT | BLE | `nelko_pl70e` | tested in simulation (mcusim) |
| Phomemo | D30 | Bluetooth Classic SPP | `phomemo_d30` | tested in simulation (mcusim) |

## Roadmap

What's next is [docs/roadmap.md](docs/roadmap.md). What we need from the parent is
[docs/parent-requirements.md](docs/parent-requirements.md).

## Branches

- `feature/*`: unvetted work.
- `dev`: features the owner has approved.
- `main`: tagged releases only (GitHub releases carry binaries, web assets
  and license files).

## License

MIT, the same as the parent. See LICENSE.
