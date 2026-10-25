/* inv_order: a bbhost plugin that decides how identical items are ordered in
 * the inventory and equipment menus.
 *
 * The menu's sort pass (0x2060530, see docs/findings.md) builds one record
 * per list entry - {key1 = the sort column's value, key2 = the item's GaItem
 * handle, index} - and stable-sorts them (0x2064dd0 / 0x2064ff0) by key1,
 * then key2. key2 only matters when key1 ties: copies of the same weapon.
 * This plugin rewrites key2 just before the sort runs.
 *
 *   ties = "game"     the game's order (lowest handle first)
 *          "reverse"  ties reversed - a visible check that the hooks work
 *   dump = true       log every list entry's bytes on each sort pass, for
 *                     finding where an entry keeps its gems
 *
 * All addresses are Binary Ninja addresses of the 1.09 eboot (image base
 * 0x400000). */
#include "bbhost_plugin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FN_SORT_PASS 0x2060530ull  /* builds the records and sorts them */
#define FN_SORT_KEY 0x205e120ull   /* (out, entry, mode): key1 of one entry */
#define FN_SORT_A 0x2064dd0ull     /* stable sort of the records, one direction */
#define FN_SORT_B 0x2064ff0ull     /* ... the other */

#define ENTRY_SIZE 0xd8
#define ENTRY_REF 0x70     /* entry: GaItem reference, handle first */
#define MAX_ENTRIES 4096

typedef struct Record {
    int32_t key1;
    uint32_t key2;
    uint32_t index;
} Record;

enum { TIES_GAME, TIES_REVERSE };

static const BbHostApi* api;
static FILE* out;
static int ties = TIES_GAME;
static int dump;

/* The current sort pass: armed by FN_SORT_PASS, spent by the first (top
 * level) sort call; the entries FN_SORT_KEY saw, in index order. */
static int armed;
static uint64_t pass;
static const uint8_t* entries[MAX_ENTRIES];
static uint32_t entry_count;

static uint32_t tie_key(uint32_t handle) {
    switch (ties) {
    case TIES_REVERSE:
        return ~handle;
    default:
        return handle;
    }
}

static int on_sort_pass(BbHookCtx* ctx, void* user) {
    (void)ctx;
    (void)user;
    armed = 1;
    entry_count = 0;
    ++pass;
    return 0;
}

static int on_sort_key(BbHookCtx* ctx, void* user) {
    (void)user;
    if (!armed || !dump || entry_count >= MAX_ENTRIES) return 0;
    const uint8_t* e = (const uint8_t*)(uintptr_t)ctx->arg[1];
    /* Only the pass's own walk over its list: contiguous entries. */
    if (entry_count && e != entries[0] + (size_t)entry_count * ENTRY_SIZE) return 0;
    entries[entry_count++] = e;
    return 0;
}

static void dump_pass(const Record* r, size_t n) {
    fprintf(out, "== pass %llu: %zu records, %u entries\n", (unsigned long long)pass, n, entry_count);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t idx = r[i].index;
        fprintf(out, "rec %zu key1=%d key2=%08x index=%u\n", i, r[i].key1, r[i].key2, idx);
        if (idx >= entry_count) continue;
        const uint8_t* e = entries[idx];
        uint32_t handle;
        memcpy(&handle, e + ENTRY_REF, 4);
        if (handle >> 28 != 0x8) continue; /* weapons only */
        fprintf(out, "  entry");
        for (int b = 0; b < ENTRY_SIZE; ++b) fprintf(out, "%s%02x", (b % 16) ? ((b % 4) ? "" : " ") : "\n   ", e[b]);
        fputc('\n', out);
    }
    fflush(out);
}

static int on_sort(BbHookCtx* ctx, void* user) {
    (void)user;
    if (!armed) return 0; /* a recursive call, or a sort outside a menu pass */
    armed = 0;
    Record* first = (Record*)(uintptr_t)ctx->arg[0];
    Record* last = (Record*)(uintptr_t)ctx->arg[1];
    if (!first || last <= first) return 0;
    const size_t n = (size_t)(last - first);
    if (ties != TIES_GAME)
        for (size_t i = 0; i < n; ++i) first[i].key2 = tie_key(first[i].key2);
    if (dump && out) dump_pass(first, n);
    return 0; /* the game's sort runs, with our keys */
}

static int parse_ties(const char* v) { return v && strcmp(v, "reverse") == 0 ? TIES_REVERSE : TIES_GAME; }

static void on_option(const char* key, const char* value, void* user) {
    (void)user;
    if (strcmp(key, "ties") == 0) ties = parse_ties(value);
    if (strcmp(key, "dump") == 0) dump = value && strcmp(value, "true") == 0;
    if (out) fprintf(out, "option %s = %s\n", key, value ? value : "");
}

static int place(uint64_t at, size_t n, BbHookFn2 fn, const char* what) {
    const int rc = api->hook2(at, NULL, n, fn, NULL);
    api->log("inv_order: hook %s at %#llx: %s", what, (unsigned long long)at,
             rc == 0 ? "ok" : rc == 1 ? "prologue differs" : "cannot place");
    return rc;
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "inv_order", "Inventory order", "0.1.0", "",
    "Decides how copies of the same item are ordered in the inventory and equipment menus."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_CHOICE | BB_OPT_LIVE, "ties", "Order of identical items",
     "game: as the game does. reverse: flipped (a test that the plugin works).", "game", 0, 0, "game|reverse"},
    {BB_OPT_BOOL | BB_OPT_LIVE, "dump", "Log list entries", "writes plugins/inv_order/inv_order.log", "false", 0, 0,
     0},
    {0, 0, 0, 0, 0, 0, 0, 0}};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* host) {
    if (host->version < 9 || !host->eboot_is_109()) return 1;
    api = host;
    char path[1024];
    snprintf(path, sizeof(path), "%s/inv_order.log", api->plugin_dir("inv_order"));
    out = fopen(path, "a");
    ties = parse_ties(api->config("inv_order.ties"));
    dump = strcmp(api->config("inv_order.dump"), "true") == 0;
    if (out) fprintf(out, "== start: ties=%s dump=%d\n", ties == TIES_REVERSE ? "reverse" : "game", dump);
    /* Prologue lengths: whole instructions, none rip-relative (checked against
     * the 1.09 eboot). */
    if (place(FN_SORT_PASS, 20, on_sort_pass, "sort pass") != 0) return 1;
    if (place(FN_SORT_KEY, 18, on_sort_key, "sort key") != 0) return 1;
    if (place(FN_SORT_A, 17, on_sort, "sort a") != 0) return 1;
    if (place(FN_SORT_B, 17, on_sort, "sort b") != 0) return 1;
    api->on_option("inv_order", on_option, NULL);
    if (out) fflush(out);
    return 0;
}
