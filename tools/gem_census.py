#!/usr/bin/env python3
"""Census of the blood gems in Bloodborne saves, classified by effect kind.

Gem records follow Noxde/Bloodborne-save-editor (src-tauri/src/data_handling/
offsets.rs, upgrades.rs): 40-byte records from offset 84 -
u32 handle, u32 gem id, u32 type (1 gem, 2 rune), u32 shape, u32 effects[6].

An effect id encodes its kind: (id // 1000) % 100 (e.g. 126619 -> 26,
Physical ATK UP), with high-tier prefixes aliased (162620 -> 62 -> 17). Names come from the save editor's resources/upgrades.json
when BB_UPGRADES_JSON points at it.

Usage: gem_census.py [--list] SAVE [SAVE...]
"""
import collections
import json
import os
import struct
import sys

UPGRADES_JSON = os.environ.get("BB_UPGRADES_JSON")
NONE = 0xFFFFFFFF

# kind code -> (category, short name). Categories follow docs/sort-design.md.
KINDS = {
    13: ("blood", "add blood"), 14: ("arcane", "add arcane"), 15: ("arcane", "add fire"),
    16: ("arcane", "add bolt"), 17: ("physical", "add physical"),
    20: ("physical", "blunt %"), 21: ("physical", "thrust %"), 22: ("blood", "blood %"),
    23: ("arcane", "arcane %"), 24: ("arcane", "fire %"), 25: ("arcane", "bolt %"),
    26: ("physical", "physical %"), 27: ("physical", "ATK %"), 28: ("curse", "ATK DOWN"),
    29: ("physical", "STR scaling"), 30: ("physical", "SKL scaling"), 31: ("blood", "BT scaling"),
    32: ("arcane", "ARC scaling"), 33: ("niche", "vs beasts UP"), 34: ("curse", "vs beasts DOWN"),
    35: ("niche", "vs kin UP"), 36: ("curse", "vs kin DOWN"), 37: ("status", "slow poison"),
    38: ("status", "rapid poison"), 39: ("niche", "vs open foes"), 40: ("niche", "stamina costs DOWN"),
    41: ("curse", "stamina costs UP"), 42: ("niche", "charge ATK UP"), 43: ("special", "HP recovery"),
    44: ("curse", "HP drain"), 45: ("niche", "rally"), 46: ("niche", "durability UP"),
    47: ("curse", "durability DOWN"), 48: ("special", "phys UP at full HP"),
    49: ("special", "phys UP near death"), 51: ("special", "ATK UP at full HP"),
    52: ("special", "ATK UP near death"), 72: ("niche", "vs kin UP"),
}


# High-tier id prefixes are aliases of the base kinds (verified against all
# 12488 effects in upgrades.json).
ALIAS = {56: 43, 62: 17, 63: 24, 64: 25, 65: 26, 66: 26, 67: 26, 68: 29, 69: 30, 70: 33,
         71: 34, 72: 35, 73: 36, 74: 37, 75: 38, 76: 49}


def kind(effect_id):
    k = (effect_id // 1000) % 100
    return k if 13 <= k <= 52 else ALIAS.get(k, -1)


def gems(b):
    """Yield (handle, gem id, shape, effects) for each gem record."""
    i = 84
    while i + 40 <= len(b):
        handle, gid, typ, shape = struct.unpack_from("<4I", b, i)
        if typ not in (1, 2) or handle >> 24 != 0xC0:
            break
        if typ == 1:
            effects = [e for e in struct.unpack_from("<6I", b, i + 16) if e != NONE]
            yield handle, gid, shape, effects
        i += 40


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    listing = "--list" in sys.argv
    names = json.load(open(UPGRADES_JSON))["gemEffects"] if UPGRADES_JSON else {}
    unknown_kinds = collections.Counter()
    for path in args:
        b = open(path, "rb").read()
        primaries = collections.Counter()
        setups = collections.Counter()
        n = 0
        for handle, gid, shape, effects in gems(b):
            n += 1
            if not effects:
                continue
            p = KINDS.get(kind(effects[0]))
            primaries[p[1] if p else f"?{kind(effects[0])}"] += 1
            setups[tuple(KINDS.get(kind(e), ("?", f"?{kind(e)}"))[1] for e in effects)] += 1
            for e in effects:
                if kind(e) not in KINDS:
                    unknown_kinds[kind(e)] += 1
                ref = names.get(str(e))
                if names and ref is None:
                    unknown_kinds[f"id {e} not in upgrades.json"] += 1
            if listing:
                desc = "; ".join(names.get(str(e), {}).get("effect", str(e)) for e in effects)
                print(f"  {handle:08x} shape={shape}  {desc}")
        print(f"== {path}: {n} gems")
        print("  primary effect kinds:", ", ".join(f"{k} x{c}" for k, c in primaries.most_common()))
        print("  most common effect combinations:")
        for combo, c in setups.most_common(12):
            print(f"    {c:4d}  {' + '.join(combo)}")
    if unknown_kinds:
        print("unclassified:", dict(unknown_kinds))


if __name__ == "__main__":
    main()
