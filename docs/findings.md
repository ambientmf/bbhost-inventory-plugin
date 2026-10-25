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
