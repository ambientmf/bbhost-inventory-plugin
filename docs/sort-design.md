# Sort design: ordering identical weapons by gem setup

Status: **goal / aspirational**. v1 (`ties = gems`) orders by raw effect ids, which is stable but not meaningful. This note describes the order players should actually get.

## Principles

1. **Weapons stay grouped as the game groups them.** Weapon + variant (normal / uncanny / lost) + upgrade level is the outer group, in the game's own order. Players' muscle memory depends on it. Only the order *within* a group changes. Today that's exactly where the plugin acts: ties on the game's sort key.
2. **Order by setup type, not by values.** Copies are grouped by what kind of build their gems make (physical, arcane, blood, …) rather than by the exact numbers.
3. **Intuitive and configurable.** The defaults below reflect how experienced players think about setups. The rankings that are a matter of taste (curse order, specialized gems) are configurable.

## Rules, in priority order

### 1. Fully gemmed first

Weapons with every slot filled come before weapons with empty slots (0-2 of 3 filled).

- "Full" is relative to the weapon's own slot count. Some parry tools (pistols and the like) have a single slot. Most of this targets right-hand weapons, but the rule must not punish weapons with fewer slots.
- Slot count comes from the weapon's GaItem (locked slots are marked).

### 2. Group by setup type

Within a group of identical weapons, cluster copies whose gems are the same *kind*:

| Setup type | Defining gems (examples) |
|---|---|
| **Adept** (ranks first) | damage-type % gems: Thrust ATK UP (Adept), Blunt ATK UP (Striking). Hyper-optimized for the weapon's damage type, so they rank above Heavy and Tempering setups. |
| **Heavy / strength** | Heavy Abyssal: STR scaling +65, plus tempering abyssal (Physical ATK UP ~24.8% + flat secondary like add physical ATK +15) |
| **Pure physical** | all Physical ATK UP % (tempering, 25-27.2%) |
| **Nourishing** | ATK UP % (all damage). It boosts physical and the latent arcane of split-damage weapons (Blade of Mercy, Burial Blade, Holy Moonlight Sword, Boom Hammer). A red gem, like the physical types, so it sits with them, ahead of arcane. |
| **Arcane / conversion** | Cold Abyssal: ARC scaling +65, plus flat secondaries (add fire / bolt / arcane ATK +15) |
| **Blood** | Blood ATK UP % gems (can be out-of-shape, e.g. radial, for bloodtinge builds) |
| **Status** | Dirty / Murky: rapid poison / slow poison, the only two status effects, so they group together. Rank above the conditional classes below. |
| **Pulsing** | HP recovery over time. Its own type so a preset can put it first. |
| **Poorman's** | Physical / ATK UP near death (30%+ at low HP). Its own type, so PvE players can place it apart from Fool's. |
| **Fool's** | Physical / ATK UP at full HP. Its own type. |

- Gem classes come with different natural curses, so grouping must use the **type of the primary effect**, not the full effect list or its values.
- **Pulsing** is a matter of taste: some players want it at the top regardless of category, others at the bottom. The `pulsing+default` preset puts it first.
- **Presets:** the F9 menu offers `default`, `arcane` (arcane, nourishing, blood first) and `pulsing+default`, plus `custom`, which reads the `type_order` list from `bbhost.toml`. Unknown or repeated names in a list are skipped and logged; unlisted types follow in default order. New presets come from what players ask for.
- **Variants are separate groups.** A Lost or Uncanny copy has its own item id, so the game groups it apart from the normal copies (userdata0007: two Blades of Mercy, then the Lost Blade of Mercy below them). `ignore_variants = true` merges them: every copy at the same level takes the `key1` of the copy that sorts first, so the gems order all of them. The variant is the item id's 10000s digit (0 normal, 1 Uncanny, 2 Lost); the two "Bloody" weapons (digit 8) are left alone. Upgrade level still separates groups.

### 3. Within a setup type: curses rank lower

Default curse order, worst (lowest) to best:

1. ATK DOWN
2. HP drain
3. Durability DOWN
4. ATK vs beasts DOWN
5. ATK vs the kin DOWN
6. Stamina cost increase

This should be configurable in the plugin, since it reflects one player's judgement. Implemented as `curse_order`, written **best first** to match the menu (top to bottom): default `stamina,kin,beasts,durability,hp,atk`. A weapon with a worse curse sits lower. `ignore_curses = true` skips this step entirely, so strength decides (strongest first).

### 3b. Within a setup type: secondaries

For the abyssal classes (Heavy, Tempering, Cold), the **flat damage secondaries** (add physical / fire / bolt / arcane ATK +N) rank above niche secondaries such as stamina cost reduction or "ATK vs beasts UP 5%". The flat adds are what PvP builds actually use. This applies to ordering *within* a setup type, not to choosing the type.

### 4. Last resort

Identical setups fall back to a fixed, stable order (the handle, as now). The order is then fully deterministic.

## Implementation notes

- **Effect data is available.** Each gem's effect ids are read already, from the gem's GaItem. Effect ids are rows of `GemeffectParam`. bbhost exposes those rows by field name (`param_get("GemeffectParam", id, field, …)`), so effect type, stat and magnitude can be read from the game's own data rather than hard-coded.
- **Classification:** map each effect id to a category (scaling, physical %, flat add, blood %, status, conditional, curse:<kind>) using GemeffectParam fields. Hard-code only what the param data can't express, such as Poorman's vs Fool's conditions.
- **Ranking:** build a multi-part key per weapon: (fully gemmed, setup type rank, curse score, …, handle). Rank the records by it and write the rank into `key2`. This is the same mechanism as v1, so no new hooks are needed.
- **Configuration:** setup-type order, curse order and Pulsing placement as plugin options (F9 menu or `bbhost.toml`). The Elden Ring-style manual ordering, chosen in the game's own menu, is a separate, later goal.

## Open questions

- Which GemeffectParam fields identify an effect's kind (stat, conditional trigger, curse)? This needs a look at the paramdef.
- ~~How to classify a weapon whose gems span several types?~~ **Decided (2026-10-25): slot 1 decides.** See "Setup type comes from slot 1" below.
- Should "fully gemmed" also apply to left-hand weapons (pistols, single slot), or only within right-hand weapons for now?
- Where blood setups and status setups rank relative to physical and arcane by default.

## Findings: classifying gem effects (2026-10-25)

Sources:
- The save editor's `resources/upgrades.json` (Noxde/Bloodborne-save-editor, GPL-3.0): 12,489 effects with name, effect text, rating and level.
- The game's own `GemeffectParam`, exported by `inv_order` with `export_gems = true`: 16,101 rows.

**Kind: solved.** `kind = (id / 1000) % 100`, with high-tier prefixes aliased: `{56:43, 62:17, 63:24, 64:25, 65-67:26, 68:29, 69:30, 70:33, 71:34, 72:35, 73:36, 74:37, 75:38, 76:49}`. This matches all 12,488 effects with zero mismatches. Every gem on userdata0001 (arcane, 64 gems) and userdata0002 (quality, 95 gems) classifies (`tools/gem_census.py`).

**`GemeffectParam` fields:**

| field | what it is | usable for strength? |
|---|---|---|
| `categoryId` | effect kind × tier blocks (e.g. Physical ATK UP = 167100-174600) | no; same information as the kind rule |
| `sortId` | the game's own gem sort: a block per kind (Physical % = 1-200, blunt 401+, thrust 601+, …) | no; ~7% of adjacent pairs inverted overall, ~16% within an id family |
| `effectRank` | a drop/quality rank | no; ~6% inverted overall, ~16% within a family |

**Strength needs the real value.** Options:
1. Embed an (effect id → value) table generated from `upgrades.json`. This is fast and GPL-compatible (the plugin is GPL anyway, as a bbhost plugin). Credit the source.
2. Read the value from the game: `GemeffectParam.speffectId` → `SpEffectParam`. This is authoritative, but the field holding the value depends on the effect kind (37 kinds to map).

Plan: (1) now, (2) later if the community table proves wrong anywhere.

## Decision: setup type comes from slot 1

A weapon's setup type is the type of the **gem in slot 1** (its first effect). Weapons that mix types are rare and niche anyway, so this rule should rarely surprise anyone.

- **Why:** it's simple, visible, and the player controls it. Example: a Lost Chikage with two Physical ATK UP radials and one Blood ATK gem is a blood weapon if the Blood ATK gem sits in slot 1. Majority voting would bury a hard-farmed, out-of-shape Blood ATK radial under the physical gems. Players already think of slot 1 as the main gem.
- **Slot 1 empty:** use the first filled slot.
- **Slot 1's first effect fits no setup type** (e.g. a niche primary): type "misc", ranked after the main types.
- **Strength within a type:** compare slot 1's primary value first, then the remaining slots in order. The defining gem counts most for strength as well.

## Decision: presets stay in the plugin menu

Presets are chosen in the bbhost plugin menu (F9) or `bbhost.toml`, not in the game's own [T] sort cycle. They're global, not per character: a preset reflects how a player likes the menu, and the gems still decide each copy's place, so a global setting covers most players. A player with a build-specific preference switches the preset in F9; it applies live.

Adding modes to the game's [T] cycle would need its mode storage, cycle limit and labels found and patched, and the game may save the mode per character. That groundwork belongs with the Elden Ring-style manual sort, later.
