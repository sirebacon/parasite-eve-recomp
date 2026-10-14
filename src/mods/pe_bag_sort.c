/*
 * Bag auto-sort: once the player sorts with the game's own Sort screen (opened
 * from Use Item with Square, pe_catalog_menu.c), the bag stays in that order:
 * whenever it changes in free field (a pickup, items taken after a fight,
 * items back from Wayne), it is sorted again the way the game sorts.
 *
 * The game's sort (decomp item/armor_capacity.c, Inv_SortInventoryByMode):
 * qsort of Aya's 50-slot bag with Inv_CompareItemsForSort -- empty slots last,
 * then g_InvSortRankTable[kind], then g_InvSortTypeRankTable[record byte 0x0E
 * & 0xF], then item id -- and then the first run of weapons (kinds 1-8) by
 * (base + bonus)[g_InvWeaponSortStatIndex] and the first run of armor (kind 9)
 * by (base + bonus)[g_InvAmmoSortStatIndex], highest first, item id breaking
 * ties; the equipped weapon / armor slots follow their items. The four
 * settings stay in those globals after a sort (0x8009D0B8 rank table,
 * 0x8009D0BC type table, 0x8009D0A4 weapon stat, 0x8009D0A8 armor stat), so
 * a change in them while the Sort screen is up is the player's choice: it is
 * kept (saved) and auto-sort turns on.
 *
 * Manual: moving items yourself in Use Item (the bag's order changes with the
 * same items, no Sort screen) switches to Manual so your layout stays; sorting
 * again switches back. F1 row "Bag sort" shows / sets it. Wayne's storage is
 * left alone. Records: bag value 0x100 + r -> equipment record 0x800C0EAC +
 * 0x20 r; 1-0xFF -> base record; 0x200 + k -> ammo pool record 0x800A1E64 +
 * 0x20 k (Inv_LookupData). Saved as save-state block "pe.bag-sort" and in the
 * save's free tail at 0x1C90 + 0x260, under the game's CRC.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

#define PE_FN_SAVE_SERIALIZE     0x8003F800u
#define PE_FN_SAVE_DESERIALIZE   0x8003FBD8u
#define PE_FN_INV_DISPATCH       0x8005D2B4u
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_MAX_HP_ADDR           0x800C0E06u
#define PE_AYA_SLOTS_ADDR        0x800C0E48u
#define PE_SLOT_COUNT_ADDR       0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR      0x8009D018u
#define PE_WEAPON_SLOT_ADDR      0x800C0E20u   /* s8 */
#define PE_ARMOR_SLOT_ADDR       0x800C0E22u   /* s8 */
#define PE_EQUIP_RECORDS_ADDR    0x800C0EACu
#define PE_AMMO_POOLS_ADDR       0x800A1E64u
#define PE_ITEM_TABLE_BASE_ADDR  0x800A8034u
#define PE_ITEM_TABLE_END_ADDR   0x800A8038u
#define PE_ITEM_TABLE_ORIGIN     0x800A8028u
#define PE_GAME_BLOCK_ADDR       0x8009D1A0u
#define PE_FIELD_BUSY_ADDR       0x800B0CD8u
#define PE_SORT_RANK_PTR         0x8009D0B8u   /* g_InvSortRankTable */
#define PE_SORT_TYPE_PTR         0x8009D0BCu   /* g_InvSortTypeRankTable */
#define PE_SORT_WEAPON_STAT      0x8009D0A4u   /* g_InvWeaponSortStatIndex */
#define PE_SORT_ARMOR_STAT       0x8009D0A8u   /* g_InvAmmoSortStatIndex */
#define PE_RANK_TABLES_LO        0x800923F8u   /* g_InvSortRankTableA .. D, type tables A/B */
#define PE_RANK_TABLES_HI        0x80092478u
#define PE_CONTEXT_FLAGS         0x8009CEF0u   /* g_MenuItemContextFlag */
#define PE_SORT_ACTION_BIT       0x40u         /* main menu action 6: Sort */
#define PE_CARD_BUFFER           0x8009EED0u
#define PE_TAIL_OFFSET           (0x1C90u + 0x260u)
#define PE_TAIL_MAGIC            0x54524F53u   /* "SORT" */
#define PE_TAIL_VERSION          1u

#define REC_ITEM_ID  0x04u
#define REC_KIND     0x06u
#define REC_BASE     0x07u                     /* u8 base[3] */
#define REC_BYTE_0E  0x0Eu
#define REC_BONUS    0x0Eu                     /* s16 bonus[3] */

enum { PE_SORT_MANUAL, PE_SORT_AUTO };
static const char *const s_sort_names[] = { "MANUAL", "AUTO" };

#define PE_SORT_STATE_VERSION 1u
static struct {
    uint32_t mode;
    uint32_t rank_ptr, type_ptr;
    uint32_t weapon_stat, armor_stat;
    uint32_t row_hidden;                       /* the Sort row's bit is off (see below) */
} s_bs;

static int s_mode_row, s_mode_shown;           /* F1 view of s_bs.mode */
static uint16_t s_last_bag[50];                /* the bag as last sorted / seen */
static int s_last_valid;
static uint32_t s_seen_globals[4];
static int s_globals_valid;
static uint32_t s_sort_screen_until, s_frames;  /* the Sort screen was up recently */

/* pe_catalog_menu.c: the Sort screen is on screen this frame. */
void pe_bag_sort_screen_seen(void) { s_sort_screen_until = s_frames + 30u; }
int pe_bag_sort_auto(void) { return s_bs.mode == PE_SORT_AUTO; }

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_ptr_ok(uint32_t p) { return p >= PE_RANK_TABLES_LO && p < PE_RANK_TABLES_HI; }

static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

/* Inv_LookupData: the record for a bag value, or 0. */
static uint32_t pe_record(uint16_t v) {
    if (v >= 0x100u && v < 0x180u) return PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 0x100u) * 0x20u;
    if (v >= 0x200u && v < 0x209u) return PE_AMMO_POOLS_ADDR + (uint32_t)(v - 0x200u) * 0x20u;
    if (v >= 1u && v <= 0xFFu) {
        const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
        const uint32_t end = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
        if (end <= base || (uint32_t)(v - 1u) >= ((end - base) >> 5)) return 0;
        return PE_ITEM_TABLE_ORIGIN + base + (uint32_t)(v - 1u) * 0x20u;
    }
    return 0;
}

/* One slot's sort keys, read once. */
typedef struct { uint16_t v; uint8_t empty, kind, rank, sub; uint16_t id; int stat; } PeKey;
static int s_cmp_stat;

static int pe_cmp(const void *a, const void *b) {
    const PeKey *l = a, *r = b;
    if (l->empty || r->empty) return (int)l->empty - (int)r->empty;   /* empties last */
    if (l->rank != r->rank) return l->rank < r->rank ? -1 : 1;
    if (l->sub != r->sub) return l->sub < r->sub ? -1 : 1;
    if (l->id != r->id) return l->id < r->id ? -1 : 1;
    return l->v < r->v ? -1 : l->v > r->v;          /* same item: steady order */
}

static int pe_cmp_stat(const void *a, const void *b) {
    const PeKey *l = a, *r = b;
    if (l->stat != r->stat) return l->stat > r->stat ? -1 : 1;     /* highest first */
    if (l->id != r->id) return l->id < r->id ? -1 : 1;
    return l->v < r->v ? -1 : l->v > r->v;          /* same item: steady order */
}

static int pe_stat_of(uint32_t rec, uint32_t stat) {
    if (stat > 2u) stat = 0;
    return psx_mod_read_byte(rec + REC_BASE + stat) +
           (int16_t)psx_mod_read_half(rec + REC_BONUS + stat * 2u);
}

/* Sort the run of `mask` kinds (first run, as the game) by `stat`. */
static void pe_sort_run(PeKey *k, int n, uint32_t mask, uint32_t stat) {
    int s = 0;
    while (s < n && !(k[s].empty == 0 && ((mask >> k[s].kind) & 1u))) ++s;
    int e = s;
    while (e < n && k[e].empty == 0 && ((mask >> k[e].kind) & 1u)) ++e;
    if (e - s < 2) return;
    for (int i = s; i < e; ++i) k[i].stat = pe_stat_of(pe_record(k[i].v), stat);
    qsort(k + s, (size_t)(e - s), sizeof *k, pe_cmp_stat);
}

static void pe_snapshot(void) {
    for (uint32_t i = 0; i < 50u; ++i) s_last_bag[i] = psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u);
    s_last_valid = 1;
}

/* Sort the bag with the saved settings. Returns 1 if the order changed. */
static int pe_sort_bag(void) {
    if (!pe_ptr_ok(s_bs.rank_ptr) || !pe_ptr_ok(s_bs.type_ptr)) return 0;
    const uint32_t limit = pe_bag_limit();
    PeKey k[50];
    const int8_t wslot = (int8_t)psx_mod_read_byte(PE_WEAPON_SLOT_ADDR);
    const int8_t aslot = (int8_t)psx_mod_read_byte(PE_ARMOR_SLOT_ADDR);
    const uint16_t weapon = wslot >= 0 && (uint32_t)wslot < limit ?
                            psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)wslot * 2u) : 0;
    const uint16_t armor = aslot >= 0 && (uint32_t)aslot < limit ?
                           psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)aslot * 2u) : 0;
    for (uint32_t i = 0; i < limit; ++i) {
        const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u);
        const uint32_t rec = v ? pe_record(v) : 0;
        PeKey *p = &k[i];
        memset(p, 0, sizeof *p);
        p->v = v;
        p->empty = rec == 0;
        if (rec) {
            p->kind = psx_mod_read_byte(rec + REC_KIND);
            p->rank = psx_mod_read_byte(s_bs.rank_ptr + (p->kind < 0x18u ? p->kind : 0x17u));
            p->sub = psx_mod_read_byte(s_bs.type_ptr + (psx_mod_read_byte(rec + REC_BYTE_0E) & 0xFu));
            p->id = psx_mod_read_byte(rec + REC_ITEM_ID);
        }
    }
    qsort(k, limit, sizeof *k, pe_cmp);
    pe_sort_run(k, (int)limit, 0x1FEu, s_bs.weapon_stat);    /* weapons */
    pe_sort_run(k, (int)limit, 0x200u, s_bs.armor_stat);     /* armor */
    int changed = 0;
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a) != k[i].v) { psx_mod_write_half(a, k[i].v); changed = 1; }
    }
    if (!changed) return 0;
    for (uint32_t i = 0; i < limit; ++i) {                    /* equipped slots follow */
        if (weapon && k[i].v == weapon) psx_mod_write_byte(PE_WEAPON_SLOT_ADDR, (uint8_t)i);
        if (armor && k[i].v == armor) psx_mod_write_byte(PE_ARMOR_SLOT_ADDR, (uint8_t)i);
    }
    return 1;
}

static int pe_same_items(const uint16_t *a, const uint16_t *b) {
    for (int i = 0; i < 50; ++i) {
        int na = 0, nb = 0;
        for (int j = 0; j < 50; ++j) { na += a[j] == a[i]; nb += b[j] == a[i]; }
        if (na != nb) return 0;
    }
    return 1;
}

/* ---- the main menu's Sort row ----------------------------------------------- */
/* Sort moved into Use Item, so its main menu row goes: while the menu is open
 * its action bit (6) is off in g_MenuItemContextFlag -- cleared as the main
 * list is set up (pe_catalog_menu.c's row hook calls pe_bag_sort_hide_row
 * before it counts the rows), put back when the menu closes and before any
 * save is written, so saves and scripts keep the game's bit. */
static void pe_show_sort_row(void) {
    if (!s_bs.row_hidden) return;
    psx_mod_write_word(PE_CONTEXT_FLAGS, psx_mod_read_word(PE_CONTEXT_FLAGS) | PE_SORT_ACTION_BIT);
    s_bs.row_hidden = 0;
}

/* Returns 1 if the Sort row was just taken off (the row count drops by one). */
int pe_bag_sort_hide_row(void) {
    const uint32_t flags = psx_mod_read_word(PE_CONTEXT_FLAGS);
    if (!(flags & PE_SORT_ACTION_BIT)) return 0;
    psx_mod_write_word(PE_CONTEXT_FLAGS, flags & ~PE_SORT_ACTION_BIT);
    s_bs.row_hidden = 1;
    return 1;
}

static void pe_bag_sort_tick(void) {
    ++s_frames;
    if (s_mode_row != s_mode_shown) {                         /* set from F1 */
        s_bs.mode = s_mode_row ? PE_SORT_AUTO : PE_SORT_MANUAL;
        if (s_bs.mode == PE_SORT_AUTO) s_last_valid = 0;       /* sort at the next chance */
        pe_log("bag_sort", "%s (F1)", s_sort_names[s_bs.mode]);
    }
    s_mode_row = s_mode_shown = (int)s_bs.mode;
    if (!pe_loaded()) return;

    const int in_menu = (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x4u) != 0;
    if (s_bs.row_hidden && !in_menu) pe_show_sort_row();

    const uint32_t g[4] = { psx_mod_read_word(PE_SORT_RANK_PTR), psx_mod_read_word(PE_SORT_TYPE_PTR),
                            psx_mod_read_word(PE_SORT_WEAPON_STAT), psx_mod_read_word(PE_SORT_ARMOR_STAT) };
    const int globals_changed = s_globals_valid && memcmp(g, s_seen_globals, sizeof g) != 0;
    memcpy(s_seen_globals, g, sizeof g);
    s_globals_valid = 1;
    const int screen = s_frames < s_sort_screen_until;

    uint16_t now[50];
    for (uint32_t i = 0; i < 50u; ++i) now[i] = psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u);
    const int bag_changed = s_last_valid && memcmp(now, s_last_bag, sizeof now) != 0;
    if (in_menu) {
        if (screen && (globals_changed || bag_changed) && pe_ptr_ok(g[0]) && pe_ptr_ok(g[1])) {
            /* The game sorted (its Sort screen): keep its settings, go Auto. */
            const int fresh = s_bs.mode != PE_SORT_AUTO || s_bs.rank_ptr != g[0] || s_bs.type_ptr != g[1] ||
                              s_bs.weapon_stat != g[2] || s_bs.armor_stat != g[3];
            s_bs.rank_ptr = g[0]; s_bs.type_ptr = g[1];
            s_bs.weapon_stat = g[2]; s_bs.armor_stat = g[3];
            s_bs.mode = PE_SORT_AUTO;
            if (fresh)
                pe_log("bag_sort", "auto rank=0x%08X type=0x%08X weapon=%u armor=%u",
                       g[0], g[1], (unsigned)g[2], (unsigned)g[3]);
        } else if (!screen && bag_changed && s_bs.mode == PE_SORT_AUTO && pe_same_items(now, s_last_bag)) {
            /* Same items in a new order, no Sort screen: the player moved them. */
            s_bs.mode = PE_SORT_MANUAL;
            pe_log("bag_sort", "%s", "manual (items moved)");
        }
        memcpy(s_last_bag, now, sizeof now);
        s_last_valid = 1;
        s_mode_row = s_mode_shown = (int)s_bs.mode;
        return;
    }
    s_mode_row = s_mode_shown = (int)s_bs.mode;
    /* Free field only: never mid-script, battle or menu. */
    if ((psx_mod_read_half(PE_FIELD_BUSY_ADDR) & 0xB600u) || (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2006u))
        return;
    if (s_bs.mode != PE_SORT_AUTO) { memcpy(s_last_bag, now, sizeof now); s_last_valid = 1; return; }
    if (s_last_valid && memcmp(now, s_last_bag, sizeof now) == 0) return;
    pe_sort_bag();
    pe_snapshot();
}

/* ---- persistence ------------------------------------------------------------ */
static uint32_t pe_checksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0x736F7274u;
    for (uint32_t i = 0; i < n; ++i) s = (s << 5) + s + p[i];
    return s;
}

static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    pe_show_sort_row();                        /* the save keeps the game's bit */
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    const uint8_t *p = (const uint8_t *)&s_bs;
    psx_mod_write_word(at + 0u, PE_TAIL_MAGIC);
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, (uint32_t)sizeof s_bs);
    for (uint32_t i = 0; i < sizeof s_bs; ++i) psx_mod_write_byte(at + 16u + i, p[i]);
    psx_mod_write_word(at + 12u, pe_checksum(p, sizeof s_bs));
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    memset(&s_bs, 0, sizeof s_bs);
    s_last_valid = s_globals_valid = 0;
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC || psx_mod_read_word(at + 4u) != PE_TAIL_VERSION ||
        psx_mod_read_word(at + 8u) != sizeof s_bs)
        return;
    uint8_t buf[sizeof s_bs];
    for (uint32_t i = 0; i < sizeof s_bs; ++i) buf[i] = psx_mod_read_byte(at + 16u + i);
    if (pe_checksum(buf, sizeof s_bs) == psx_mod_read_word(at + 12u)) memcpy(&s_bs, buf, sizeof s_bs);
}

static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if ((int)cpu->gpr[4] == PE_INV_CMD_START_NEW_GAME) {
        memset(&s_bs, 0, sizeof s_bs);
        s_last_valid = s_globals_valid = 0;
    }
}

static void pe_bag_sort_state_loaded(int result, uint32_t saved_version, const void *saved,
                                     uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_bs, 0, sizeof s_bs);
    s_last_valid = s_globals_valid = 0;
}

/* Called from pe_mod_menu.c. */
void pe_bag_sort_register(void) {
    (void)pe_menu_register_choice(0, "Bag sort",
        "Auto keeps the Sort you chose (Square in Use Item).", &s_mode_row, s_sort_names, 2);
    (void)pe_menu_register_tick(pe_bag_sort_tick);
    (void)psx_mod_register_function_entry_plugin("pe.bag-sort.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.bag-sort.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_function_entry_plugin("pe.bag-sort.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_state("pe.bag-sort", PE_SORT_STATE_VERSION,
        &s_bs, sizeof s_bs, pe_bag_sort_state_loaded);
}
