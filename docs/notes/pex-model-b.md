# PEX model B for our modules (feature/pex-model-b, 2026-10-28)

Why: under model A (`MODEL reloc`, gcc) a module installed after boot runs
from a RAM copy until the next boot, because its flash copy holds absolute RW
addresses and the RW area is reserved only at mount
(`btgw/pex/runtime/pex_xip.c:485,797`; parent-requirements R11 item 3, R14).
Model B (`MODEL pic`, clang `-fropi -frwpi`, RW through r9) runs in place
right after `pex.install` and takes pool SRAM only for its RW while loaded.
Simulator only so far: nothing here has run on a board.

## The switch

- `PRN_PEX_MODEL` in `drivers/CMakeLists.txt`, `reloc` (default) or `pic`. `prn_add_module()` uses it unless a `module.cmake` pins `MODEL`; the module files no longer name one.
- `tools/build.py drivers --pex-model pic` builds into `build/drivers-pic/pex/` (the `.pex` names are the same as model A's, so one tree per model).
- To make B the default: change the cache default in `drivers/CMakeLists.txt`. Existing build trees keep their cached value until reconfigured with `-DPRN_PEX_MODEL=pic`. `tools/deploy.py` ships from `build/drivers/pex/`, so that tree decides what goes to the card.
- clang has no C library headers for arm-none-eabi and our sources include `<string.h>` (memcpy and friends come from the host API in both models). Model B takes newlib's headers from the gcc toolchain (`$PEX_GCC -print-sysroot`/include), as model A does implicitly. libgcc was already shared.

## What had to change in the code

pexlink refused every `const` table of pointers in rodata, naming each:

| module | tables |
|---|---|
| printer_hub | `OPS`, `job_end.names`, `op_status.st` |
| nelko_tspl | `MODELS`, `on_cl.why`, `send_query.q` |
| phomemo_d30 (spp_printer.inc.c) | `DRV`, `WHY` |
| nelko_pl70e | `pl70e_pipes` |

They are `PEX_PTR_TABLE` (non-const, kept in RW) now; `pl70e_pipes` is plain non-const (extern linkage keeps clang from moving it to rodata, and `pl70e_proto.c` stays free of pex.h). Under model A `PEX_PTR_TABLE` is empty: the tables just move from rodata to data.

Cross-module calls: every one already goes through `pex_call` (hub to driver in `dcall`, driver to hub in `prn_core.c` and `spp_printer.inc.c`). The only function-pointer tables (`spp_drv_t`, `pend_add`) stay inside one module. Host callbacks (ops, tasks, Bluetooth) are entered through the runtime's trampoline (`pex_load.c` `pex_tramp`).

Also on this branch:
- the hub's 4 KB `printers.json` buffer (auto-connect) is a scratch arena (`pex_scratch_begin/alloc/end`) instead of `pex_malloc`, so it can't strand a hole on the pool's long-term side;
- each module logs where it was loaded once at APP_RUN (`PRN_LOG_BASES()` in `printer_driver.h`), e.g. `[pex:printer_hub] printer_hub: code 10402370 rw 20043130`. code is `pex_main` (image offset 0), rw is `__pex_rw_start` (r9 under B). An ELF symbol's address is that base plus its offset in its segment (B links RW at 0x40000000).

## Sizes, model A vs B

pexlink, the `--no-debug` files. code = text + ro, RW = data + bss + GOT.

| module | file A | file B | code A | code B | text A / B | RW A | RW B |
|---|---|---|---|---|---|---|---|
| nelko_pl70e.m33 | 11680 | 13868 (+19%) | 9356 | 12920 (+38%) | 7516 / 11036 | 1396 | 1392 |
| nelko_tspl.m0p | 15540 | 16144 (+4%) | 12928 | 14896 (+15%) | 10384 / 12884 | 1712 | 1792 |
| nelko_tspl.m33 | 15080 | 17184 (+14%) | 12428 | 15936 (+28%) | 9884 / 13924 | 1712 | 1792 |
| phomemo_d30.m33 | 6720 | 7604 (+13%) | 5128 | 6452 (+26%) | 4088 / 5524 | 2460 | 2588 |
| printer_hub.m0p | 17792 | 17532 (-1%) | 15768 | 16396 (+4%) | 14108 / 14752 | 7232 | 7328 |
| printer_hub.m33 | 11972 | 12948 (+8%) | 9988 | 11812 (+18%) | 8328 / 10168 | 7232 | 7328 |
| printer_virtual.m0p | 4348 | 4700 (+8%) | 3448 | 4144 (+20%) | 2308 / 3004 | 628 | 628 |
| printer_virtual.m33 | 4016 | 4832 (+20%) | 3092 | 4276 (+38%) | 1952 / 3136 | 628 | 628 |

- RW, the scarce part (the 48 KB pool), is the same within 128 B: the pointer tables and the GOT.
- Code grows 18-38 % on M33, much more than the 3-8 % pex-format.md gives for ROPI itself. Most of it is clang `-Os` against gcc `-Os`, not r9 (the M0+ builds, where gcc is weaker, grow 4-20 %). Code costs flash in the PEX partition (installed) and nothing in SRAM; the file sizes matter only for the R11 item 1 copy through the pool.
- The file is the same size as what the loader keeps in flash: model B has no RO relocations (A has 91-386 of them).

## mcusim

`tools/e2e_sim.py --pex-model pic --install` (standalone image from `tools/build.py standalone`, parent 719b357, mcusim feature/printer-sims): for each printer, after the gateway is up, printer_hub and the driver are uploaded to `lfs:/pex/`, `pex.install`ed, `pex.load`ed by name (the installed copy) and started; the script fails if a model-B module isn't `xip:true`. After the job it unloads and uninstalls the driver and installs, loads and starts it again, twice, with the hub running, and compares the pool's `free`/`largest` each time with the value before the driver was first loaded.

Results (2026-10-28), every module installed after boot:

| printer | driver | hub / driver after install | page | pool free/largest, hub only → after each of 3 driver unload+uninstall |
|---|---|---|---|---|
| PM220 | nelko_tspl | pic, `xip:true` / pic, `xip:true` | 384×216 at 0,24 on 400×240, 0 dots differ | 38440/38440 → 38440/38440 ×3 |
| P21 | nelko_tspl | same | 96×320 on 112×320, 0 dots differ | same |
| D30 | phomemo_d30 | same | 96×320 on 96×320, 0 dots differ | same |
| PL70e-BT | nelko_pl70e | same | 812×1218 on 832×1218, 0 dots differ | same |

After both modules were unloaded and uninstalled the pool was back at
46072/46072 each time. The console for one of them:

```
pex: xip: installed printer_hub v0.1.0 (model B, 12948 bytes) at 0x02000
pex: loaded printer_hub v0.1.0 (m33, model B, in place (XIP)): code 11812 @10402370, RW 7328 @20043130, 40 imports, 1 exports
[pex:printer_hub] printer_hub: code 10402370 rw 20043130
pex: xip: installed nelko_pl70e v0.1.0 (model B, 13868 bytes) at 0x06000
pex: loaded nelko_pl70e v0.1.0 (m33, model B, in place (XIP)): code 12920 @104062F0, RW 1392 @20042A98, 34 imports, 1 exports
[pex:nelko_pl70e] nelko_pl70e: code 104062F0 rw 20042A98
...
pex: unloaded nelko_pl70e
pex: xip: uninstalled nelko_pl70e
pex: xip: installed nelko_pl70e v0.1.0 (model B, 13868 bytes) at 0x06000
pex: loaded nelko_pl70e v0.1.0 (m33, model B, in place (XIP)): ...
```

The same with model A (`--pex-model reloc --install pl70e`) prints too, but
both modules report `xip_ready:false` / `xip:false` and run from RAM
(code_addr 0x2004xxxx). The hub alone then takes 17,536 B of the pool
(46072 → 28536) against 7,632 B under model B (46072 → 38440): its code no
longer comes out of the pool.

What this does not cover: the pool when it is crowded (the reinstall always
found the same spot free, so "no holes" is shown for an otherwise quiet pool);
an install while another module runs from flash next to it with Anker-sized
RW; a power cut during install (the parent's gw_sim covers that); the m0p
(RP2040) builds, which were only linked.

## Open, before trying it on hardware

- Owner: flip the default to `pic`? Then `deploy.py` ships model B from `build/drivers/pex/`.
- The bench board: the modules already installed there are model A. Replacing them with B builds still needs R11 item 1 (the whole file through the pool at install) to fit; B frees the reboot of item 3, not that one.
- pexcc (the browser editor's compiler) emits model A only (R14); a driver built in the browser keeps the reboot problem.
- Code growth: if flash in the PEX partition gets tight, compare clang `-Oz`, or gcc for B once the SDK supports it (pex-format.md section 11).
