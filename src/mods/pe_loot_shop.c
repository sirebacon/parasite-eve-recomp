/*
 * Loot overhaul, phase 4: Requisition, the Junk shop, and the soft-lock rescue
 * (bridge findings pe-loot-overhaul-design-2026-10-11.md, decisions 14, 36,
 * 37, 40, 43; checklist phase 4). F1 > PE Enhancements > Workshop >
 *
 *   Requisition  a board of goods: medicine, cures, bullets, rockets and
 *                Tools. Accept (or Buy) buys one. Tools cost 40 Junk, +10 per
 *                Tool bought, never more than 100 (decision 14); the count is
 *                saved. Not sold (decisions 14, 33, 43): Super Tools, guns,
 *                armor, stat boosters, Revive, Full Recover, Maeda's bullets.
 *   Spare Club   decision 36: when Aya can't attack (no melee weapon, and no
 *                gun with rounds loaded or in its ammo pool), a free Club 1,
 *                and Bullets x15 cost 0 until she has ammo again. A toast
 *                points here when it happens.
 *
 * Goods are added the way a pickup is (decomp Inv_AddItem): a consumable takes
 * a free bag slot (value = item id); an ammo item adds its rounds to one of
 * three pools at 0x800A1E64 + 0x20 * (item kind - 16) (bullets, rockets,
 * Maeda's), up to the pool's capacity (+9 base + +0x12 bonus, at most 999),
 * and the pool needs a bag slot (value 0x200 + pool) if it has none yet.
 *
 * Saved as save-state block "pe.loot.shop" and in the save's free tail at
 * 0x1C90 + 0x1C0 (after the Mod Catalog's block), under the game's CRC.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"

#define PE_FN_INV_DISPATCH      0x8005D2B4u
#define PE_FN_SAVE_SERIALIZE    0x8003F800u
#define PE_FN_SAVE_DESERIALIZE  0x8003FBD8u
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_MAX_HP_ADDR          0x800C0E06u
#define PE_SLOT_COUNT_ADDR      0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR     0x8009D018u
#define PE_AYA_SLOTS_ADDR       0x800C0E48u
#define PE_EQUIP_RECORDS_ADDR   0x800C0EACu
#define PE_EQUIP_RECORD_COUNT   128u
#define PE_AMMO_POOLS_ADDR      0x800A1E64u   /* D_800A1E64[3] */
#define PE_ITEM_TABLE_BASE_ADDR 0x800A8034u   /* Item_LookupBaseData */
#define PE_ITEM_TABLE_END_ADDR  0x800A8038u
#define PE_ITEM_TABLE_ORIGIN    0x800A8028u
#define PE_GAME_BLOCK_ADDR      0x8009D1A0u
#define PE_FIELD_BUSY_ADDR      0x800B0CD8u

#define REC_SIZE      0x20u
#define REC_ICON      0x00u
#define REC_KIND      0x06u
#define REC_CAPACITY  0x09u                   /* baseStats[2]: bullets */
#define REC_AMMO      0x0Au
#define REC_BONUS_CAP 0x12u                   /* bonusStats[2] */

#define PE_ITEM_BULLETS_15 0x02u
#define PE_ITEM_TOOL       0x16u
#define PE_ITEM_CLUB_1     0x3Fu

#define PE_CARD_BUFFER   0x8009EED0u
#define PE_TAIL_OFFSET   (0x1C90u + 0x1C0u)
#define PE_TAIL_MAGIC    0x48534550u          /* "PESH" */
#define PE_TAIL_VERSION  1u

/* ---- goods ---------------------------------------------------------------- */
typedef struct {
    uint8_t id;
    const char *tile;                         /* 7 characters fit a tile */
    const char *name;
    int price;                                /* Junk at 100%; Tools priced apart */
} PeGood;

static const PeGood s_goods[] = {
    { 0x06, "MED 1",   "MEDICINE 1",   6 },
    { 0x07, "MED 2",   "MEDICINE 2",   12 },
    { 0x08, "MED 3",   "MEDICINE 3",   25 },
    { 0x09, "MED 4",   "MEDICINE 4",   50 },
    { 0x0D, "CURE-P",  "CURE-P",       5 },
    { 0x0E, "CURE-D",  "CURE-D",       5 },
    { 0x0F, "CURE-C",  "CURE-C",       5 },
    { 0x10, "CURE-M",  "CURE-M",       5 },
    { 0x11, "FULLCUR", "FULL CURE",    15 },
    { 0x02, "AMMO 15", "BULLETS X15",  5 },
    { 0x03, "AMMO 30", "BULLETS X30",  9 },
    { 0x05, "ROCKET",  "ROCKET",       20 },
    { PE_ITEM_TOOL, "TOOL", "TOOL",    0 },
};
#define PE_GOODS ((int)(sizeof s_goods / sizeof s_goods[0]))

/* Tunable (Debug > Loot tuning). */
static int s_price_pct = 100;
static int s_tool_start = 40, s_tool_step = 10, s_tool_cap = 100;

#define PE_SHOP_STATE_VERSION 1u
static struct {
    uint16_t tools_bought;
    uint16_t pad;
} s_shop;

/* ---- game access ------------------------------------------------------------ */
static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_free_field(void) {
    return !(psx_mod_read_half(PE_FIELD_BUSY_ADDR) & 0xB600u) &&
           !(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2006u);
}

static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

static uint16_t pe_bag(uint32_t i) { return psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u); }

static int pe_bag_find(uint16_t value) {
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i)
        if (pe_bag(i) == value) return (int)i;
    return -1;
}

static int pe_bag_count(uint16_t value) {
    int n = 0;
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i) n += pe_bag(i) == value;
    return n;
}

/* Base record of item id (Item_LookupBaseData), or 0. */
static uint32_t pe_base_record(uint8_t id) {
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
    const uint32_t end = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
    const uint32_t index = (uint32_t)id - 1u;
    if (!id || end <= base || index >= ((end - base) >> 5)) return 0;
    return PE_ITEM_TABLE_ORIGIN + base + index * REC_SIZE;
}

/* Ammo pool an ammo item fills (0 bullets, 1 rockets, 2 Maeda's), or -1. */
static int pe_ammo_pool_of_item(uint8_t id) {
    const uint32_t rec = pe_base_record(id);
    const int pool = rec ? (int)psx_mod_read_byte(rec + REC_KIND) - 16 : -1;
    return pool >= 0 && pool < 3 ? pool : -1;
}

static uint32_t pe_pool_record(int pool) { return PE_AMMO_POOLS_ADDR + (uint32_t)pool * REC_SIZE; }

static int pe_pool_rounds(int pool) { return psx_mod_read_half(pe_pool_record(pool) + REC_AMMO); }

static int pe_pool_capacity(int pool) {
    const uint32_t rec = pe_pool_record(pool);
    const int cap = psx_mod_read_byte(rec + REC_CAPACITY) +
                    (int16_t)psx_mod_read_half(rec + REC_BONUS_CAP);
    return cap < 1000 ? cap : 999;
}

/* Gun kinds 1-5 fire bullets, 6 rockets, 7 Maeda's (decomp ammo lookup). */
static int pe_gun_pool(int kind) {
    if (kind >= 1 && kind <= 5) return 0;
    if (kind == 6 || kind == 7) return kind - 5;
    return -1;
}

/* Decision 36: Aya can't attack with anything she carries. */
static int pe_cannot_attack(void) {
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i) {
        const uint16_t v = pe_bag(i);
        if (v < 256 || v >= 256 + PE_EQUIP_RECORD_COUNT) continue;
        const uint32_t rec = PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * REC_SIZE;
        const int kind = psx_mod_read_byte(rec + REC_KIND);
        if (kind == 8) return 0;              /* melee */
        const int pool = pe_gun_pool(kind);
        if (pool >= 0 && (psx_mod_read_half(rec + REC_AMMO) || pe_pool_rounds(pool))) return 0;
    }
    return 1;
}

/* ---- prices ----------------------------------------------------------------- */
static uint32_t pe_tool_price(void) {
    uint32_t p = (uint32_t)s_tool_start + (uint32_t)s_tool_step * s_shop.tools_bought;
    if (p > (uint32_t)s_tool_cap) p = (uint32_t)s_tool_cap;   /* decision 14 cap */
    return p;
}

static uint32_t pe_price(const PeGood *g) {
    if (g->id == PE_ITEM_TOOL) return pe_tool_price();
    if (g->id == PE_ITEM_BULLETS_15 && pe_loaded() && pe_cannot_attack()) return 0;  /* 36 */
    uint32_t p = ((uint32_t)g->price * (uint32_t)s_price_pct + 50u) / 100u;
    return p ? p : 1u;
}

/* Why one of g can't be bought now, or 0. */
static const char *pe_blocked(const PeGood *g) {
    const int pool = pe_ammo_pool_of_item(g->id);
    if (pool >= 0) {
        if (pe_pool_rounds(pool) >= pe_pool_capacity(pool)) return "Ammo is full";
        if (pe_bag_find((uint16_t)(0x200 + pool)) < 0 && pe_bag_find(0) < 0) return "Bag full";
    } else if (pe_bag_find(0) < 0) {
        return "Bag full";
    }
    if (pe_loot_junk() < pe_price(g)) return "Not enough Junk";
    return 0;
}

/* ---- the board -------------------------------------------------------------- */
static int s_grid;
static char s_result[64] = "-";
static char s_sel_text[64];
static int s_text_dummy[2];

static void pe_toast(const char *text) {
    psx_mod_osd_message(text);
    snprintf(s_result, sizeof s_result, "%s", text);
}

static unsigned pe_goods_count(void) {
    if (s_grid < 0 || s_grid >= PE_GOODS) s_grid = 0;
    return (unsigned)PE_GOODS;
}

static int pe_goods_cell(int index, char *label, unsigned size, unsigned *flags) {
    if (index < 0) {                          /* header: free bag slots */
        if (!pe_loaded()) return 0;
        const uint32_t limit = pe_bag_limit();
        int used = 0;
        for (uint32_t i = 0; i < limit; ++i) used += pe_bag(i) != 0;
        snprintf(label, size, "BAG %d/%u", used, (unsigned)limit);
        return 1;
    }
    if (index >= PE_GOODS) return 0;
    const PeGood *g = &s_goods[index];
    snprintf(label, size, "%s", g->tile);
    unsigned f = 0;
    if (!pe_loaded() || pe_blocked(g)) f |= PE_CELL_DIM;
    if (g->id == PE_ITEM_TOOL) f |= PE_CELL_HIGHLIGHT;
    if (pe_loaded() && pe_price(g) == 0) f |= PE_CELL_MARK;   /* free: rescue */
    *flags = f;
    return 1;
}

static unsigned pe_one(void) { return 1; }

static const char *pe_sel_name(unsigned i) {
    (void)i;
    const PeGood *g = &s_goods[s_grid >= 0 && s_grid < PE_GOODS ? s_grid : 0];
    if (!pe_loaded()) { snprintf(s_sel_text, sizeof s_sel_text, "%s", g->name); return s_sel_text; }
    const int pool = pe_ammo_pool_of_item(g->id);
    const uint32_t price = pe_price(g);
    if (pool >= 0)
        snprintf(s_sel_text, sizeof s_sel_text, "%s: %u (HAVE %d/%d)", g->name, (unsigned)price,
                 pe_pool_rounds(pool), pe_pool_capacity(pool));
    else
        snprintf(s_sel_text, sizeof s_sel_text, "%s: %u (HAVE %d)", g->name, (unsigned)price,
                 pe_bag_count(g->id));
    return s_sel_text;
}

static const char *pe_result_name(unsigned i) { (void)i; return s_result; }

static void pe_buy(void) {
    char msg[64];
    if (!pe_loaded()) { pe_toast("Load a game first"); return; }
    if (!pe_loot_enabled()) { pe_toast("Junk economy is off (Workshop)"); return; }
    pe_goods_count();
    const PeGood *g = &s_goods[s_grid];
    const char *why = pe_blocked(g);
    if (why) { pe_toast(why); return; }
    const uint32_t price = pe_price(g);
    const int pool = pe_ammo_pool_of_item(g->id);
    if (pool >= 0) {
        const uint32_t item = pe_base_record(g->id), rec = pe_pool_record(pool);
        if (pe_bag_find((uint16_t)(0x200 + pool)) < 0)
            psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)pe_bag_find(0) * 2u,
                               (uint16_t)(0x200 + pool));
        int rounds = pe_pool_rounds(pool) + psx_mod_read_half(item + REC_AMMO);
        if (rounds > pe_pool_capacity(pool)) rounds = pe_pool_capacity(pool);
        psx_mod_write_half(rec + REC_AMMO, (uint16_t)rounds);
    } else {
        psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)pe_bag_find(0) * 2u, g->id);
    }
    if (price) pe_loot_spend_junk(price);
    if (g->id == PE_ITEM_TOOL && s_shop.tools_bought < 0xFFFFu) ++s_shop.tools_bought;
    snprintf(msg, sizeof msg, "Bought %s: -%u Junk (total %u)", g->name, (unsigned)price,
             (unsigned)pe_loot_junk());
    pe_toast(msg);
}

/* ---- rescue (decision 36) --------------------------------------------------- */
static void pe_spare_club(void) {
    if (!pe_loaded()) { pe_toast("Load a game first"); return; }
    if (!pe_cannot_attack()) { pe_toast("Aya can still fight: no spare Club"); return; }
    const int slot = pe_bag_find(0);
    if (slot < 0) { pe_toast("Bag full"); return; }
    uint32_t rec_index = PE_EQUIP_RECORD_COUNT;
    for (uint32_t i = 0; i < PE_EQUIP_RECORD_COUNT; ++i)
        if (!psx_mod_read_byte(PE_EQUIP_RECORDS_ADDR + i * REC_SIZE + REC_ICON)) { rec_index = i; break; }
    const uint32_t src = pe_base_record(PE_ITEM_CLUB_1);
    if (rec_index >= PE_EQUIP_RECORD_COUNT || !src) { pe_toast("No free equipment record"); return; }
    const uint32_t dst = PE_EQUIP_RECORDS_ADDR + rec_index * REC_SIZE;
    for (uint32_t o = 0; o < REC_SIZE; o += 4) psx_mod_write_word(dst + o, psx_mod_read_word(src + o));
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, (uint16_t)(rec_index + 256u));
    pe_toast("Spare Club issued at the Workshop");
}

/* Point at the rescue once per episode of being unable to attack. */
static void pe_shop_tick(void) {
    static unsigned frames;
    static int warned;
    if (++frames % 60u || !pe_loaded() || !pe_loot_enabled() || !pe_free_field()) return;
    if (!pe_cannot_attack()) { warned = 0; return; }
    if (warned) return;
    warned = 1;
    psx_mod_osd_message("Aya can't attack: spare Club in F1 > Workshop > Requisition");
}

/* ---- debug ------------------------------------------------------------------ */
static int pe_get_tools_bought(void) { return s_shop.tools_bought; }
static int pe_set_tools_bought(int v) { s_shop.tools_bought = (uint16_t)v; return 1; }
static int pe_get_junk(void) { return (int)pe_loot_junk(); }
static int pe_readonly(int v) { (void)v; return 0; }

/* ---- persistence ------------------------------------------------------------ */
static uint32_t pe_checksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0x5A17C0DEu;
    for (uint32_t i = 0; i < n; ++i) s = (s << 5) + s + p[i];
    return s;
}

static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    const uint8_t *p = (const uint8_t *)&s_shop;
    psx_mod_write_word(at + 0u, PE_TAIL_MAGIC);
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, (uint32_t)sizeof s_shop);
    for (uint32_t i = 0; i < sizeof s_shop; ++i) psx_mod_write_byte(at + 16u + i, p[i]);
    psx_mod_write_word(at + 12u, pe_checksum(p, sizeof s_shop));
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    memset(&s_shop, 0, sizeof s_shop);
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC || psx_mod_read_word(at + 4u) != PE_TAIL_VERSION ||
        psx_mod_read_word(at + 8u) != sizeof s_shop)
        return;
    uint8_t buf[sizeof s_shop];
    for (uint32_t i = 0; i < sizeof s_shop; ++i) buf[i] = psx_mod_read_byte(at + 16u + i);
    if (pe_checksum(buf, sizeof s_shop) == psx_mod_read_word(at + 12u)) memcpy(&s_shop, buf, sizeof s_shop);
}

static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if ((int)cpu->gpr[4] == PE_INV_CMD_START_NEW_GAME) memset(&s_shop, 0, sizeof s_shop);
}

static void pe_shop_state_loaded(int result, uint32_t saved_version, const void *saved,
                                 uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_shop, 0, sizeof s_shop);
}

/* ---- menu ------------------------------------------------------------------- */
#define PE_SHOP_PAGE   "Workshop/Requisition"
#define PE_TUNING_PAGE "Debug/Loot tuning"

/* Called from pe_mod_menu.c after pe_loot_catalog_menu_register. */
void pe_loot_shop_menu_register(void) {
    (void)pe_menu_register_live_int(PE_SHOP_PAGE, "Your Junk",
        "Junk you have now.", 0, 99999, 1, pe_get_junk, pe_readonly);
    (void)pe_menu_register_grid(PE_SHOP_PAGE, "Goods",
        "Grey: can't buy now. Mark: free.", &s_grid, PE_GOODS, 5,
        pe_goods_count, pe_goods_cell, pe_buy, 0);
    (void)pe_menu_register_dynamic_choice(PE_SHOP_PAGE, "Selected",
        "Price in Junk, and what you carry.", &s_text_dummy[0], 1, pe_one, pe_sel_name);
    (void)pe_menu_register_action(PE_SHOP_PAGE, "Buy",
        "Buy one of the selected goods.", pe_buy);
    (void)pe_menu_register_action(PE_SHOP_PAGE, "Spare Club",
        "Free Club 1 if Aya can't attack.", pe_spare_club);
    (void)pe_menu_register_dynamic_choice(PE_SHOP_PAGE, "Result",
        "What the last action did.", &s_text_dummy[1], 1, pe_one, pe_result_name);

    (void)pe_menu_register_int(PE_TUNING_PAGE, "Shop price %",
        "Requisition prices (not Tools).", &s_price_pct, 10, 300, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Tool price start",
        "First Tool's price.", &s_tool_start, 0, 100, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Tool price step",
        "Added per Tool bought.", &s_tool_step, 0, 50, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Tool price cap",
        "Most a Tool costs (decision: 100).", &s_tool_cap, 10, 100, 5);
    (void)pe_menu_register_live_int(PE_TUNING_PAGE, "Tools bought",
        "Debug: sets the next Tool's price.", 0, 99, 1, pe_get_tools_bought, pe_set_tools_bought);

    (void)pe_menu_register_tick(pe_shop_tick);
    (void)psx_mod_register_function_entry_plugin("pe.loot-shop.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot-shop.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot-shop.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_state("pe.loot.shop", PE_SHOP_STATE_VERSION,
        &s_shop, sizeof s_shop, pe_shop_state_loaded);
}
