#!/usr/bin/env python3
"""Dump weapon entries from a Bloodborne save's inventory and storage.

Layout follows Noxde/Bloodborne-save-editor (src-tauri/src/data_handling):
offsets.rs (section offsets), inventory.rs (16-byte entries), slots.rs
(60-byte GaItem blocks holding gem slots).

Usage: inv_dump.py SAVE [SAVE...]
"""
import json
import os
import struct
import sys

# Optional: path to the save editor's resources/weapons.json for names
WEAPONS_JSON = os.environ.get("BB_WEAPONS_JSON")

USERNAME_TO_INV = 469
INV_TO_STORAGE = 34268
SLOTS = 1984


def find_offsets(b):
    face = b.find(b"FACE", 0xF000)
    inv = face + 4 - 4 - 34028
    return inv, inv + INV_TO_STORAGE


def ga_blocks(b, end):
    """Map 8-byte (handle, id) key -> (offset, durability, gem ids) for 60-byte GaItem blocks."""
    out = {}
    i = 0
    while i < end - 60:
        if b[i + 16:i + 20] == b"\x01\x00\x00\x00":
            key = b[i:i + 8]
            handle = struct.unpack_from("<I", b, i)[0]
            if handle and handle >> 28 in (0x8, 0x9):
                gems = [struct.unpack_from("<I", b, v + 4)[0] for v in range(i + 20, i + 60, 8)]
                dur = struct.unpack_from("<I", b, i + 8)[0]
                out.setdefault(key, (i, dur, gems))
        i += 4
    return out


def entries(b, start):
    for n in range(SLOTS):
        o = start + n * 16
        idx, handle, item, amount = struct.unpack_from("<IIII", b, o)
        if handle >> 28 == 0x8:  # weapons
            yield n, o, idx, handle, item, amount, b[o + 4:o + 12]


def load_names():
    if not WEAPONS_JSON:
        return {}
    d = json.load(open(WEAPONS_JSON))
    return {int(k): v["item_name"] for hand in d.values() for k, v in hand.items()}


def name_of(names, item):
    for div in (1, 10000, 100000):
        if item // div * div in names:
            return names[item // div * div]
    return "?"


def main():
    names = load_names()
    for path in sys.argv[1:]:
        b = open(path, "rb").read()
        inv, sto = find_offsets(b)
        ga = ga_blocks(b, inv)
        print(f"== {path}  inv@{inv:#x} storage@{sto:#x}")
        for label, start in (("INV", inv), ("STO", sto)):
            for n, o, idx, handle, item, amount, key in entries(b, start):
                ga_off, dur, gems = ga.get(key, (None, None, []))
                g = " ".join(f"{x:08x}" for x in gems if x)
                print(f"{label} slot={n:4d} @{o:#07x} idx={idx:#010x} handle={handle:#010x} "
                      f"item={item} {name_of(names, item)!r} ga={ga_off and hex(ga_off)} dur={dur} gems=[{g}]")


if __name__ == "__main__":
    main()
