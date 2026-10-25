#!/usr/bin/env python3
"""Predicts the inv_order (ties = gems) order of duplicate weapons in a save.

Mirrors gem_tie()/by_tie() in plugin/inv_order.c, using the save's GaItem
blocks (handle, item id, durability, ?, 1, then 5 x (slot shape, gem
handle)) and gem records. Default type and curse order only.

Usage: predict_order.py [--ignore-curses] SAVE   (BB_UPGRADES_JSON / BB_WEAPONS_JSON for names)
"""
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gem_census  # noqa: E402
import inv_dump  # noqa: E402

LOCKED = 0x80000000
TYPE_KINDS = [[29, 30], [26, 27, 20, 21, 17], [32, 23, 24, 25, 14, 15, 16], [31, 22, 13], [38, 37], [43, 49, 52, 48, 51]]
TYPE_NAMES = ["heavy", "physical", "arcane", "blood", "status", "special", "misc"]
CURSES = [28, 44, 47, 34, 36, 41]  # worst first
NUMBER = re.compile(r"[-+]?\d+(?:\.\d+)?")


def classify(kind):
    for t, kinds in enumerate(TYPE_KINDS):
        if kind in kinds:
            return t, kinds.index(kind)
    return 6, 99 if kind < 0 else kind


def main():
    path = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    ignore_curses = "--ignore-curses" in sys.argv
    b = open(path, "rb").read()
    effects_json = json.load(open(os.environ["BB_UPGRADES_JSON"]))["gemEffects"]
    value = lambda e: round(abs(float(NUMBER.search(effects_json[str(e)]["effect"]).group())) * 100) if str(e) in effects_json else 0
    text = lambda e: effects_json.get(str(e), {}).get("effect", str(e))
    gem_fx = {h: fx for h, gid, shape, fx in gem_census.gems(b)}
    names = inv_dump.load_names()
    inv, _ = inv_dump.find_offsets(b)
    ga = {}
    i = 0
    while i < inv - 60:  # GaItem blocks: handle, id, dur, ?, 1, 5 x (shape, gem)
        if b[i + 16:i + 20] == b"\x01\x00\x00\x00":
            h, item, dur = struct.unpack_from("<3I", b, i)
            if h >> 28 == 0x8 and h not in ga:
                ga[h] = (item, dur, [struct.unpack_from("<2I", b, i + 20 + 8 * s) for s in range(5)])
        i += 4
    groups = collections.defaultdict(list)
    for n, o, idx, h, item, amount, key in inv_dump.entries(b, inv):
        if h in ga and item != 1000000:
            groups[item].append(h)
    for item, handles in sorted(groups.items()):
        if len(handles) < 2:
            continue
        rows = []
        for h in handles:
            _, dur, slots = ga[h]
            open_ = gems = flats = 0
            worst = len(CURSES)
            defining = None
            strength = [0] * 5
            desc = []
            for s, (shape, gh) in enumerate(slots):
                if shape == LOCKED:
                    continue
                open_ += 1
                fx = gem_fx.get(gh, [])
                if not fx:
                    continue
                gems += 1
                defining = fx if defining is None else defining
                strength[s] = value(fx[0])
                for e_i, e in enumerate(fx):
                    k = gem_census.kind(e)
                    if k in CURSES:
                        worst = min(worst, CURSES.index(k))
                    if e_i > 0 and 13 <= k <= 17:
                        flats += 1
                desc.append(" + ".join(text(e) for e in fx))
            t, p = classify(gem_census.kind(defining[0])) if defining else (7, 99)
            curse = 0 if ignore_curses else len(CURSES) - worst
            key = (not (open_ and gems == open_), 5 - gems, t, p, curse, -flats,
                   [-x for x in strength], h)
            rows.append((key, h, dur, gems, open_, t, desc))
        rows.sort()
        print(f"== {item} {inv_dump.name_of(names, item)!r}: {len(rows)} copies, predicted order:")
        for rank, (key, h, dur, gems, open_, t, desc) in enumerate(rows, 1):
            print(f"  {rank}. handle {h:08x} dur {dur} gems {gems}/{open_} type {TYPE_NAMES[t] if t < 7 else '-'}")
            for d in desc:
                print(f"       {d}")


if __name__ == "__main__":
    main()
