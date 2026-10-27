# B5: removing a PEX module needs free PEX pool memory

For the bluetooth_rp2_webapp agent. Found by btgw_printer on 2026-10-27, on
the bench board (10.255.255.23, Pico 2 W, 48 KB PEX pool, dev firmware with
the launchers). Source references are parent dev `719b357`.

## Summary

`pex.uninstall` can fail with `PEX pool full (index update)`. Removing a
module from the PEX flash partition only rewrites the partition index, but
the firmware builds the new index in a 3 KB buffer borrowed from the PEX pool.
When the pool is nearly full, which is exactly when you want to remove a
module, there is no room for that buffer, so the module can't be removed.
On the bench board the way out was a reboot.

The same buffer is needed by every partition change (install, uninstall,
compaction), so an SD install can fail the same way after the module file
itself has been read into the pool.

## What happened

Installed in the partition: `pinecil` (RW 2,592 B), `anker` (RW 19,068 B),
`printer_hub` (RW 7,184 B, installed after boot so it ran from RAM) and
`nelko_tspl` (RW ~1.5 KB, running in place). `pex.list` with ours running:
pool 49,152 B, `free` 2,800, `largest` 2,800.

1. `printer.disconnect`, then `pex.stop` and `pex.unload` of `nelko_tspl`:
   both OK.
2. `pex.uninstall {"name":"nelko_tspl"}` →
   `{"code":"out of PEX pool memory","message":"PEX pool full (index update)"}`.
3. `pex.stop`, `pex.unload` of `printer_hub`: OK. Its code and RW were in the
   pool, so this should have freed about 16.5 KB.
4. `pex.uninstall {"name":"printer_hub"}` → the same error.
5. `sys.reboot`. With nothing of ours loaded, `pex.uninstall` of both
   modules succeeded.

Earlier the same day the same error came from `pex.install
{"path":"sd:/.../printer_hub.m33.pex"}` right after an uninstall: the pool
had 12,800 B free, the SD install held the 11,268 B file in the pool, and
the remaining ~1.5 KB was too small for the index buffer.

## Why (from the source)

**Every partition change copies the index into pool scratch memory.**
`pex/runtime/pex_xip.c`:

- The index is `ix_t`: a 64-byte header plus `IX_MAX` (32) entries of 88
  bytes = 2,880 B, rounded up to a 256-byte page: `IX_BYTES` = 3,072 B. It
  is kept in two flash sectors. An update writes the copy that is not
  current (`commit()`).
- `ix_copy()` (line ~135) gets the new copy with
  `pex__scratch_begin(IX_BYTES, PEX_OWNER_HOST)`. If that returns NULL it
  reports `PEX pool full (index update)` and the operation stops before
  anything changes.
- Callers: `pex_uninstall()` (~686), the install path (~642), and
  compaction (~409, ~431: one journal update per overlapping step, plus the
  final one).
- The committed index itself, `s_ix`, also lives in the pool (line 81,
  "IX_BYTES, pool"), so another 3 KB stays taken for good.

**Scratch can only use the gap between the two halves of the pool, not the
free total.** `pex/runtime/pex_mem.c` (header comment, `pex__scratch_begin`
~176):

```
base                                                              end
| scratch arenas -> (LIFO, bump)   ...free gap...   <- long-term blocks |
                  ^top                             ^low
```

- `pex__scratch_begin` succeeds only if `s_low - s_top >= need`.
- Module images and RW are long-term blocks, growing down from the end. A
  freed long-term block becomes a free block in place. `s_low` only moves
  back up when the free blocks are the lowest ones (~84–87).
- So unloading a module returns its memory to the long-term free list, but
  the scratch gap grows only if nothing long-term was allocated below it
  afterwards. That's the likely reason step 3 didn't help: something
  allocated later (another module's blocks, an op's buffer) sat below the
  hub's blocks. Not confirmed on the board. `pex.list`'s `free` and
  `largest` count free long-term blocks too (~252), so `largest` can look
  big enough while scratch has no room.

**The model-A RW reservations add to it.** Installed model-A modules get
their RW reserved at the top of the pool at mount (`aoff`, `rw_area`:
26,080 B for Anker + Pinecil, 33,264 B with printer_hub). An uninstall
doesn't give that back until the next boot.

## Effect

- A module can't be removed when the pool is full, which is exactly when
  removing one is the fix. The only way out is a reboot, which stops every
  other module and client.
- Updating a module on a full board takes two reboots: uninstall needs a
  boot with nothing loaded, and the install needs another boot to drop the
  old RW reservation (see R11 in parent-requirements.md).
- `pex.list` doesn't show the number that matters (the scratch gap), so a
  tool can't tell in advance whether an install or uninstall will work.

## Suggested fixes (in order of value)

1. **Update the index without a second copy.** `s_ix` is a RAM mirror of
   the current flash copy, and `commit()` writes the other sector. So
   change `s_ix` in place, write it to the other sector, and if the write
   fails, restore `s_ix` from the still-intact current copy
   (`memcpy(s_ix, fptr(s_cur * SEC), IX_BYTES)`; the partition is
   XIP-readable). No extra memory, and the power-cut guarantee is
   unchanged, because the current flash copy isn't touched until the new
   one is verified. If a separate buffer is still wanted, make it one
   static 3 KB buffer outside the pool. Either way an uninstall can't fail
   for lack of memory.
2. **Keep `s_ix` out of the pool** (static, or read it from flash when
   needed), so the pool is all for modules.
3. **Stream SD installs into the partition** in small chunks instead of
   holding the whole file in the pool (R11 item 1). The pool would then
   bound a module's RW, not its file size.
4. **Report the scratch gap** (`s_low - s_top`) in `pex.list`, next to
   `free` and `largest`, and say in an install or uninstall refusal how much
   was needed and how much the gap had.
5. **Give back the RW reservation on uninstall** (or reserve it at load),
   so an uninstall plus install doesn't need a reboot (R11 item 2).

## Done when

On the bench board with Anker and Pinecil installed and printer_hub plus
`nelko_tspl` running, `pex.unload` and then `pex.uninstall` of either module
succeeds without a reboot, and so does installing a new `nelko_tspl` from
the card afterwards.
