# Third-party code

btgw_printer is MIT licensed (LICENSE). The parent firmware's own third-party list is
`btgw/THIRD_PARTY.md`. Code adapted from other projects in this repository:

| Where | From | License | What |
|---|---|---|---|
| `drivers/vendors/nelko/p21/p21_proto.c` | [julienvincent/nelko-p21-rs](https://github.com/julienvincent/nelko-p21-rs) © 2026 Julien Vincent | MIT, `licenses/nelko-p21-rs.MIT.txt` | P21 job layout, reply parsers, CRC-16 |
| `drivers/vendors/phomemo/d30/d30_proto.c` | [polskafan/phomemo_d30](https://github.com/polskafan/phomemo_d30) © 2022 Timo 'polskafan' Dobbrick, which adapted [theacodes/phomemo_m02s](https://github.com/theacodes/phomemo_m02s) © 2021 Alethea Katherine Flowers | MIT, `licenses/phomemo_d30.MIT.txt`, `licenses/phomemo_m02s.MIT.txt` | D30 preamble and raster job bytes |

Protocol facts from copyleft projects and vendor apps were used without taking code;
`docs/references.md` lists every source and how it was used.
