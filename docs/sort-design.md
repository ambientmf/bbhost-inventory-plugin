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
- Slot count comes from the weapon GaItem (locked slots have shape `0x80000000`).

### 2. Group by setup type

Within a group of identical weapons, cluster copies whose gems are the same *kind*:

| Setup type | Defining gems (examples) |
|---|---|
| **Heavy / strength** | Heavy Abyssal: STR scaling +65, plus tempering abyssal (Physical ATK UP ~24.8% + flat secondary like add physical ATK +15) |
| **Pure physical** | all Physical ATK UP % (tempering, 25-27.2%) |
| **Arcane / conversion** | Cold Abyssal: ARC scaling +65, plus flat secondaries (add fire / bolt / arcane ATK +15) |
| **Blood** | Blood ATK UP % gems (can be out-of-shape, e.g. radial, for bloodtinge builds) |
| **PvP status** | Dirty / Murky: fast poison / slow poison. Rank above the specialized classes below. |
| **Specialized** | Poorman's (Physical ATK UP 30%+ at low HP), Fool's (Physical ATK UP at full HP), Pulsing (HP recovery over time) |

- Gem classes come with different natural curses, so grouping must use the **type of the primary effect**, not the full effect list or its values.
- **Pulsing** is a matter of taste: some players want it at the top regardless of category, others at the bottom. Make it configurable later, after the core setup sorting works.

### 3. Within a setup type: curses rank lower

Default curse order, worst (lowest) to best:

1. ATK DOWN
2. HP drain
3. Durability DOWN
4. ATK vs beasts DOWN
5. ATK vs the kin DOWN
6. Stamina cost increase

This should be configurable in the plugin, since it reflects one player's judgement.

### 3b. Within a setup type: secondaries

For the abyssal classes (Heavy, Tempering, Cold), the **flat damage secondaries** (add physical / fire / bolt / arcane ATK +N) rank above niche secondaries such as stamina cost reduction or "ATK vs beasts UP 5%". The flat adds are what PvP builds actually use. This applies to ordering *within* a setup type, not to choosing the type.

### 4. Last resort

Identical setups fall back to a fixed, stable order (the handle, as now). The order is then fully deterministic.

## Implementation notes

- **Effect data is available.** Each gem's effect ids are read already (gem GaItem +0x20). Effect ids are rows of `GemeffectParam`. bbhost exposes those rows by field name (`param_get("GemeffectParam", id, field, …)`), so effect type, stat and magnitude can be read from the game's own data rather than hard-coded.
- **Classification:** map each effect id to a category (scaling, physical %, flat add, blood %, status, conditional, curse:<kind>) using GemeffectParam fields. Hard-code only what the param data can't express, such as Poorman's vs Fool's conditions.
- **Ranking:** build a multi-part key per weapon: (fully gemmed, setup type rank, curse score, …, handle). Rank the records by it and write the rank into `key2`. This is the same mechanism as v1, so no new hooks are needed.
- **Configuration:** setup-type order, curse order and Pulsing placement as plugin options (F9 menu or `bbhost.toml`). The Elden Ring-style manual ordering, chosen in the game's own menu, is a separate, later goal.

## Open questions

- Which GemeffectParam fields identify an effect's kind (stat, conditional trigger, curse)? This needs a look at the paramdef.
- How to classify a weapon whose gems span several types: by the slot-1 gem, by majority, or by the strongest primary?
- Should "fully gemmed" also apply to left-hand weapons (pistols, single slot), or only within right-hand weapons for now?
- Where blood setups and PvP status setups rank relative to physical and arcane by default.
