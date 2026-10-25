# bb-inventory-order-mod

A patch for Bloodborne (running on PC via [shadPS4](https://github.com/shadps4-emu/shadPS4)) that gives weapons a **fixed, stable order in the inventory menu**.

## The problem

At meta level (BL120, everything +10), you end up carrying several copies of the same weapon with different blood gem loadouts. Every time the game restarts, those copies come back in a different order in the inventory/equipment menu, so you have to check gems to find the one you want.

## Why it needs an executable patch

All copies of a weapon share one `EquipParamWeapon` row, and so one sort key. Gems and upgrade level belong to each copy and live in the save, not in the params. When the menu sorts, the copies tie on the param sort key, and the tie is broken by something that isn't stable across launches (probably a runtime item handle or a pool-assigned slot; this still needs confirming).

No param edit can tell two Saw Cleavers apart, so the fix has to change the comparison in `eboot.bin`: keep the existing sort and add a final, deterministic tie-breaker.

## Plan

0. **Find which layer shuffles.** Save and quit, copy the save, restart, note the menu order, quit without changes, copy the save again, and diff the inventory region.
   - Save order stable, menu order changes: the problem is the display sort.
   - Save order changes: the game rewrites the inventory on load/save.
1. **Data formats.** Unpack the params (DCX/BND4) and confirm the weapon variants share one sort ID. Note that field's offset in the param struct; it's the first search key in the binary.
2. **Get a usable executable.** A decrypted ELF (`eboot.bin`) from game version **1.09**, the version the community patches target.
3. **Static RE.** Load the ELF in Ghidra and find the inventory sort routine and its comparator (look for reads of the sort-ID offset, or work back from menu strings and FMG text IDs).
4. **Dynamic RE.** Attach a debugger to shadPS4, set a memory-read breakpoint on the inventory array, open the weapons menu, and land in the sort and comparator. Work out the load base to map runtime addresses to Ghidra addresses.
5. **Patch.** Add a tie-breaker to the comparator, using a code cave if it doesn't fit in place. Ship it as a shadPS4 / GoldHEN-style XML memory patch.

### Tie-breaker options

| Option | Notes |
|---|---|
| Inventory slot index / acquisition order | Simple and deterministic. **Target for v1.** |
| Hash of the gem configuration | Stable but arbitrary. |
| User-defined order | Needs storage for the order. Deferred to v2. |

## Repo layout

```
docs/       research notes: structures, offsets, findings
patches/    shadPS4 XML patch(es)
tools/      helper scripts (save diffing, address conversion, ...)
```

## Not in this repo

Game files, the executable, saves and Ghidra project databases are **gitignored** and must never be committed. Bring your own legally dumped copy of the game.

## Target

- Game: Bloodborne (CUSA00900 / CUSA03173 etc.), version 1.09
- Runtime: shadPS4 on Windows 11
