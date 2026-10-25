/* inv_order: a bbhost plugin that decides how identical items are ordered in
 * the inventory and equipment menus.
 *
 * The menu's sort pass (0x2060530, see docs/findings.md) builds one record
 * per list entry - {key1 = the sort column's value, key2 = the item's GaItem
 * handle, index} - and stable-sorts them (0x2064dd0 / 0x2064ff0) by key1,
 * then key2. key2 only matters when key1 ties: copies of the same weapon.
 * This plugin rewrites key2 just before the sort runs.
 *
 *   ties = "gems"     copies ordered by gem setup (docs/sort-design.md):
 *                     fully gemmed, gem count, setup type of slot 1,
 *                     primary kind, worst curse, flat secondaries,
 *                     strength, then the handle
 *   type_order, curse_order   the setup type and curse rankings
 *   ignore_curses = true      skip the curse step: strongest first
 *          "game"     the game's order (lowest handle first)
 *          "reverse"  ties reversed - a visible check that the hooks work
 *   dump = true       log each sort pass's weapons and gems
 *
 * All addresses are Binary Ninja addresses of the 1.09 eboot (image base
 * 0x400000). */
#include "bbhost_plugin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gem_values.h"

#define FN_SORT_PASS 0x2060530ull /* builds the records and sorts them */
#define FN_SORT_A 0x2064dd0ull    /* stable sort of the records: key1, key2 ascending */
#define FN_SORT_B 0x2064ff0ull    /* ... descending, both keys */
#define GAITEM_MAN 0x593e990ull   /* global: the GaItem manager; items[slot] at +8 */

/* GaItem layouts (docs/findings.md, round 6). */
#define GAITEM_HANDLE 0x08
#define WEAPON_SLOTS 0x30   /* 5 gem slots, 16 bytes each */
#define SLOT_STRIDE 0x10
#define SLOT_SHAPE 0x08     /* within a slot: its shape, SLOT_LOCKED when the slot isn't open */
#define SLOT_GEM 0x0c       /* within a slot: the gem's handle, 0 when empty */
#define SLOT_LOCKED 0x80000000u
#define GEM_EFFECTS 0x20    /* gem: up to 6 effect ids, 0xffffffff when unused */

#define NUM_SLOTS 5
#define NUM_EFFECTS 6
#define SIG_LEN (NUM_SLOTS * NUM_EFFECTS)
#define MAX_RECORDS 4096

typedef struct Record {
    int32_t key1;
    uint32_t key2;
    uint32_t index;
} Record;

enum { TIES_GEMS, TIES_GAME, TIES_REVERSE };

static const BbHostApi* api;
static FILE* out;
static int ties = TIES_GEMS;
static int dump;

/* Armed by each sort pass, spent by its first (top level) sort call: the
 * merge sort calls itself on halves, and those calls are left alone. */
static int armed;
static uint64_t pass;

/* Gem effects (docs/sort-design.md). An effect id encodes its kind as
 * (id / 1000) % 100; high-tier prefixes are aliases of the base kinds. */
static int effect_kind(uint32_t id) {
    static const struct { int from, to; } alias[] = {{56, 43}, {62, 17}, {63, 24}, {64, 25}, {65, 26}, {66, 26},
                                                     {67, 26}, {68, 29}, {69, 30}, {70, 33}, {71, 34}, {72, 35},
                                                     {73, 36}, {74, 37}, {75, 38}, {76, 49}};
    const int k = (int)((id / 1000) % 100);
    if (k >= 13 && k <= 52) return k;
    for (size_t i = 0; i < sizeof(alias) / sizeof(alias[0]); ++i)
        if (alias[i].from == k) return alias[i].to;
    return -1;
}

/* |value| x 100 of an effect, from gem_values.h; 0 when unknown. */
static uint32_t effect_value(uint32_t id) {
    size_t lo = 0, hi = GEM_VALUES_COUNT;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (gem_values[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < GEM_VALUES_COUNT && gem_values[lo].id == id ? gem_values[lo].value : 0;
}

/* Setup types, in default order, and the primary kinds that make each one;
 * within a type, primaries sort in the order listed. */
enum { TYPE_ADEPT, TYPE_HEAVY, TYPE_PHYSICAL, TYPE_ARCANE, TYPE_BLOOD, TYPE_STATUS, TYPE_SPECIAL, TYPE_MISC, TYPE_COUNT };
static const char* const type_names[TYPE_COUNT] = {"adept",  "heavy",  "physical", "arcane",
                                                   "blood", "status", "special",  "misc"};
static const int type_kinds[TYPE_COUNT][8] = {
    {21, 20, -1},                     /* adept: thrust % (Adept), blunt % (Striking) - per damage type */
    {29, 30, -1},                     /* heavy: STR / SKL scaling (Heavy / Sharp abyssal) */
    {26, 27, 17, -1},                 /* physical: phys % (Tempering), ATK %, add physical */
    {32, 23, 24, 25, 14, 15, 16, -1}, /* arcane: ARC scaling, arcane / fire / bolt %, adds */
    {31, 22, 13, -1},                 /* blood: BT scaling, blood %, add blood */
    {38, 37, -1},                     /* status: rapid poison (Dirty), slow poison (Murky) */
    {43, 49, 52, 48, 51, -1},         /* special: Pulsing, Poorman's, Fool's */
    {-1},                             /* misc: anything else */
};
static int type_rank[TYPE_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7}; /* configurable: type_order */
static int ignore_curses; /* configurable: skip the curse step, so strength decides */

static void classify(int kind, int* type, int* order_in_type) {
    for (int t = 0; t < TYPE_COUNT; ++t)
        for (int i = 0; type_kinds[t][i] >= 0; ++i)
            if (type_kinds[t][i] == kind) {
                *type = t;
                *order_in_type = i;
                return;
            }
    *type = TYPE_MISC;
    *order_in_type = kind < 0 ? 99 : kind;
}

/* Curses, worst first internally (configured best first: curse_order). A
 * weapon is judged by its worst curse and sits lower the worse it is; no
 * curse ranks best. */
#define CURSE_KINDS 6
static int curse_kinds[CURSE_KINDS] = {28, 44, 47, 34, 36, 41}; /* ATK DOWN, HP drain, dur DOWN, beasts, kin, stamina */
static const char* const curse_names[CURSE_KINDS] = {"atk", "hp", "durability", "beasts", "kin", "stamina"};
static const int curse_name_kinds[CURSE_KINDS] = {28, 44, 47, 34, 36, 41};

static int curse_rank(int kind) {
    for (int i = 0; i < CURSE_KINDS; ++i)
        if (curse_kinds[i] == kind) return i;
    return -1;
}

static int is_flat_damage(int kind) { return kind >= 13 && kind <= 17; }

/* One record's tie key in gems mode, compared in this order. */
typedef struct Tie {
    uint32_t not_full;          /* 0 when every open slot holds a gem */
    uint32_t empty;             /* NUM_SLOTS - gems: fewer empty slots first */
    uint32_t type;              /* setup type's rank, from slot 1 (the first filled slot) */
    uint32_t primary;           /* the defining gem's primary kind, within its type */
    uint32_t curse;             /* CURSE_KINDS - rank of the worst curse: lower is milder */
    uint32_t flats;             /* flat-damage secondaries: more first */
    uint32_t strength[NUM_SLOTS]; /* each slot's primary value: higher first */
    uint32_t sig[SIG_LEN];      /* raw effect ids: a last, stable distinction */
    uint32_t handle;
    uint32_t record;
} Tie;
static Tie tie[MAX_RECORDS];
static uint32_t order[MAX_RECORDS];

/* The GaItem a handle names, as the game resolves it (0x1e89070): slot =
 * the handle's low 16 bits, and the item must carry the same handle. */
static uint64_t gaitem(uint32_t handle) {
    uint64_t man = 0, item = 0;
    uint32_t have = 0;
    const uint32_t slot = handle & 0xffff;
    if (!(handle & 0x800000) || slot == 0xffff) return 0;
    if (api->read(GAITEM_MAN, &man, 8) != 0 || !man) return 0;
    if (api->read(man + 8 + (uint64_t)slot * 8, &item, 8) != 0 || !item) return 0;
    if (api->read(item + GAITEM_HANDLE, &have, 4) != 0 || have != handle) return 0;
    return item;
}

/* Reads a weapon's gems and fills in its tie key. A non-weapon, or one whose
 * GaItem can't be read, gets the key of an empty weapon. */
static void gem_tie(uint32_t handle, Tie* t) {
    memset(t->sig, 0xff, sizeof(t->sig));
    memset(t->strength, 0, sizeof(t->strength));
    t->not_full = 1;
    t->empty = NUM_SLOTS;
    t->type = TYPE_COUNT;
    t->primary = 99;
    t->curse = 0;
    t->flats = 0;
    if (handle >> 28 != 0x8) return; /* weapons only */
    const uint64_t weapon = gaitem(handle);
    if (!weapon) return;
    uint32_t open = 0, gems = 0;
    int defining = -1, worst = CURSE_KINDS;
    for (int s = 0; s < NUM_SLOTS; ++s) {
        const uint64_t slot = weapon + WEAPON_SLOTS + s * SLOT_STRIDE;
        uint32_t shape = SLOT_LOCKED, gem_handle = 0;
        if (api->read(slot + SLOT_SHAPE, &shape, 4) != 0 || shape == SLOT_LOCKED) continue;
        ++open;
        if (api->read(slot + SLOT_GEM, &gem_handle, 4) != 0) continue;
        const uint64_t gem = gem_handle ? gaitem(gem_handle) : 0;
        uint32_t* fx = &t->sig[s * NUM_EFFECTS];
        if (!gem || api->read(gem + GEM_EFFECTS, fx, NUM_EFFECTS * sizeof(uint32_t)) != 0 || fx[0] == 0xffffffff)
            continue;
        ++gems;
        if (defining < 0) defining = s;
        t->strength[s] = effect_value(fx[0]);
        for (int e = 0; e < NUM_EFFECTS && fx[e] != 0xffffffff; ++e) {
            const int k = effect_kind(fx[e]);
            const int c = curse_rank(k);
            if (c >= 0 && c < worst) worst = c;
            if (e > 0 && is_flat_damage(k)) ++t->flats;
        }
    }
    t->not_full = !(open && gems == open);
    t->empty = NUM_SLOTS - gems;
    t->curse = (uint32_t)(CURSE_KINDS - worst);
    if (defining >= 0) {
        int type, in_type;
        classify(effect_kind(t->sig[defining * NUM_EFFECTS]), &type, &in_type);
        t->type = (uint32_t)type_rank[type];
        t->primary = (uint32_t)in_type;
    }
}

#define CMP(a, b) \
    if ((a) != (b)) return (a) < (b) ? -1 : 1
static int by_tie(const void* a, const void* b) {
    const Tie* x = &tie[*(const uint32_t*)a];
    const Tie* y = &tie[*(const uint32_t*)b];
    CMP(x->not_full, y->not_full);
    CMP(x->empty, y->empty);
    CMP(x->type, y->type);
    CMP(x->primary, y->primary);
    if (!ignore_curses) CMP(x->curse, y->curse);
    CMP(y->flats, x->flats); /* more first */
    for (int s = 0; s < NUM_SLOTS; ++s) CMP(y->strength[s], x->strength[s]); /* stronger first */
    for (int i = 0; i < SIG_LEN; ++i) CMP(x->sig[i], y->sig[i]);
    CMP(x->handle, y->handle);
    return 0;
}

/* gems: rank every record by its tie key and make the rank its key2. Only
 * ties on key1 ever compare key2, so this orders copies of a weapon by
 * their gems and leaves everything else as the game has it. */
static void rank_by_gems(Record* r, size_t n, int descending) {
    for (size_t i = 0; i < n; ++i) {
        tie[i].handle = r[i].key2;
        tie[i].record = (uint32_t)i;
        gem_tie(r[i].key2, &tie[i]);
        order[i] = (uint32_t)i;
    }
    qsort(order, n, sizeof(order[0]), by_tie);
    /* The descending sort compares key2 descending too: hand it the ranks
     * inverted, so a loadout keeps its place whichever way the list runs. */
    for (size_t rank = 0; rank < n; ++rank)
        r[order[rank]].key2 = (uint32_t)(descending ? n - 1 - rank : rank);
}

static void dump_pass(const Record* r, size_t n) {
    fprintf(out, "== pass %llu: %zu records, ties=%d\n", (unsigned long long)pass, n, ties);
    for (size_t i = 0; i < n; ++i) {
        const Tie* t = &tie[i];
        const uint32_t handle = ties == TIES_GEMS ? t->handle : r[i].key2;
        fprintf(out, "rec %zu key1=%d key2=%u handle=%08x", i, r[i].key1, r[i].key2, handle);
        if (ties == TIES_GEMS && handle >> 28 == 0x8) {
            fprintf(out, " full=%u gems=%u type=%u primary=%u curse=%u flats=%u str=", !t->not_full,
                    NUM_SLOTS - t->empty, t->type, t->primary, t->curse, t->flats);
            for (int s = 0; s < NUM_SLOTS; ++s) fprintf(out, "%s%u", s ? "/" : "", t->strength[s]);
            for (int s = 0; s < NUM_SLOTS; ++s) {
                if (t->sig[s * NUM_EFFECTS] == 0xffffffff) continue;
                fprintf(out, " [");
                for (int e = 0; e < NUM_EFFECTS && t->sig[s * NUM_EFFECTS + e] != 0xffffffff; ++e)
                    fprintf(out, "%s%u", e ? " " : "", t->sig[s * NUM_EFFECTS + e]);
                fprintf(out, "]");
            }
        }
        fputc('\n', out);
    }
    fflush(out);
}

/* "adept,heavy,physical,..." -> type_rank; unnamed types keep their order
 * after the named ones. */
static void parse_type_order(const char* v) {
    int next = 0, seen[TYPE_COUNT] = {0};
    for (const char* p = v ? v : ""; *p;) {
        while (*p == ',' || *p == ' ') ++p;
        size_t len = strcspn(p, ", ");
        for (int t = 0; t < TYPE_COUNT; ++t)
            if (len && strlen(type_names[t]) == len && strncmp(p, type_names[t], len) == 0 && !seen[t]) {
                type_rank[t] = next++;
                seen[t] = 1;
            }
        p += len;
    }
    for (int t = 0; t < TYPE_COUNT; ++t)
        if (!seen[t]) type_rank[t] = next++;
}

/* "stamina,kin,beasts,durability,hp,atk": best first, as the menu reads (a
 * weapon with a worse curse sits lower) -> curse_kinds, which is worst first. */
static void parse_curse_order(const char* v) {
    int kinds[CURSE_KINDS], n = 0;
    for (const char* p = v ? v : ""; *p && n < CURSE_KINDS;) {
        while (*p == ',' || *p == ' ') ++p;
        size_t len = strcspn(p, ", ");
        for (int c = 0; c < CURSE_KINDS; ++c)
            if (len && strlen(curse_names[c]) == len && strncmp(p, curse_names[c], len) == 0) {
                int dup = 0;
                for (int i = 0; i < n; ++i) dup |= kinds[i] == curse_name_kinds[c];
                if (!dup) kinds[n++] = curse_name_kinds[c];
            }
        p += len;
    }
    if (n != CURSE_KINDS) return; /* incomplete: keep the default */
    for (int i = 0; i < CURSE_KINDS; ++i) curse_kinds[i] = kinds[CURSE_KINDS - 1 - i];
}

static int on_sort_pass(BbHookCtx* ctx, void* user) {
    (void)ctx;
    (void)user;
    armed = 1;
    ++pass;
    return 0;
}

static int on_sort(BbHookCtx* ctx, void* user) {
    const int descending = user != NULL; /* FN_SORT_B */
    if (!armed) return 0; /* a recursive call, or a sort outside a menu pass */
    armed = 0;
    Record* first = (Record*)(uintptr_t)ctx->arg[0];
    Record* last = (Record*)(uintptr_t)ctx->arg[1];
    if (!first || last <= first || (size_t)(last - first) > MAX_RECORDS) return 0;
    const size_t n = (size_t)(last - first);
    switch (ties) {
    case TIES_GEMS:
        rank_by_gems(first, n, descending);
        break;
    case TIES_REVERSE:
        for (size_t i = 0; i < n; ++i) first[i].key2 = ~first[i].key2;
        break;
    default:
        break;
    }
    if (dump && out) dump_pass(first, n);
    return 0; /* the game's sort runs, with our keys */
}

/* export_gems: write every GemeffectParam row to gem_effects.csv once the
 * params have loaded, for working out effect categories and strengths
 * offline (docs/sort-design.md). Rows are GEMEFFECT_PARAM_ST
 * (include/bbhost/params.hpp). */
static int export_pending;

static void export_gem_table(void) {
    size_t count = 0;
    if (api->param_ids("GemeffectParam", NULL, 0, &count) != 0 || !count) return; /* not loaded yet */
    export_pending = 0;
    uint32_t* ids = (uint32_t*)malloc(count * sizeof(uint32_t));
    if (!ids) return;
    api->param_ids("GemeffectParam", ids, count, &count);
    char path[1024];
    snprintf(path, sizeof(path), "%s/gem_effects.csv", api->plugin_dir("inv_order"));
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "id,speffectId,categoryId,effectRank,rankMin,rankMax,disposalPrice,gemIconIdOffset,sortId,"
                   "sortCategory,gemNameIdOffset,speffectId_forAtk\n");
        for (size_t i = 0; i < count; ++i) {
            size_t bytes = 0;
            const uint8_t* row = (const uint8_t*)api->param_row("GemeffectParam", ids[i], &bytes);
            if (!row || bytes < 36) continue;
            int32_t v[6], atk;
            uint16_t icon, sort;
            uint32_t name;
            memcpy(v, row, sizeof(v));
            memcpy(&icon, row + 24, 2);
            memcpy(&sort, row + 26, 2);
            memcpy(&name, row + 28, 4);
            memcpy(&atk, row + 32, 4);
            fprintf(f, "%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d\n", ids[i], v[0], v[1], v[2], v[3], v[4], v[5], icon,
                    sort & 0x3fff, sort >> 14, name, atk);
        }
        fclose(f);
    }
    api->log("inv_order: exported %zu GemeffectParam rows to %s", count, path);
    free(ids);
}

static void on_frame(void* user) {
    (void)user;
    if (export_pending) export_gem_table();
}

static int parse_ties(const char* v) {
    if (v && strcmp(v, "game") == 0) return TIES_GAME;
    if (v && strcmp(v, "reverse") == 0) return TIES_REVERSE;
    return TIES_GEMS;
}

static void on_option(const char* key, const char* value, void* user) {
    (void)user;
    if (strcmp(key, "ties") == 0) ties = parse_ties(value);
    if (strcmp(key, "dump") == 0) dump = value && strcmp(value, "true") == 0;
    if (strcmp(key, "type_order") == 0) parse_type_order(value);
    if (strcmp(key, "ignore_curses") == 0) ignore_curses = value && strcmp(value, "true") == 0;
    if (strcmp(key, "curse_order") == 0) parse_curse_order(value);
    if (strcmp(key, "export_gems") == 0) export_pending = value && strcmp(value, "true") == 0;
    if (out) fprintf(out, "option %s = %s\n", key, value ? value : "");
}

static int place(uint64_t at, size_t n, BbHookFn2 fn, void* user, const char* what) {
    const int rc = api->hook2(at, NULL, n, fn, user);
    api->log("inv_order: hook %s at %#llx: %s", what, (unsigned long long)at,
             rc == 0 ? "ok" : rc == 1 ? "prologue differs" : "cannot place");
    return rc;
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "inv_order", "Inventory order", "0.4.0", "",
    "Orders copies of the same weapon by their gem loadout in the inventory and equipment menus."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_CHOICE | BB_OPT_LIVE, "ties", "Order of identical weapons",
     "gems: by gem setup (fully gemmed, type of slot 1, curses, strength). game: as the game does. reverse: flipped (a test).", "gems", 0, 0,
     "gems|game|reverse"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "dump", "Log sort passes", "writes plugins/inv_order/inv_order.log", "false", 0, 0,
     0},
    {BB_OPT_TEXT | BB_OPT_LIVE, "type_order", "Setup type order",
     "comma list of adept, heavy, physical, arcane, blood, status, special, misc",
     "adept,heavy,physical,arcane,blood,status,special,misc", 0, 0, 0},
    {BB_OPT_TEXT | BB_OPT_LIVE, "curse_order", "Curse order, best first",
     "top of the menu first: comma list of stamina, kin, beasts, durability, hp, atk",
     "stamina,kin,beasts,durability,hp,atk", 0, 0, 0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "ignore_curses", "Ignore curses",
     "on: curses don't affect the order, so the strongest copy comes first", "false", 0, 0, 0},
    {BB_OPT_BOOL, "export_gems", "Export the gem effect table", "writes plugins/inv_order/gem_effects.csv once at start",
     "false", 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0}};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* host) {
    if (host->version < 9 || !host->eboot_is_109()) return 1;
    api = host;
    char path[1024];
    snprintf(path, sizeof(path), "%s/inv_order.log", api->plugin_dir("inv_order"));
    out = fopen(path, "a");
    ties = parse_ties(api->config("inv_order.ties"));
    dump = strcmp(api->config("inv_order.dump"), "true") == 0;
    export_pending = strcmp(api->config("inv_order.export_gems"), "true") == 0;
    parse_type_order(api->config("inv_order.type_order"));
    parse_curse_order(api->config("inv_order.curse_order"));
    ignore_curses = strcmp(api->config("inv_order.ignore_curses"), "true") == 0;
    if (out) fprintf(out, "== start: ties=%d dump=%d\n", ties, dump);
    /* Prologue lengths: whole instructions, none rip-relative (checked against
     * the 1.09 eboot). */
    if (place(FN_SORT_PASS, 20, on_sort_pass, NULL, "sort pass") != 0) return 1;
    if (place(FN_SORT_A, 17, on_sort, NULL, "sort ascending") != 0) return 1;
    if (place(FN_SORT_B, 17, on_sort, (void*)1, "sort descending") != 0) return 1;
    api->on_option("inv_order", on_option, NULL);
    api->on_frame(on_frame, NULL);
    if (out) fflush(out);
    return 0;
}
