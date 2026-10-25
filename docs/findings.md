# Findings

## Overview

### Background

- **Memory is one huge `byte[]`.** An address is just an index into it (`0x1001c36f0`). A pointer is a variable holding such an index. There's no GC or object header; a "struct" is a convention that "starting at index *p*, bytes 0-3 are an int, bytes 4-7 are another int…". Reading `entry + 0x70` means "read the 4 bytes at index `p + 112`".
- **The game's code is in that same array.** Its functions start at fixed addresses (`0x2060530`, …), exactly like methods you could look up by name if they had names. We name them by address.
- **Calling convention = how arguments are passed.** On x86-64 (SysV, which the PS4 uses) the first six integer or pointer arguments go in registers `rdi, rsi, rdx, rcx, r8, r9`, and the return value comes back in `rax`. So "at entry, `rdi` = first arg" is the assembly equivalent of reading `args[0]`.
- **A hook is monkey-patching / an interceptor.** bbhost overwrites the first ~17 bytes of a game function with a jump to its own stub. The stub calls our C function with the argument registers, so it gets `ctx->arg[0..5]`. Then it runs the original bytes and continues into the real function. Think Express middleware, or a Java agent wrapping a method: we see and can change the inputs before the original runs.
- **Mutating through a pointer is mutating the caller's data.** Like passing a Java array or a Python list: if the sort function receives a pointer to an array of records and we change `records[i].key2` before it runs, the sort sees our values.

### The sort, as pseudocode

```java
// Roughly what 0x2060530 does, in Java terms.
record SortRecord(int key1, int key2, int index) {}       // 12 bytes in memory

void sortWeaponList(List<MenuEntry> entries, int sortMode) {   // MenuEntry = 216-byte (0xd8) struct
    var records = new ArrayList<SortRecord>();
    for (int i = 0; i < entries.size(); i++) {
        MenuEntry e = entries.get(i);
        records.add(new SortRecord(
            statForMode(e, sortMode),   // 0x205e120: the [T] column's value (weapon type order, attack, ...)
            e.itemHandle,               // read from entry+0x70
            i));
    }
    if (sortMode != 0) {
        // 0x2064dd0 / 0x2064ff0: a stable merge sort (like Java's Collections.sort / List.sort)
        records.sort(Comparator.comparingInt(SortRecord::key1)
                               .thenComparingInt(SortRecord::key2));   // key2 compared unsigned
    }
    rebuildListInOrder(entries, records);   // copy entries into a new list in record order
}
```

- `key1` is the same for every copy of a weapon (three Church Picks have the same type and sort value).
- So `thenComparingInt(key2)` decides their order, and `key2` is the **item handle**: an ID the game assigns to each individual item when the save loads, in the order those items sit in an internal table. You can't see or change it in game, which is why gems, durability and storage trips never affected the order.

### What the plugin does

```java
// Intercepts 0x2064dd0/0x2064ff0 just before they run (first call of each pass only;
// the merge sort calls itself recursively on halves, and those calls are ignored).
void beforeSort(SortRecord[] records) {          // really: a pointer to the first record, and one to the end
    for (var r : records)
        r.key2 = myTieKey(r.key2);               // in C: first[i].key2 = tie_key(first[i].key2);
}
// today:  myTieKey = h -> ~h            ("reverse": proves the hook controls the order)
// next:   myTieKey = h -> gemKey(h)     (look up that item's gems, rank the loadout)
```

Because only `key2` changes, weapons of different types sort exactly as before. Only copies of the same weapon are reordered.

### Why it took so long to find

The obvious approach, catching whatever copies the 216-byte entries during the sort, found nothing. The game sorts the small 12-byte records, not the entries, and only rebuilds the entry list afterwards. The detours (save diffs, call traces, a debugger, memory dumps) are logged below as rounds 1-5.

## 2026-10-25: identical weapons are ordered by GaItem handle, not inventory slot

Sources:
- Five saves of one character (`saves/userdata0002_*`, not in git).
- The VLC screenshots taken in the same session.
- `tools/inv_dump.py`, which uses the save layout from [Noxde/Bloodborne-save-editor](https://github.com/Noxde/Bloodborne-save-editor) (`src-tauri/src/data_handling/`).

### Save layout used

- **Inventory entry:** 16 bytes. `u32 ?`, `u32 handle` (`0x8xxxxxxx` = weapon), `u32 item id`, `u32 amount`.
- **GaItem block:** 60 bytes. `u32 handle`, `u32 item id`, `u32 durability`, `u32 ?`, `u32 1`, then 5 × (`u32 slot shape`, `u32 gem handle`). The blocks sit before the inventory.

### Observations

1. **Handles change on every save.** Each save shows different handle values for every weapon. For example, one Stake Driver went 0x80800633 → 0x808008cb → 0x80800bd0 across three saves. Their relative order stayed the same.
2. **The tie order follows handle order, not slot order.** The handle order matches the order of the GaItem blocks in the save. The Church Picks are the deciding case (menu shows durability 249, 249, 247):

   | inv slot | handle | durability |
   |---|---|---|
   | 99  | 0x808101f8 | 249 |
   | 103 | 0x808102ab | 247 |
   | 162 | 0x8081005b | 249 |

   - Slot order would give 249, 247, 249. That doesn't match.
   - Handle order gives 249 (162), 249 (99), 247 (103). That matches.
   - The Whirligig Saws (228, 214) and Stake Drivers (+482, +519) match both orderings.
3. **A storage round trip moves the weapon.** Storing a weapon frees its inventory slot. Retrieving it puts it in the first free slot (the Stake Drivers moved from slots 55/58 to 24/31) and gives it a new GaItem block and handle.

### Interpretation (unconfirmed)

The menu sorts by param sort ID. Ties fall back to GaItem handle or pool position. Pool slots are reused first-free, so anything that frees or takes GaItem slots (storage, pickups, gem swaps, chalice loot) can move a copy ahead of or behind its twins. That would explain reorders that look random. A restart alone kept the order in these saves.

To confirm: store and retrieve one Church Pick and check that its new menu position matches its new GaItem block position. Collect more samples with more than two copies.

### Implication for the mod

The comparator's tie-break is the handle, and the handle isn't under the player's control. Moving items through storage can't give a reliable order. The fix is to replace that tie-break with a key derived from the gems.

## 2026-10-25 (round 2): Church Pick storage and durability tests

Sources: 11 saves and 4 screenshots in `screenshots/churchpick/` (not in git). The picks are identified by their full gem data, because gem handles change on every save:

| id | gems | durability |
|---|---|---|
| P1 | thrust set | 249 |
| P2 | thrust set | 247 |
| P3 | physical set (no thrust) | 249, damaged to 248 mid-test |

### Menu observations

From the screenshots and the maintainer's report:

1. **Start:** P3 (249), P1 (249), P2 (247). Screenshots at 14:36.
2. **Thrust pair only:** P1 above P2, every time. In `thrust_pair_retrieved`, P2 is in inv slot 32 and P1 in slot 38, so **slot order is ruled out**.
3. **P3 at 248 with P1 at 249:** P3 is still above P1, so **durability alone is ruled out**. Reported by the maintainer; no save captures this state, because `248_retrieved_then_249_deposited` is byte-identical to `249_retrieved_then_247`.

### Saves

- In all 11 saves, the GaItem block order (and so the handle order) is P3 < P1 < P2.
- The blocks moved and the handles changed on every save, but the relative order never changed: not after storage trips, and not after P3's durability dropped.
- The round 1 claim that storage trips reshuffle the tie order isn't supported. These tests never changed it.

### Hypotheses still standing

Both fit every observation so far:

- **H1:** ties are broken by GaItem handle or pool order.
- **H2:** ties are broken by a gem-based key (P3's set before the thrust set), then durability, highest first.

The tests so far can't tell them apart.

### Tests that separate H1 from H2

- **Durability test:** damage P1 below 247. H2 predicts P2 moves above P1; H1 predicts no change.
- **Gem test:** swap the gem sets of P1 and P3. H2 predicts the order follows the gems; H1 predicts it follows the weapon.

Take a save and a menu screenshot after each test.

## 2026-10-25 (round 3): durability test

Sources:
- Saves `both_thursts_same_gems` and `reduced_dura`.
- Captures at 15:08 and 15:11 (Game Bar, `Videos/Captures`).

Setup: the thrust picks were cut down to one gem each with identical effects (STR scaling +65, add physical +15, beasts -11.3%). The gem item ids differ: P1's gem is `0x801012ed` and P2's is `0x8010140e`. In the saves, P1 is now P4 and P2 is now P5.

| save | menu order (screenshot) | handle order |
|---|---|---|
| both_thursts_same_gems | ... 249 (P4), 247 (P5) | P3 0x21d < P4 0x262 < P5 0x28a |
| reduced_dura | 248 (P3), **244 (P4), 247 (P5)** | P3 0x536 < P4 0x57b < P5 0x5a3 |

### Result

- **Durability doesn't decide the order.** P4 at 244 stays above P5 at 247. H2's "then durability" part is ruled out.
- The handle order still matches the menu. It has stayed P3 < P1/P4 < P2/P5 across all 13 saves and has never been disturbed.
- Still possible: a key based on gem identity. P4 and P5 have different gem item ids, so a sort on gem id would also give this order.

### Next test

Swap the single gems between P4 and P5:
- If the order follows the weapons, the tie-break is the handle or pool order (H1).
- If it follows the gems, the tie-break is a gem-based key.

### Side note

The game has its own sort toggle (`[T] : Sort` on PC). The 14:16 screenshots show inventories in different sort modes, which is likely what `normal_sort` and `gem_sort` captured. bbhost doesn't add this. That means the game already has more than one comparator to look at.

## 2026-10-25 (round 4): gem swap test

Sources:
- Saves `swapped_one_gem_to_p3` and `gems_fully_swapped`. The first is a mid-swap state: one gem was parked in storage so the two identical gems could be told apart, which left P5 (247) with no gem. P5 kept handle 0x28a while it had no gem. No screenshot exists of this state.
- Captures at 15:15:19 and 15:15:24.

Setup: the single gems of P4 and P5 were swapped. P4 (244) now has gem `0x8010140e`, and P5 (247) has `0x801012ed`.

| save | menu order | handles |
|---|---|---|
| gems_fully_swapped | 248 (P3), 244 (P4), 247 (P5) | P3 0x21d < P4 0x262 < P5 0x28a |

### Result

- **The order follows the weapon, not the gem.** Gem item id is ruled out.
- The handles are identical to those in `both_thursts_same_gems` (0x21d / 0x262 / 0x28a), even though that save came from a different load. Handles look like a deterministic numbering from the GaItem table order at load.

### Status

Every test is consistent with H1: ties are broken by GaItem handle or pool order. The following are all ruled out:

- inventory slot
- durability
- gem item id
- storage trips (which leave the order unchanged)

A gem-*effect* key wasn't isolated, because P4 and P5 have identical effects. But P3 sorts first under every condition, which also matches its handle.

The order that holds is the GaItem table order. It seems fixed for each copy and has survived every action tested. It's still open what made the order change in the original report. Candidates: the game's `[T]` sort mode, or GaItem pool churn from chalice loot or acquiring items.

### For the RE work

- The comparator probably compares GaItem handles on ties. Look for a 32-bit handle compare (`0x8xxxxxxx` values, or the handle's low bits) after the param `sortId` compare.
- Replacing that compare with a gem-loadout key is the plugin's job.

## 2026-10-25 (round 5): the sort, found

Found with a bbhost trace plugin, x64dbg logging breakpoints, memory dumps, and bounded static disassembly of the 1.09 eboot. Addresses are Binary Ninja addresses (image at 0x400000); at runtime under bbhost on Windows, add 0x2ffc00000.

### How the weapon list is built

| BN address | role |
|---|---|
| `0x206ffc0` | menu-side list builder: calls the item provider, filters, adds rows |
| `0x2075fc0` | provider: walks the player's in-memory inventory (`PlayerGameData+0x380` / `+0x370`, count `+0x34c`), filters by tab, skips item 1000000 (Unarmed), and appends a 0xd8-byte entry per item |
| `0x2060d70` | builds one entry from inventory index *i* |
| `0x205d890` | entry constructor; stores the GaItem reference at **entry+0x70** via `0x1e8bb70` (GAITEM_REFERENCE_COPY_CONSTRUCT) |
| `0x2060530` | **the sort pass** (below) |
| `0x1fd7ed0` | per-row callback: only sets flag bits on the entry (equippable, etc.), no ordering |

The in-memory inventory array is **not** in menu order. For example, the Church Picks sit at indices 97/103/179 but are shown 179, 97, 103. The sort pass is what reorders them.

### The sort pass (`0x2060530`)

For each entry *i*, it builds a 12-byte record:

```
key1  = stat value for the current sort mode  (0x205e120(entry, mode); floats truncated)
key2  = *(uint32*)(entry + 0x70)              // the GaItem handle
index = i
```

If the mode is non-zero, it stable-sorts the records with `0x2064dd0`, or `0x2064ff0` (picked by the flag at `[rbp-0x214]`, likely the direction), then rebuilds the list in record order.

The comparison is inlined in `0x2064dd0` (merge sort; insertion sort for 32 records or fewer):

```
a < b  <=>  a.key1 < b.key1 (signed)  ||  (a.key1 == b.key1 && a.key2 < b.key2 (unsigned))
```

**Tie-break = GaItem handle.** This confirms H1 from the save experiments. Handles are numbered by GaItem table order at load, which is why the order survived storage trips, durability changes and gem swaps.

### Hook points for the plugin

- `0x205e120` (entry, `rsi` = entry): runs once per entry just before its record is packed. A place to compute and cache a gem-loadout key per entry.
- `0x2064dd0` / `0x2064ff0` (`rdi`, `rsi` = record range): before the sort runs, overwrite each record's `key2` with the gem key for `record.index`. Only ties consult `key2`, so other ordering is unaffected.
- `0x2060530`: the start of a sort pass. Reset per-pass state here.

These are function entries, so bbhost's `hook2` can place them; prologues still need checking.

### Corrections to earlier rounds

- Round 1's "pool reuse reshuffles copies" and the stable-sort and heap-buffer guesses from the copy traces were wrong. The heap-to-heap copies were the list vector growing (`0x2065440`).

## 2026-10-25 (round 6): GaItems and gems in memory

Found with inv_order's dump option, checked against the Church Picks' known durability and gems.

### Handle to GaItem (as `0x1e89070` does it)

```c
GaItemMan* man = *(GaItemMan**)0x593e990;    // global
GaItem* item = *(GaItem**)(man + 8 + (handle & 0xffff) * 8);
valid if item && item->handle == handle;     // handle at item+0x08
```

This only works for handles with bit `0x800000` set, which covers weapons (`0x808…`) and gems (`0xc08…`).

### Weapon GaItem

| offset | field |
|---|---|
| +0x08 | handle |
| +0x0c | item id (30001000 = Church Pick) |
| +0x18 | durability (248 / 244 / 247 matched the menu) |
| +0x30 | 5 gem slots × 16 bytes: shape at +0x08 (1 radial, 2 triangle, 0x80000000 locked), gem handle at +0x0c (0 = empty) |

### Gem GaItem

| offset | field |
|---|---|
| +0x08 | handle |
| +0x0c | gem item id |
| +0x1c | shape |
| +0x20 | up to 6 effect ids (0xffffffff = unused) |

Two gems with identical effects can have different gem item ids (`0x1080140e` and `0x108012ed`, both with effects `0x1f9f0 0x42e0 0x85aa`), so compare gems by effects, not by id.

### inv_order `ties = gems` (v0.2.0)

For each record of a sort pass, the plugin:

1. Builds a signature: the effect ids of slots 0-4 in order, with empty slots as `0xffffffff`.
2. Ranks every record by (signature, handle) and writes the rank into `key2`.

Copies with the same loadout end up next to each other in a fixed order. The handle only breaks ties between copies with exactly the same gems.
