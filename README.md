# bb-inventory-order-mod

A [bbhost](https://github.com/droogie/bbhost) plugin for Bloodborne on PC that gives copies of the same weapon a **predictable order in the inventory and equipment menus**, based on their blood gem loadout.

## The problem

At meta level (BL120, everything +10) you carry several copies of the same weapon with different gem loadouts. They show up in an order that changes over time, so you have to inspect gems to find the one you want.

## What the save data shows

Details and evidence are in [`docs/findings.md`](docs/findings.md).

- All copies of a weapon share one `EquipParamWeapon` row, so they tie on the param sort ID.
- **Ties are broken by GaItem handle order, not inventory slot order.** The GaItem is the per-copy record holding durability and gems. Handles are renumbered on every save, and their order follows the position of the GaItem blocks.
- GaItem pool positions are reused first-free. Storage trips, pickups, gem swaps and chalice loot can all move a copy ahead of or behind its twins. The player has no reliable way to control the order (unconfirmed, but consistent with every sample so far).

No param edit can tell two Saw Cleavers apart, so the fix has to be in code: keep the game's sort and replace its tie-breaker.

## Approach: a bbhost plugin

bbhost runs the decrypted 1.09 `eboot.bin` natively on PC and exposes a C plugin API (`include/bbhost_plugin.h`): `hook`, `read`/`write`, `call_guest`, and option descriptors that appear in the in-game F9 plugin menu. That makes this possible without patching bbhost itself.

The plugin will:

1. Hook the inventory sort comparator.
2. On a tie (same param sort ID), resolve each copy's GaItem from its handle and read its gems.
3. Compare a gem-loadout key instead of the handle.
4. Expose an option such as `Duplicate weapon order: game | gem loadout`.

### Already mapped in bbhost

- Equip param `sort_id` offsets: `include/bbhost/engine/param.hpp`
- Inventory layout and add/lookup functions: `plugins/boss_rush/boss_rush.cpp`
- GaItem reference functions: `include/bbhost/engine/sprj/game_data_man.hpp`
- `FrpgMenuDlgInventory` / `FrpgMenuDlgEquip` classes and the inventory list update (`sub_1f108c0`): `include/bbhost/engine/symbols.hpp`, `src/engine/menu_pointer.cpp`

### Still to find

- The sort routine and its comparator. Start from the inventory list update and step in with x64dbg attached to `bbhost.exe`.

### Later

- Gem-loadout classification: decide what counts as a "group of gems" and build the sort key from it.
- Optionally, upstream the option into bbhost's own PC Enhancements menu (`src/engine/option_menu.cpp`).

## Building and installing

```sh
tools/build-plugin.sh            # needs clang + mingw-w64; uses /mnt/c/bb/bbhost-dev/include
cp build/inv_order.dll <bbhost dir>/plugins/
```

Settings live under `[inv_order]` in `bbhost.toml`, or in the in-game plugin menu (F9):

- `ties = "game" | "reverse"`: order of identical items. `reverse` is a test mode: identical weapons appear in the opposite order.
- `dump = true`: logs list entries to `<data>/plugins/inv_order/inv_order.log`.

Windows Smart App Control can block an unsigned DLL it hasn't seen before (bbhost logs `error 4551`).

## Repo layout

```
docs/       research notes: findings, structures, offsets
tools/      helper scripts (inv_dump.py: dump weapons, handles and gems from a save)
plugin/     the bbhost plugin (inv_order.c)
```

### tools/inv_dump.py

```bash
export BB_WEAPONS_JSON=~/Bloodborne-save-editor/src-tauri/resources/weapons.json  # optional, for names
python3 -I tools/inv_dump.py saves/userdata0002_*
```

The save layout comes from [Noxde/Bloodborne-save-editor](https://github.com/Noxde/Bloodborne-save-editor).

## Not in this repo

Game files, the executable, saves, screenshots and RE databases are **gitignored** and must never be committed. Bring your own legally dumped copy of the game.

## Target

- Game: Bloodborne 1.09 (the eboot SHA-256 that bbhost enforces)
- Runtime: bbhost on Windows 11
- Tools: x64dbg, Ghidra or Binary Ninja (bbhost addresses use base `0x400000`), a C/C++ toolchain, ImHex
