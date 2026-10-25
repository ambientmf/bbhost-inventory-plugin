# Findings

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
