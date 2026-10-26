# feature/nelko-pm220: notes

## Done

- fake host
- hub
- core
- TSPL
- virtual driver
- PM220 driver
- native tests
- `.pex` builds (m33 and m0p)

See the commit log for details.

## Open

- **Hardware.** The PM220 was powered off, so the module is unproven on a real printer and board. Expect the first bench run to need fixes:
  - exact RFCOMM timing;
  - whether the PM220 answers `ESC !?` when it is the first command;
  - how long the status replies take.
- **Parent R16 / C9.** Closing them needs that bench run: `BATTERY?` round trip plus a streamed job.
- **Pool.** Resident RAM if every module runs from RAM, measured on m33:

  | Module | Resident | Detail |
  |---|---|---|
  | hub | about 16.5 KB | text 8 KB, bss 7 KB |
  | PM220 | about 12 KB | |
  | virtual | about 3.5 KB | |

  That is about 32 KB of the 48 KB pool. Next to the Anker driver (136 KB, installed to flash) they should be installed to flash too (`pex.install`).
- **PIN lists.** The parent's classic group restarts the open's PIN list after the one drop-key retry. The driver works around it by answering `PEX_CL_PIN` itself (see `docs/protocols/nelko-tspl.md`). Possible parent item: carry on in the list after the retry. This is a nice-to-have, not needed.
- **Job ending.** "Done" means every byte left the TX buffer. The PM220 sends no completion event.
