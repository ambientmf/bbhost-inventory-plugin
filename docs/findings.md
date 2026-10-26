# Findings

How the game orders identical weapons, and where the plugin steps in. The engine addresses the plugin needs are in `plugin/inv_order.c` and aren't repeated here. Engine structures that bbhost already documents (GaItem references, the inventory layout, the menu classes, param rows) are in bbhost's `include/bbhost/` and are only referenced, not restated.

## Summary

The weapon menu sorts a list of small records, one per entry:

```c
struct SortRecord {
    int32_t  key1;   /* the value for the current [T] sort mode (weapon type order, attack, ...) */
    uint32_t key2;   /* the entry's GaItem handle */
    uint32_t index;  /* the entry's position in the unsorted list */
};

void sort_weapon_list(MenuEntry* entries, size_t n, int mode) {
    SortRecord* rec = alloca(n * sizeof *rec);
    for (size_t i = 0; i < n; i++)
        rec[i] = (SortRecord){sort_value(&entries[i], mode), entries[i].gaitem_handle, (uint32_t)i};
    if (mode != 0)
        stable_merge_sort(rec, n);   /* by key1 (signed), then key2 (unsigned); a mirror routine sorts descending */
    rebuild_in_record_order(entries, rec, n);
}
```

- Every copy of a weapon at the same level and variant has the same `key1`, so `key2`, the **GaItem handle**, decides their order.
- Handles are numbered from the order of the GaItem table when a save loads. Players can't see or influence them, which is why gems, durability and storage trips never changed the order.
- The plugin hooks the merge sort's entry. Before it runs, it overwrites each record's `key2` with a rank computed from that copy's gems (and, with `ignore_variants`, makes variants share a `key1`). Records that differ in `key1` sort exactly as before, so only copies of the same weapon move.

The merge sort calls itself on halves; only the first call of each sort pass is acted on.

## What the plugin reads

- **A weapon's gems.** From the handle, the plugin looks up the weapon's GaItem (the same lookup the game uses) and reads its five gem slots: each slot's shape (locked slots are marked) and the handle of the gem in it.
- **A gem's effects.** A gem's GaItem holds up to six effect ids. Compare gems by effects, never by gem item id: two gems with identical effects can have different ids.
- **Effect kinds and values.** An effect id encodes its kind: `(id / 1000) % 100`, with high-tier prefixes aliased onto the base kinds (the table is in `inv_order.c`). This matches all 12,488 effects in the save editor's data. Strength comes from the effect values in that data (`plugin/gem_values.h`); `GemeffectParam`'s own fields don't order strength reliably (`docs/sort-design.md`).

## Lab notebook (2026-10-25)

### Save experiments: the tie-break is the handle

Five rounds of saves and screenshots of one character, three Church Picks (P1-P3) and a few other duplicated weapons. Weapons were identified by their full gem data, since handles change on every save. The save layout is from [Noxde/Bloodborne-save-editor](https://github.com/Noxde/Bloodborne-save-editor).

| test | result |
|---|---|
| Inventory slot order vs menu order | Ruled out: with the picks in slots 99, 103, 162, slot order gives 249, 247, 249; the menu showed 249 (162), 249 (99), 247 (103), which is handle order. |
| Storage round trips | Slot and handle values change, but the relative handle order never did, so the menu order didn't either. |
| Durability (P4 damaged to 244, P5 at 247) | Ruled out: P4 stayed above P5. |
| Gem swap between P4 and P5 | Ruled out gem item id: the order followed the weapon, not the gem. |
| Handle order across 13 saves | Always matched the menu. Handles look like a deterministic numbering of the GaItem table at load. |

Conclusion: ties are broken by GaItem handle. The original report of copies "reshuffling" is still unexplained; GaItem table churn (pickups, chalice loot) is the likely cause. With `ties = gems` it no longer matters.

### Finding the sort

The obvious approach, catching whatever copies the menu entries during the sort, found nothing: the game sorts the 12-byte records above, not the entries, and rebuilds the entry list afterwards. The route that worked:

1. A bbhost trace plugin and x64dbg logging breakpoints (never pausing: a pause longer than a couple of seconds loses the GPU device) to find the menu's list builder.
2. The list builder walks the player's inventory, which is **not** in menu order (the picks sat at indices 97, 103, 179 and showed as 179, 97, 103), so the reorder had to happen later in the same pass.
3. Bounded disassembly of that pass found the record packing and the stable merge sort. The comparison is inlined: `a < b` if `a.key1 < b.key1` (signed), or the keys tie and `a.key2 < b.key2` (unsigned).

One wrong turn: heap-to-heap copies seen in the traces were the list's vector growing, not the sort.

### Plugin test results

- **Hooks proven:** `ties = reverse` flipped the Church Picks (247, 244, 248); `ties = game` restored the handle order live, without a reload.
- **First gem key (raw effect ids):** stable, but adding a gem moved a pick by effect-id order, which means nothing to players. Replaced by the ranked rules in `docs/sort-design.md`.
- **Fully gemmed first:** the picks read 248 (3/3), 247 (2/3), 244 (1/3), as designed. Flipping the sort direction keeps the tie order.
- **Upgrade level is part of `key1`:** a new +0 Church Pick never ties with the +10s and sorts as its own group, which is the game's grouping (weapon + level + variant) working as intended.
- **Variants:** normal, Uncanny and Lost copies differ in `key1` too. `ignore_variants` merges them; tested on the Saw Cleavers and Beasthunter Saifs (`screenshots/ignore-variants/`).
