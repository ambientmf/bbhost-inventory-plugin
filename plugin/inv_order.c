/* inv_order: a bbhost plugin that decides how identical items are ordered in
 * the inventory and equipment menus.
 *
 * The menu's sort pass (0x2060530, see docs/findings.md) builds one record
 * per list entry - {key1 = the sort column's value, key2 = the item's GaItem
 * handle, index} - and stable-sorts them (0x2064dd0 / 0x2064ff0) by key1,
 * then key2. key2 only matters when key1 ties: copies of the same weapon.
 * This plugin rewrites key2 just before the sort runs.
 *
 *   ties = "gems"     copies ordered fully gemmed first, then by gem count,
 *                     then by gem loadout, then by handle
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

/* One record's tie key in gems mode, compared field by field: fully gemmed
 * first, then more gems first, then the gem signature, then the handle. */
typedef struct Tie {
    uint32_t not_full; /* 0 when every open slot holds a gem */
    uint32_t empty;    /* NUM_SLOTS - gems: fewer empty slots first */
    uint32_t sig[SIG_LEN];
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

/* A weapon's gem loadout: how many open slots hold gems, and their effect
 * ids slot by slot. An empty slot (or not a weapon) reads as all
 * 0xffffffff, which sorts after any gem; a non-weapon counts as not full. */
static void gem_signature(uint32_t handle, Tie* t) {
    memset(t->sig, 0xff, sizeof(t->sig));
    t->not_full = 1;
    t->empty = NUM_SLOTS;
    if (handle >> 28 != 0x8) return; /* weapons only */
    const uint64_t weapon = gaitem(handle);
    if (!weapon) return;
    uint32_t open = 0, gems = 0;
    for (int s = 0; s < NUM_SLOTS; ++s) {
        const uint64_t slot = weapon + WEAPON_SLOTS + s * SLOT_STRIDE;
        uint32_t shape = SLOT_LOCKED, gem_handle = 0;
        if (api->read(slot + SLOT_SHAPE, &shape, 4) != 0 || shape == SLOT_LOCKED) continue;
        ++open;
        if (api->read(slot + SLOT_GEM, &gem_handle, 4) != 0) continue;
        const uint64_t gem = gem_handle ? gaitem(gem_handle) : 0;
        if (!gem) continue;
        ++gems;
        api->read(gem + GEM_EFFECTS, &t->sig[s * NUM_EFFECTS], NUM_EFFECTS * sizeof(uint32_t));
    }
    t->not_full = !(open && gems == open);
    t->empty = NUM_SLOTS - gems;
}

static int by_tie(const void* a, const void* b) {
    const Tie* x = &tie[*(const uint32_t*)a];
    const Tie* y = &tie[*(const uint32_t*)b];
    if (x->not_full != y->not_full) return x->not_full < y->not_full ? -1 : 1;
    if (x->empty != y->empty) return x->empty < y->empty ? -1 : 1;
    for (int i = 0; i < SIG_LEN; ++i)
        if (x->sig[i] != y->sig[i]) return x->sig[i] < y->sig[i] ? -1 : 1;
    return x->handle < y->handle ? -1 : x->handle > y->handle;
}

/* gems: rank every record by (signature, handle) and make the rank its key2.
 * Only ties on key1 ever compare key2, so this orders copies of a weapon by
 * their gems and leaves everything else as the game has it. */
static void rank_by_gems(Record* r, size_t n, int descending) {
    for (size_t i = 0; i < n; ++i) {
        tie[i].handle = r[i].key2;
        tie[i].record = (uint32_t)i;
        gem_signature(r[i].key2, &tie[i]);
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
        const uint32_t handle = ties == TIES_GEMS ? tie[i].handle : r[i].key2;
        fprintf(out, "rec %zu key1=%d key2=%u handle=%08x", i, r[i].key1, r[i].key2, handle);
        if (ties == TIES_GEMS && handle >> 28 == 0x8)
            for (int s = 0; s < NUM_SLOTS; ++s) {
                if (tie[i].sig[s * NUM_EFFECTS] == 0xffffffff) continue;
                fprintf(out, " [");
                for (int e = 0; e < NUM_EFFECTS && tie[i].sig[s * NUM_EFFECTS + e] != 0xffffffff; ++e)
                    fprintf(out, "%s%u", e ? " " : "", tie[i].sig[s * NUM_EFFECTS + e]);
                fprintf(out, "]");
            }
        fputc('\n', out);
    }
    fflush(out);
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
    BB_PLUGIN_API_VERSION, 0, "inv_order", "Inventory order", "0.3.0", "",
    "Orders copies of the same weapon by their gem loadout in the inventory and equipment menus."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_CHOICE | BB_OPT_LIVE, "ties", "Order of identical weapons",
     "gems: fully gemmed first, more gems first, then grouped by gems. game: as the game does. reverse: flipped (a test).", "gems", 0, 0,
     "gems|game|reverse"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "dump", "Log sort passes", "writes plugins/inv_order/inv_order.log", "false", 0, 0,
     0},
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
