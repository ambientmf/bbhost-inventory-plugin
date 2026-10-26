#!/usr/bin/env python3
"""Predicts the inv_order (ties = gems) order of duplicate weapons in a save.

Mirrors gem_tie()/by_tie() in plugin/inv_order.c, using the save's GaItem
blocks (handle, item id, durability, ?, 1, then 5 x (slot shape, gem
handle)) and gem records. Default type and curse order only.

Usage: predict_order.py [--ignore-curses] [--ignore-variants] [--type-order=LIST] [--curse-order=LIST] SAVE   (BB_UPGRADES_JSON / BB_WEAPONS_JSON for names)
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
TYPE_KINDS = [[21, 20], [29, 30], [26, 17], [27], [32, 23, 24, 25, 14, 15, 16], [31, 22, 13], [38, 37], [43],
              [49, 52], [48, 51]]
TYPE_NAMES = ["adept", "heavy", "physical", "nourishing", "arcane", "blood", "status", "pulsing", "poormans", "fools", "misc"]
MISC = len(TYPE_KINDS)
CURSES = [28, 44, 47, 34, 36, 41]  # worst first
CURSE_NAMES = ["stamina", "kin", "beasts", "durability", "hp", "atk"]  # best first
CURSE_NAME_KINDS = [41, 36, 34, 47, 44, 28]
NUMBER = re.compile(r"[-+]?\d+(?:\.\d+)?")


def classify(kind):
    for t, kinds in enumerate(TYPE_KINDS):
        if kind in kinds:
            return t, kinds.index(kind)
    return MISC, 99 if kind < 0 else kind


def parse_list(what, value, names):
    """Like parse_list() in inv_order.c: named first, as written; the rest follow."""
    order = []
    for word in re.split(r"[, ]+", value):
        if not word:
            continue
        if word.lower() not in names or names.index(word.lower()) in order:
            print(f"warning: {what}: ignoring {'repeated' if word.lower() in names else 'unknown'} name {word!r}")
        else:
            order.append(names.index(word.lower()))
    return order + [i for i in range(len(names)) if i not in order]


def option(name):
    return next((a.split("=", 1)[1] for a in sys.argv if a.startswith(f"--{name}=")), None)


def main():
    global CURSES
    path = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    type_rank = list(range(len(TYPE_NAMES) + 1))
    if option("type-order") is not None:
        for rank, t in enumerate(parse_list("type_order", option("type-order"), TYPE_NAMES)):
            type_rank[t] = rank
    if option("curse-order") is not None:
        CURSES = [CURSE_NAME_KINDS[i] for i in reversed(parse_list("curse_order", option("curse-order"), CURSE_NAMES))]
    ignore_curses = "--ignore-curses" in sys.argv
    ignore_variants = "--ignore-variants" in sys.argv
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
            variant = item // 10000 % 10
            groups[item - variant * 10000 if ignore_variants and variant in (1, 2) else item].append(h)
    for item, handles in sorted(groups.items()):
        if len(handles) < 2:
            continue
        rows = []
        for h in handles:
            item_id, dur, slots = ga[h]
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
            t, p = classify(gem_census.kind(defining[0])) if defining else (MISC + 1, 99)
            curse = 0 if ignore_curses else len(CURSES) - worst
            key = (not (open_ and gems == open_), 5 - gems, type_rank[t], p, curse, -flats,
                   [-x for x in strength], h)
            rows.append((key, h, dur, gems, open_, t, desc, item_id))
        rows.sort()
        print(f"== {item} {inv_dump.name_of(names, item)!r}: {len(rows)} copies, predicted order:")
        for rank, (key, h, dur, gems, open_, t, desc, item_id) in enumerate(rows, 1):
            print(f"  {rank}. handle {h:08x} {inv_dump.name_of(names, item_id)!r} dur {dur} gems {gems}/{open_}"
                  f" type {TYPE_NAMES[t] if t <= MISC else '-'}")
            for d in desc:
                print(f"       {d}")


if __name__ == "__main__":
    main()
