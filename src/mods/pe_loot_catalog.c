/*
 * Loot overhaul, phase 3: the Mod Catalog (bridge findings
 * pe-loot-overhaul-design-2026-10-11.md, decisions 9-12, 25, 26, 36, 37;
 * checklist phase 3). F1 > PE Enhancements > Workshop >
 *
 *   Dismantle    pick a carried gun or armor (never the equipped ones, never
 *                the last Club) and press Dismantle twice: its effects are
 *                unlocked in the Mod Catalog, it pays Junk (5 + 2 per + bonus
 *                + 3 per mod, plus 5 per effect already unlocked, at most 150)
 *                and it goes on the Rebuild list.
 *   Mod Catalog  install an unlocked effect into a slot of the equipped gun or
 *                armor for Junk (common / mid / premium price; premium also
 *                costs a Tool unless a Tool Kit is carried), or remove one
 *                (it stays unlocked). The game's group rule holds: one mod per
 *                group, so a same-group mod elsewhere on the item is replaced.
 *                Effects seen on carried gear but not unlocked are listed as
 *                LOCKED ("dismantle to unlock").
 *   Rebuild      buy a dismantled item back: same item, same + bonuses and mod
 *                slots, empty slots; price = payout x 1.5, at most 150.
 *
 * Records (decomp ItemDataRecord, as pe_item_editor.c): bag slot value
 * r + 256 -> record 0x800C0EAC + 0x20 r; +0 icon (0 = free record), +4 item
 * id, +6 kind (1-8 weapons, 8 = clubs, 9 = armor), +0x0E s16 bonus[3], +0x14
 * mod slot count, +0x15 mod bytes (low 5 bits effect, high 3 bits group).
 * Bag-capacity armor mods (8-10) are never removed or replaced here, since a
 * smaller bag would strand items.
 *
 * Saved as save-state block "pe.loot.catalog" and in the save's free tail at
 * 0x1C90 + 0x40 (after pe_loot.c's block), under the game's CRC.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_item_table.h"
#include "pe_loot.h"

#define PE_FN_INV_DISPATCH      0x8005D2B4u
#define PE_FN_SAVE_SERIALIZE    0x8003F800u
#define PE_FN_SAVE_DESERIALIZE  0x8003FBD8u
#define PE_FN_TOOL_TRANSFER     0x8005A318u   /* Inv_TransferItemBetweenLists */
#define PE_SEL_INDEX_ADDR       0x8009D090u   /* g_InvSavedSelectionIndex[2] */
#define PE_SEL_STORAGE_ADDR     0x8009D098u   /* g_InvSavedSelectionFromStorage[2] */
#define PE_LIST_OVERRIDE_ADDR   0x8009D04Cu   /* g_InvActiveListOverride */
#define PE_OVERRIDE_LIMIT_ADDR  0x8009D054u   /* g_InvOverrideSlotLimit */
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_MAX_HP_ADDR          0x800C0E06u
#define PE_SLOT_COUNT_ADDR      0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR     0x8009D018u
#define PE_WEAPON_SLOT_ADDR     0x800C0E20u
#define PE_ARMOR_SLOT_ADDR      0x800C0E22u
#define PE_AYA_SLOTS_ADDR       0x800C0E48u
#define PE_STORAGE_ADDR         0x800C1EB8u
#define PE_STORAGE_COUNT        100u
#define PE_EQUIP_RECORDS_ADDR   0x800C0EACu
#define PE_EQUIP_RECORD_COUNT   128u
#define PE_ITEM_TABLE_BASE_ADDR 0x800A8034u   /* Item_LookupBaseData */
#define PE_ITEM_TABLE_END_ADDR  0x800A8038u
#define PE_ITEM_TABLE_ORIGIN    0x800A8028u

#define REC_SIZE      0x20u
#define REC_ICON      0x00u
#define REC_ITEM_ID   0x04u
#define REC_KIND      0x06u
#define REC_BONUS     0x0Eu
#define REC_MOD_COUNT 0x14u
#define REC_MODS      0x15u
#define PE_MAX_MODS   10

#define PE_ITEM_TOOL           0x16u
#define PE_ITEM_TOOL_KIT       0x35u
#define PE_ITEM_SUPER_TOOL_KIT 0x36u

#define PE_CARD_BUFFER   0x8009EED0u
#define PE_TAIL_OFFSET   (0x1C90u + 0x40u)
#define PE_TAIL_MAGIC    0x54434550u          /* "PECT" */
#define PE_TAIL_VERSION  1u

#define PE_REBUILD_MAX   30
#define PE_PAYOUT_CAP    150u                 /* decision 12: no dismantle pays more */
#define PE_REBUILD_CAP   150u                 /* decision 12: user's cap */

enum { PE_GUN = 0, PE_ARMOR = 1 };

/* ---- names (as pe_item_editor.c) ------------------------------------------ */
static const char *const s_gun_mods[21] = {
    "EMPTY",
    "RATE OF FIRE 2", "RATE OF FIRE 3", "RATE OF FIRE 5", "RATE OF FIRE 7",
    "RATE OF FIRE 10", "RANDOM FIRE 1-5", "BURST SPRAY", "ATTACK ALL",
    "HEAT BULLETS", "FROST BULLETS", "CYANIDE BULLETS", "ACID BULLETS",
    "TRANQUILIZER", "COUNTERATTACK", "QUICKDRAW", "STEAL (HALF ATK)",
    "STEAL ON ATTACK", "CRITICAL UP", "2 COMMANDS", "3 COMMANDS",
};
static const char *const s_armor_mods[21] = {
    "EMPTY",
    "RESIST POISON", "RESIST STIFFNESS", "RESIST DARKNESS", "LESS STOLEN",
    "RESIST CONFUSION", "AUTO MEDICINE", "AUTO CURE", "BAG +1", "BAG +2",
    "BAG +4", "MAX HP UP", "ATTACK UP", "SLOWER ACTIVE TIME", "PE BURDEN DOWN",
    "ATTACK DOWN",
    /* 16 unused; 17-20 the economy mods (pe_loot.h, custom items doc C1-C4) */
    "", "LOWERS SHOP PRICES", "INCREASES JUNK FOUND", "FINDS RARE ITEMS MORE OFTEN",
    "MORE JUNK FROM DISMANTLING",
};
#define PE_GUN_EFFECTS   20
#define PE_ARMOR_EFFECTS 20                   /* highest armor effect; 16 unused */

/* Effects on each board, in board order. */
static const uint8_t s_board_gun[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
};
static const uint8_t s_board_armor[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    PE_FX_SCAVENGER, PE_FX_LUCKY_FIND, PE_FX_SALVAGER, PE_FX_BARGAIN,
};

/* Mod Catalog board tiles: 7 characters fit one. */
static const char *const s_gun_short[21] = {
    "", "ROF 2", "ROF 3", "ROF 5", "ROF 7", "ROF 10", "RND ROF", "BURST", "ATK ALL",
    "HEAT", "FROST", "CYANIDE", "ACID", "TRANQ", "COUNTER", "QUICK", "STEAL",
    "STL ATK", "CRIT UP", "2 CMDS", "3 CMDS",
};
static const char *const s_armor_short[21] = {
    "", "POISON", "STIFF", "DARK", "STL DN", "CONFUSE", "AUTOMED", "AUTOCUR",
    "BAG +1", "BAG +2", "BAG +4", "MAX HP", "ATK UP", "SLOW AT", "PE BRDN", "ATK DN",
    "", "BARGAIN", "SCAVENG", "LUCKY", "SALVAGE",
};

/* Price tier per effect: 0 common, 1 mid, 2 premium (Junk + a Tool) -- the
 * value ranking of the design doc 6.1b. */
static const uint8_t s_gun_tier[21] = {
    0, 0, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 2, 2,
};
static const uint8_t s_armor_tier[21] = {
    0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0,
    0, 1, 1, 1, 1,                            /* economy mods: mid (custom items doc) */
};
static int s_price[3] = { 10, 30, 80 };   /* tunable (Debug > Loot tuning) */
static int s_rebuild_markup = 150;        /* percent of the payout */

/* Install price of a tier after the preset and Bargain (pe_loot_price). */
static int pe_tier_price(int tier) { return (int)pe_loot_price((uint32_t)s_price[tier]); }

static int pe_mod_group(int which, int effect) {
    if (which == PE_ARMOR) return (effect >= 8 && effect <= 10) ? 5 : 0;
    if (effect >= 1 && effect <= 5) return 1;   /* rate of fire */
    if (effect >= 6 && effect <= 8) return 2;   /* multi-shot */
    if (effect == 19 || effect == 20) return 3; /* extra commands */
    if (effect == 16 || effect == 17) return 4; /* steal */
    return 0;
}

static const char *pe_effect_name(int which, int e) {
    if (which == PE_GUN) return e >= 0 && e <= PE_GUN_EFFECTS ? s_gun_mods[e] : "?";
    return e >= 0 && e <= PE_ARMOR_EFFECTS ? s_armor_mods[e] : "?";
}

static int pe_effect_tier(int which, int e) {
    return which == PE_GUN ? s_gun_tier[e] : s_armor_tier[e];
}

static int pe_is_bag_mod(int which, int e) { return which == PE_ARMOR && e >= 8 && e <= 10; }

static const char *pe_item_name(int id) {
    for (unsigned i = 0; i < PE_ITEM_COUNT; i++)
        if (pe_item_ids[i] == id) return pe_item_names[i];
    return "UNKNOWN";
}

/* ---- state ------------------------------------------------------------------ */
typedef struct {
    uint8_t id;          /* base item id */
    uint8_t slots;       /* mod slot count */
    int16_t bonus[3];
    uint16_t payout;     /* Junk dismantling paid (rebuild price base) */
} PeRebuildEntry;

#define PE_CATALOG_STATE_VERSION 1u
static struct {
    uint32_t bits[2];                         /* unlocked effects, bit e */
    uint8_t rebuild_count;
    uint8_t pad[3];
    PeRebuildEntry rebuild[PE_REBUILD_MAX];   /* newest first */
} s_cat;

/* ---- game access --------------------------------------------------------------- */
static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

static uint32_t pe_record_of_value(uint16_t v) {
    return (v >= 256 && v < 384) ? PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * REC_SIZE : 0;
}

static uint32_t pe_bag_record(int slot) {
    if (slot < 0 || slot >= 50) return 0;
    return pe_record_of_value(psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u));
}

static int pe_equipped_slot(int which) {
    return (int8_t)psx_mod_read_byte(which == PE_GUN ? PE_WEAPON_SLOT_ADDR : PE_ARMOR_SLOT_ADDR);
}

static int pe_record_type(uint32_t rec) {
    const int kind = psx_mod_read_byte(rec + REC_KIND);
    if (kind >= 1 && kind <= 8) return PE_GUN;
    if (kind == 9) return PE_ARMOR;
    return -1;
}

static int pe_mod_count(uint32_t rec) {
    const int n = psx_mod_read_byte(rec + REC_MOD_COUNT);
    return n > PE_MAX_MODS ? PE_MAX_MODS : n;
}

static int pe_bonus_sum(uint32_t rec) {
    int s = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        const int b = (int16_t)psx_mod_read_half(rec + REC_BONUS + i * 2u);
        if (b > 0) s += b;
    }
    return s;
}

static int pe_unlocked(int which, int e) { return (s_cat.bits[which] >> e) & 1u; }

static int pe_carried(uint16_t id) {
    for (uint32_t i = 0; i < 50u; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == id) return 1;
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i)
        if (psx_mod_read_half(PE_STORAGE_ADDR + i * 2u) == id) return 1;
    return 0;
}

/* One Tool for a premium install: free with a Tool Kit, else one Tool from the
 * bag, then from Wayne's storage. Returns 0 if none. */
static int pe_take_tool(int dry_run) {
    if (pe_carried(PE_ITEM_TOOL_KIT) || pe_carried(PE_ITEM_SUPER_TOOL_KIT)) return 1;
    for (uint32_t i = 0; i < 50u; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a) == PE_ITEM_TOOL) { if (!dry_run) psx_mod_write_half(a, 0); return 1; }
    }
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i) {
        const uint32_t a = PE_STORAGE_ADDR + i * 2u;
        if (psx_mod_read_half(a) == PE_ITEM_TOOL) { if (!dry_run) psx_mod_write_half(a, 0); return 1; }
    }
    return 0;
}

static char s_result[64] = "-";
static void pe_toast(const char *text) {
    psx_mod_osd_message(text);
    snprintf(s_result, sizeof s_result, "%s", text);
}

static int pe_ready(void) {
    if (!pe_loaded()) { pe_toast("Load a game first"); return 0; }
    if (!pe_loot_enabled()) { pe_toast("Junk economy is off (Workshop)"); return 0; }
    return 1;
}

/* ---- Dismantle ----------------------------------------------------------------- */
static int s_dis_choice;
static int s_dis_slots[50];
static unsigned s_dis_count;
static char s_dis_names[50][28];
static int s_dis_armed = -1;              /* bag slot armed by the first press */

static unsigned pe_dis_list_count(void) {
    s_dis_count = 0;
    if (!pe_loaded()) return 1;
    const int gun = pe_equipped_slot(PE_GUN), armor = pe_equipped_slot(PE_ARMOR);
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t rec = pe_bag_record((int)i);
        if (!rec || (int)i == gun || (int)i == armor || pe_record_type(rec) < 0) continue;
        const int bonus = pe_bonus_sum(rec);
        if (bonus > 0)
            snprintf(s_dis_names[s_dis_count], sizeof s_dis_names[0], "%s +%d",
                     pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)), bonus);
        else
            snprintf(s_dis_names[s_dis_count], sizeof s_dis_names[0], "%s",
                     pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)));
        s_dis_slots[s_dis_count++] = (int)i;
    }
    return s_dis_count ? s_dis_count : 1;
}

static const char *pe_dis_list_name(unsigned i) {
    if (!s_dis_count) return "NO UNEQUIPPED GEAR";
    return i < s_dis_count ? s_dis_names[i] : "";
}

static int pe_is_last_club(int slot) {
    const uint32_t rec = pe_bag_record(slot);
    if (!rec || psx_mod_read_byte(rec + REC_KIND) != 8) return 0;
    for (int i = 0; i < 50; ++i) {
        if (i == slot) continue;
        const uint32_t other = pe_bag_record(i);
        if (other && psx_mod_read_byte(other + REC_KIND) == 8) return 0;
    }
    return 1;
}

/* Junk for salvaging gear with these + bonuses and mod bytes, and how many
 * effects it newly unlocks. */
static uint32_t pe_salvage_payout(int which, int bonus_sum, const uint8_t *mods, int n,
                                  int *unlocks_out) {
    uint32_t payout = 5u + 2u * (uint32_t)bonus_sum;
    int unlocks = 0, dupes = 0;
    for (int m = 0; m < n; ++m) {
        const int e = mods[m] & 0x1F;
        if (!e) continue;
        payout += 3u;
        if (pe_unlocked(which, e)) ++dupes; else ++unlocks;
    }
    payout += 5u * (uint32_t)dupes;
    if (pe_loot_armor_has(PE_FX_SALVAGER)) payout += payout / 4u;   /* C3 */
    if (payout > PE_PAYOUT_CAP) payout = PE_PAYOUT_CAP;
    if (unlocks_out) *unlocks_out = unlocks;
    return payout;
}

static int pe_read_mods(uint32_t rec, uint8_t *mods) {
    const int n = pe_mod_count(rec);
    for (int m = 0; m < n; ++m) mods[m] = (uint8_t)psx_mod_read_byte(rec + REC_MODS + (uint32_t)m);
    return n;
}

/* Junk a dismantle pays and how many effects it newly unlocks. */
static uint32_t pe_dismantle_preview(uint32_t rec, int which, int *unlocks_out) {
    uint8_t mods[PE_MAX_MODS];
    const int n = pe_read_mods(rec, mods);
    return pe_salvage_payout(which, pe_bonus_sum(rec), mods, n, unlocks_out);
}

/* Unlock the effects in `mods` and put the item on the Rebuild list (newest first). */
static void pe_record_salvage(int which, uint8_t id, uint8_t slots, const int16_t bonus[3],
                              const uint8_t *mods, int n, uint32_t payout) {
    for (int m = 0; m < n; ++m) {
        const int e = mods[m] & 0x1F;
        if (e) s_cat.bits[which] |= 1u << e;
    }
    if (s_cat.rebuild_count == PE_REBUILD_MAX) --s_cat.rebuild_count;   /* drop oldest */
    memmove(&s_cat.rebuild[1], &s_cat.rebuild[0], sizeof(PeRebuildEntry) * s_cat.rebuild_count);
    s_cat.rebuild[0].id = id;
    s_cat.rebuild[0].slots = slots;
    for (uint32_t i = 0; i < 3; ++i) s_cat.rebuild[0].bonus[i] = bonus[i];
    s_cat.rebuild[0].payout = (uint16_t)payout;
    ++s_cat.rebuild_count;
}

static void pe_dismantle(void) {
    char msg[96];
    if (!pe_ready()) return;
    pe_dis_list_count();
    if (!s_dis_count || s_dis_choice < 0 || (unsigned)s_dis_choice >= s_dis_count) {
        pe_toast("Nothing to dismantle"); return;
    }
    const int slot = s_dis_slots[s_dis_choice];
    const uint32_t rec = pe_bag_record(slot);
    const int which = rec ? pe_record_type(rec) : -1;
    if (which < 0) return;
    if (pe_is_last_club(slot)) { pe_toast("Keep your last Club"); s_dis_armed = -1; return; }

    int unlocks = 0;
    uint8_t mods[PE_MAX_MODS];
    const int n = pe_read_mods(rec, mods);
    const uint32_t payout = pe_salvage_payout(which, pe_bonus_sum(rec), mods, n, &unlocks);

    const char *name = pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID));
    if (s_dis_armed != slot) {                /* first press: show what happens */
        s_dis_armed = slot;
        snprintf(msg, sizeof msg, "Dismantle %s: +%u Junk, %d new effect(s). Press again",
                 name, (unsigned)payout, unlocks);
        pe_toast(msg);
        return;
    }
    s_dis_armed = -1;

    int16_t bonus[3];
    for (uint32_t i = 0; i < 3; ++i) bonus[i] = (int16_t)psx_mod_read_half(rec + REC_BONUS + i * 2u);
    pe_record_salvage(which, (uint8_t)psx_mod_read_byte(rec + REC_ITEM_ID),
                      (uint8_t)psx_mod_read_byte(rec + REC_MOD_COUNT), bonus, mods, n, payout);

    for (uint32_t o = 0; o < REC_SIZE; o += 4) psx_mod_write_word(rec + o, 0);   /* free record */
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, 0);
    pe_loot_give_junk(payout);
    snprintf(msg, sizeof msg, "Dismantled %s: +%u Junk (total %u)", name, (unsigned)payout,
             (unsigned)pe_loot_junk());
    pe_toast(msg);
}

/* pe_loot.h, for the game's own Discard (pe_native_menu.c): what dismantling
 * the gun or armor in record `rec` pays (0 if it isn't one), and the reward
 * itself -- effects unlocked, Rebuild entry, Junk -- leaving the record for
 * the game to free. */
uint32_t pe_catalog_dismantle_preview(uint32_t rec) {
    const int which = rec ? pe_record_type(rec) : -1;
    return which < 0 ? 0u : pe_dismantle_preview(rec, which, 0);
}
uint32_t pe_catalog_dismantle_reward(uint32_t rec, int *unlocks_out) {
    const int which = rec ? pe_record_type(rec) : -1;
    if (which < 0) return 0;
    int unlocks = 0;
    uint8_t mods[PE_MAX_MODS];
    const int n = pe_read_mods(rec, mods);
    const uint32_t payout = pe_salvage_payout(which, pe_bonus_sum(rec), mods, n, &unlocks);
    int16_t bonus[3];
    for (uint32_t i = 0; i < 3; ++i) bonus[i] = (int16_t)psx_mod_read_half(rec + REC_BONUS + i * 2u);
    pe_record_salvage(which, (uint8_t)psx_mod_read_byte(rec + REC_ITEM_ID),
                      (uint8_t)psx_mod_read_byte(rec + REC_MOD_COUNT), bonus, mods, n, payout);
    pe_loot_give_junk(payout);
    if (unlocks_out) *unlocks_out = unlocks;
    return payout;
}

/* ---- Mod Catalog ---------------------------------------------------------------- */
static int s_target;                      /* 0 = equipped gun, 1 = equipped armor */
static int s_grid;                        /* board cursor: effect s_grid + 1 */
static int s_grid_armed = -1;             /* tile armed by the first accept */
static int s_slot_choice;
static char s_slot_names[PE_MAX_MODS][32];
static char s_target_names[2][32];

static uint32_t pe_equipped_record(int which) {
    if (!pe_loaded()) return 0;
    const uint32_t rec = pe_bag_record(pe_equipped_slot(which));
    return rec && pe_record_type(rec) == which ? rec : 0;
}

static uint32_t pe_target_record(void) { return pe_equipped_record(s_target); }

/* "GUN: <name>" / "ARMOR: <name>": the item the Catalog works on. */
static unsigned pe_target_list_count(void) { return 2; }
static const char *pe_target_list_name(unsigned i) {
    if (i > 1) return "";
    const uint32_t rec = pe_equipped_record((int)i);
    snprintf(s_target_names[i], sizeof s_target_names[0], "%s: %s", i ? "ARMOR" : "GUN",
             rec ? pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)) : "NONE EQUIPPED");
    return s_target_names[i];
}

/* Effect seen on any carried item of this type (bag), for the LOCKED preview. */
static int pe_seen(int which, int e) {
    for (int i = 0; i < 50; ++i) {
        const uint32_t rec = pe_bag_record(i);
        if (!rec || pe_record_type(rec) != which) continue;
        const int n = pe_mod_count(rec);
        for (int m = 0; m < n; ++m)
            if ((psx_mod_read_byte(rec + REC_MODS + (uint32_t)m) & 0x1F) == e) return 1;
    }
    return 0;
}

static int pe_effect_count(int which) {
    return which == PE_GUN ? (int)sizeof s_board_gun : (int)sizeof s_board_armor;
}
/* Effect on board tile i of `which`, or 0. */
static int pe_board_effect(int which, int i) {
    if (i < 0 || i >= pe_effect_count(which)) return 0;
    return which == PE_GUN ? s_board_gun[i] : s_board_armor[i];
}
static int pe_grid_effect(void) { return pe_board_effect(s_target, s_grid); }

static unsigned pe_grid_count(void) {
    const int n = pe_effect_count(s_target);
    if (s_grid < 0 || s_grid >= n) s_grid = 0;
    return (unsigned)n;
}

/* The equipped target already carries effect e. */
static int pe_target_has(int e) {
    const uint32_t rec = pe_target_record();
    const int n = rec ? pe_mod_count(rec) : 0;
    for (int m = 0; m < n; ++m)
        if ((psx_mod_read_byte(rec + REC_MODS + (uint32_t)m) & 0x1F) == e) return 1;
    return 0;
}

static int pe_grid_cell(int index, char *label, unsigned size, unsigned *flags) {
    const int which = s_target;
    if (index < 0) {                          /* header: unlocked / total */
        int got = 0;
        const int n = pe_effect_count(which);
        for (int i = 0; i < n; ++i) got += pe_unlocked(which, pe_board_effect(which, i));
        snprintf(label, size, "%d/%d", got, n);
        return 1;
    }
    const int e = pe_board_effect(which, index);
    if (!e) return 0;
    unsigned f = 0;
    if (pe_unlocked(which, e)) {
        snprintf(label, size, "%s", which == PE_GUN ? s_gun_short[e] : s_armor_short[e]);
    } else if (pe_loaded() && pe_seen(which, e)) {
        snprintf(label, size, "%s", which == PE_GUN ? s_gun_short[e] : s_armor_short[e]);
        f |= PE_CELL_DIM;
    } else {
        snprintf(label, size, "?");
        f |= PE_CELL_EMPTY;
    }
    if (pe_effect_tier(which, e) == 2) f |= PE_CELL_HIGHLIGHT;
    if (pe_target_has(e)) f |= PE_CELL_MARK;
    *flags = f;
    return 1;
}

/* Slot an install of effect e would use: the slot holding its group mate
 * (the game allows one per group), else the chosen slot if it is free,
 * else the first free slot, else the chosen slot. */
static int pe_install_slot(int e) {
    const uint32_t rec = pe_target_record();
    const int n = rec ? pe_mod_count(rec) : 0;
    if (n <= 0) return -1;
    const int group = pe_mod_group(s_target, e);
    for (int m = 0; group && m < n; ++m) {
        const int b = psx_mod_read_byte(rec + REC_MODS + (uint32_t)m);
        if ((b & 0x1F) && (b >> 5) == group) return m;
    }
    const int chosen = s_slot_choice >= 0 && s_slot_choice < n ? s_slot_choice : 0;
    if (!(psx_mod_read_byte(rec + REC_MODS + (uint32_t)chosen) & 0x1F)) return chosen;
    for (int m = 0; m < n; ++m)
        if (!(psx_mod_read_byte(rec + REC_MODS + (uint32_t)m) & 0x1F)) return m;
    return chosen;
}

static void pe_grid_moved(void) {
    s_grid_armed = -1;
    const int slot = pe_install_slot(pe_grid_effect());
    if (slot >= 0) s_slot_choice = slot;
}

/* "Selected" row: the cursor tile's full name and what it costs. */
static char s_sel_text[64];
static unsigned pe_one(void) { return 1; }
static const char *pe_sel_name(unsigned i) {
    (void)i;
    const int e = pe_grid_effect();
    if (!e) return "";
    if (pe_unlocked(s_target, e)) {
        const int tier = pe_effect_tier(s_target, e);
        snprintf(s_sel_text, sizeof s_sel_text, "%s %d%s", pe_effect_name(s_target, e),
                 pe_tier_price(tier), tier == 2 ? "+TOOL" : "");
    } else if (pe_loaded() && pe_seen(s_target, e)) {
        snprintf(s_sel_text, sizeof s_sel_text, "%s: DISMANTLE", pe_effect_name(s_target, e));
    } else {
        snprintf(s_sel_text, sizeof s_sel_text, "NOT FOUND YET");
    }
    return s_sel_text;
}

/* "Replaces" row: what installing the cursor tile in the chosen slot removes. */
static char s_repl_text[64];
static const char *pe_repl_name(unsigned i) {
    (void)i;
    const uint32_t rec = pe_target_record();
    const int n = rec ? pe_mod_count(rec) : 0;
    if (!rec || n <= 0 || s_slot_choice < 0 || s_slot_choice >= n) return "-";
    const int e = pe_grid_effect(), group = pe_mod_group(s_target, e);
    const int cur = psx_mod_read_byte(rec + REC_MODS + (uint32_t)s_slot_choice) & 0x1F;
    int other = 0;
    for (int m = 0; group && m < n; ++m) {
        const int b = psx_mod_read_byte(rec + REC_MODS + (uint32_t)m);
        if (m != s_slot_choice && (b & 0x1F) && (b >> 5) == group) other = b & 0x1F;
    }
    if (cur == e) snprintf(s_repl_text, sizeof s_repl_text, "ALREADY THERE");
    else if (cur && other)
        snprintf(s_repl_text, sizeof s_repl_text, "%s, %s", pe_effect_name(s_target, cur),
                 pe_effect_name(s_target, other));
    else if (cur || other)
        snprintf(s_repl_text, sizeof s_repl_text, "%s", pe_effect_name(s_target, cur ? cur : other));
    else snprintf(s_repl_text, sizeof s_repl_text, "NOTHING (EMPTY SLOT)");
    return s_repl_text;
}

static const char *pe_result_name(unsigned i) { (void)i; return s_result; }
static int s_text_dummy[3];               /* storage for the read-only text rows */

static unsigned pe_slot_list_count(void) {
    const uint32_t rec = pe_target_record();
    const int n = rec ? pe_mod_count(rec) : 0;
    for (int m = 0; m < n; ++m)
        snprintf(s_slot_names[m], sizeof s_slot_names[0], "%d %s", m + 1,
                 pe_effect_name(s_target, psx_mod_read_byte(rec + REC_MODS + (uint32_t)m) & 0x1F));
    return n > 0 ? (unsigned)n : 1;
}

static const char *pe_slot_list_name(unsigned i) {
    const uint32_t rec = pe_target_record();
    const int n = rec ? pe_mod_count(rec) : 0;
    if (!rec) return s_target == PE_GUN ? "NO GUN EQUIPPED" : "NO ARMOR EQUIPPED";
    if (n <= 0) return "NO MOD SLOTS";
    return i < (unsigned)n ? s_slot_names[i] : "";
}

static void pe_install(void) {
    char msg[96];
    if (!pe_ready()) return;
    const uint32_t rec = pe_target_record();
    if (!rec) { pe_toast(s_target == PE_GUN ? "Equip a gun first" : "Equip armor first"); return; }
    const int n = pe_mod_count(rec);
    if (n <= 0 || s_slot_choice < 0 || s_slot_choice >= n) { pe_toast("This item has no mod slots"); return; }
    pe_grid_count();
    const int e = pe_grid_effect();
    if (!pe_unlocked(s_target, e)) { pe_toast("Locked: dismantle an item with this effect"); return; }

    const uint32_t slot_addr = rec + REC_MODS + (uint32_t)s_slot_choice;
    const int current = psx_mod_read_byte(slot_addr) & 0x1F;
    if (current == e) { pe_toast("Already installed there"); return; }
    if (pe_is_bag_mod(s_target, current)) { pe_toast("Bag mods can't be replaced here"); return; }
    const int group = pe_mod_group(s_target, e);
    if (group) {
        for (int m = 0; m < n; ++m) {
            if (m == s_slot_choice) continue;
            const int other = psx_mod_read_byte(rec + REC_MODS + (uint32_t)m);
            if ((other & 0x1F) && (other >> 5) == group && pe_is_bag_mod(s_target, other & 0x1F)) {
                pe_toast("Bag mods can't be replaced here"); return;
            }
        }
    }

    const int tier = pe_effect_tier(s_target, e);
    const uint32_t price = (uint32_t)pe_tier_price(tier);
    if (pe_loot_junk() < price) {
        snprintf(msg, sizeof msg, "Need %u Junk (have %u)", (unsigned)price, (unsigned)pe_loot_junk());
        pe_toast(msg); return;
    }
    if (tier == 2 && !pe_take_tool(1)) { pe_toast("Needs a Tool (or a Tool Kit)"); return; }

    if (group) {                              /* the game's rule: one per group */
        for (int m = 0; m < n; ++m) {
            if (m == s_slot_choice) continue;
            const uint32_t a = rec + REC_MODS + (uint32_t)m;
            const int other = psx_mod_read_byte(a);
            if ((other & 0x1F) && (other >> 5) == group) psx_mod_write_byte(a, 0);
        }
    }
    psx_mod_write_byte(slot_addr, (uint8_t)((group << 5) | e));
    pe_loot_spend_junk(price);
    if (tier == 2) pe_take_tool(0);
    snprintf(msg, sizeof msg, "%s: installed %s (-%u Junk%s)%s",
             pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)), pe_effect_name(s_target, e),
             (unsigned)price, tier == 2 ? ", Tool" : "",
             pe_is_bag_mod(s_target, e) ? ". Re-equip to grow the bag" : "");
    pe_toast(msg);
}

/* Accept on a board tile: the first press says what would happen, the second
 * installs into the slot shown on the Slot row. */
static void pe_grid_accept(void) {
    char msg[64];
    if (!pe_ready()) return;
    const int e = pe_grid_effect();
    if (!pe_unlocked(s_target, e)) { pe_install(); return; }   /* explains why not */
    if (s_grid_armed != s_grid) {
        s_grid_armed = s_grid;
        const int tier = pe_effect_tier(s_target, e);
        snprintf(msg, sizeof msg, "Slot %d, %d Junk%s: press again", s_slot_choice + 1,
                 pe_tier_price(tier), tier == 2 ? " + Tool" : "");
        pe_toast(msg);
        return;
    }
    s_grid_armed = -1;
    pe_install();
}

static void pe_remove(void) {
    char msg[64];
    if (!pe_ready()) return;
    const uint32_t rec = pe_target_record();
    if (!rec) { pe_toast(s_target == PE_GUN ? "Equip a gun first" : "Equip armor first"); return; }
    const int n = pe_mod_count(rec);
    if (n <= 0 || s_slot_choice < 0 || s_slot_choice >= n) return;
    const uint32_t a = rec + REC_MODS + (uint32_t)s_slot_choice;
    const int e = psx_mod_read_byte(a) & 0x1F;
    if (!e) { pe_toast("That slot is empty"); return; }
    if (pe_is_bag_mod(s_target, e)) { pe_toast("Bag mods can't be removed here"); return; }
    psx_mod_write_byte(a, 0);
    snprintf(msg, sizeof msg, "%s: removed %s (stays unlocked)",
             pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)), pe_effect_name(s_target, e));
    pe_toast(msg);
}

static int pe_readonly(int value) { (void)value; return 0; }

/* ---- Rebuild -------------------------------------------------------------------- */
static int s_rb_choice;
static char s_rb_names[PE_REBUILD_MAX][32];

static uint32_t pe_rebuild_price(const PeRebuildEntry *r) {
    uint32_t p = pe_loot_price((uint32_t)r->payout * (uint32_t)s_rebuild_markup / 100u);
    if (p < r->payout) p = r->payout;         /* never below what dismantling paid */
    return p > PE_REBUILD_CAP ? PE_REBUILD_CAP : p;
}

static unsigned pe_rb_list_count(void) {
    for (unsigned i = 0; i < s_cat.rebuild_count; ++i)
        snprintf(s_rb_names[i], sizeof s_rb_names[0], "%s %u", pe_item_name(s_cat.rebuild[i].id),
                 (unsigned)pe_rebuild_price(&s_cat.rebuild[i]));
    return s_cat.rebuild_count ? s_cat.rebuild_count : 1;
}

static const char *pe_rb_list_name(unsigned i) {
    if (!s_cat.rebuild_count) return "NOTHING DISMANTLED YET";
    return i < s_cat.rebuild_count ? s_rb_names[i] : "";
}

/* Put a new copy of base item `id` in a free bag slot: + bonuses, mod slot
 * count and mod bytes as given. Returns 1, or 0 with a toast. */
static int pe_create_item(uint8_t id, uint8_t slots, const int16_t bonus[3],
                          const uint8_t *mods, int nmods) {
    int slot = -1;
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit && slot < 0; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == 0) slot = (int)i;
    if (slot < 0) { pe_toast("Bag full"); return 0; }
    uint32_t rec_index = PE_EQUIP_RECORD_COUNT;
    for (uint32_t i = 0; i < PE_EQUIP_RECORD_COUNT; ++i)
        if (!psx_mod_read_byte(PE_EQUIP_RECORDS_ADDR + i * REC_SIZE + REC_ICON)) { rec_index = i; break; }
    if (rec_index >= PE_EQUIP_RECORD_COUNT) { pe_toast("No free equipment record"); return 0; }
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
    const uint32_t end = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
    const uint32_t index = (uint32_t)id - 1u;
    if (end <= base || index >= ((end - base) >> 5)) { pe_toast("Unknown item"); return 0; }
    const uint32_t src = PE_ITEM_TABLE_ORIGIN + base + index * REC_SIZE;
    const uint32_t dst = PE_EQUIP_RECORDS_ADDR + rec_index * REC_SIZE;
    for (uint32_t o = 0; o < REC_SIZE; o += 4) psx_mod_write_word(dst + o, psx_mod_read_word(src + o));
    for (uint32_t i = 0; i < 3; ++i) psx_mod_write_half(dst + REC_BONUS + i * 2u, (uint16_t)bonus[i]);
    psx_mod_write_byte(dst + REC_MOD_COUNT, slots);
    for (uint32_t m = 0; m < PE_MAX_MODS; ++m)
        psx_mod_write_byte(dst + REC_MODS + m, (int)m < nmods ? mods[m] : 0);
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, (uint16_t)(rec_index + 256u));
    return 1;
}

static void pe_rebuild(void) {
    char msg[80];
    if (!pe_ready()) return;
    if (!s_cat.rebuild_count || s_rb_choice < 0 || s_rb_choice >= s_cat.rebuild_count) {
        pe_toast("Nothing to rebuild"); return;
    }
    const PeRebuildEntry r = s_cat.rebuild[s_rb_choice];
    const uint32_t price = pe_rebuild_price(&r);
    if (pe_loot_junk() < price) {
        snprintf(msg, sizeof msg, "Need %u Junk (have %u)", (unsigned)price, (unsigned)pe_loot_junk());
        pe_toast(msg); return;
    }
    if (!pe_create_item(r.id, r.slots, r.bonus, 0, 0)) return;

    pe_loot_spend_junk(price);
    memmove(&s_cat.rebuild[s_rb_choice], &s_cat.rebuild[s_rb_choice + 1],
            sizeof(PeRebuildEntry) * (size_t)(s_cat.rebuild_count - s_rb_choice - 1));
    --s_cat.rebuild_count;
    snprintf(msg, sizeof msg, "Rebuilt %s (-%u Junk)", pe_item_name(r.id), (unsigned)price);
    pe_toast(msg);
}

/* ---- live shop rows: your Junk, what the selection costs or pays ------------------- */
static int pe_get_junk(void) { return (int)pe_loot_junk(); }

static uint32_t pe_selected_dismantle_record(int *which) {
    if (!pe_loaded()) return 0;
    pe_dis_list_count();
    if (!s_dis_count || s_dis_choice < 0 || (unsigned)s_dis_choice >= s_dis_count) return 0;
    const uint32_t rec = pe_bag_record(s_dis_slots[s_dis_choice]);
    *which = rec ? pe_record_type(rec) : -1;
    return *which >= 0 ? rec : 0;
}
static int pe_get_dismantle_pays(void) {
    int which;
    const uint32_t rec = pe_selected_dismantle_record(&which);
    return rec ? (int)pe_dismantle_preview(rec, which, 0) : 0;
}
static int pe_get_dismantle_unlocks(void) {
    int which, unlocks = 0;
    const uint32_t rec = pe_selected_dismantle_record(&which);
    if (rec) pe_dismantle_preview(rec, which, &unlocks);
    return unlocks;
}
static int pe_get_rebuild_cost(void) {
    if (!s_cat.rebuild_count || s_rb_choice < 0 || s_rb_choice >= s_cat.rebuild_count) return 0;
    return (int)pe_rebuild_price(&s_cat.rebuild[s_rb_choice]);
}

/* ---- debug (Debug > Loot tuning) -------------------------------------------------- */
static void pe_debug_unlock_all(void) {
    for (int which = 0; which < 2; ++which)
        for (int i = 0; i < pe_effect_count(which); ++i)
            s_cat.bits[which] |= 1u << pe_board_effect(which, i);
    pe_toast("Mod Catalog: all effects unlocked");
}
static void pe_debug_clear_catalog(void) {
    memset(&s_cat, 0, sizeof s_cat);
    pe_toast("Mod Catalog and Rebuild list cleared");
}
/* Fixed test gear, one mod per rule: a group mod, an ungrouped one, a
 * premium one, another group; the armor carries the protected bag mod. */
static void pe_debug_test_gun(void) {
    static const uint8_t mods[4] = { 0x22u, 0x09u, 0x74u, 0x91u };  /* ROF 3, Heat, 3 Commands, Steal on attack */
    static const int16_t bonus[3] = { 5, 0, 0 };
    if (!pe_loaded()) { pe_toast("Load a game first"); return; }
    if (pe_create_item(0x45u, 6u, bonus, mods, 4)) pe_toast("Test gun added: M9 +5 with 4 mods");
}
static void pe_debug_test_armor(void) {
    static const uint8_t mods[4] = { 0x01u, 0x06u, 0xA8u, 0x0Bu };  /* Resist Poison, Auto Medicine, Bag +1, Max HP Up */
    static const int16_t bonus[3] = { 0, 0, 0 };
    if (!pe_loaded()) { pe_toast("Load a game first"); return; }
    if (pe_create_item(0x97u, 4u, bonus, mods, 4)) pe_toast("Test armor added: N Protector with 4 mods");
}

/* ---- Tool salvage (decision 42) ------------------------------------------------------
 * A plain Tool deletes the donor item, and whatever was left on it was lost.
 * Treat that donor as dismantled: what stays behind unlocks in the Catalog,
 * pays dismantle Junk and goes on the Rebuild list. A Super Tool keeps the
 * donor, so nothing happens. Runs at the entry of
 * Inv_TransferItemBetweenLists(selection, modifier, flags, cost): the donor is
 * saved selection `selection`, the target is the other one; modifier >= 0
 * moves that one mod, < 0 moves the + bonuses; flags bit 0 = Super Tool. */
static uint32_t pe_selection_record(uint32_t sel) {
    if (sel >= 2u) return 0;
    const int index = (int32_t)psx_mod_read_word(PE_SEL_INDEX_ADDR + sel * 4u);
    uint32_t list = PE_AYA_SLOTS_ADDR, limit = pe_bag_limit();
    const uint32_t override = psx_mod_read_word(PE_LIST_OVERRIDE_ADDR);
    if (psx_mod_read_word(PE_SEL_STORAGE_ADDR + sel * 4u) && override) {
        list = override;
        limit = psx_mod_read_word(PE_OVERRIDE_LIMIT_ADDR);
    }
    if (index < 0 || (uint32_t)index >= limit) return 0;
    return pe_record_of_value(psx_mod_read_half(list + (uint32_t)index * 2u));
}

/* Would the game's transfer of `value` find room on `dest`? (same group
 * overwrites, else the first empty slot) */
static int pe_tool_move_fits(uint32_t dest, int value) {
    const int n = psx_mod_read_byte(dest + REC_MOD_COUNT);
    if (value & 0xE0)
        for (int i = 0; i < n; ++i)
            if ((psx_mod_read_byte(dest + REC_MODS + (uint32_t)i) & 0xE0) == (value & 0xE0)) return 1;
    for (int i = 0; i < n; ++i)
        if (!(psx_mod_read_byte(dest + REC_MODS + (uint32_t)i) & 0x1F)) return 1;
    return 0;
}

/* The target mod a same-group move overwrites (0 if none). */
static int pe_tool_overwritten(uint32_t dest, int value) {
    if (!(value & 0xE0)) return 0;
    const int n = psx_mod_read_byte(dest + REC_MOD_COUNT);
    for (int i = 0; i < n; ++i) {
        const int old = psx_mod_read_byte(dest + REC_MODS + (uint32_t)i);
        if ((old & 0xE0) == (value & 0xE0)) return old & 0x1F;
    }
    return 0;
}

static void pe_on_tool_transfer(struct CPUState *cpu, uint32_t address) {
    (void)address;
    char msg[96];
    const uint32_t sel = cpu->gpr[4];
    const int modifier = (int32_t)cpu->gpr[5];
    if (!pe_loot_enabled() || !pe_loaded()) return;
    const uint32_t src = pe_selection_record(sel);
    const uint32_t dest = pe_selection_record(sel ^ 1u);
    const int which = src ? pe_record_type(src) : -1;
    if (which < 0 || !dest) return;

    uint8_t mods[PE_MAX_MODS];
    const int n = pe_read_mods(src, mods);

    /* Any Tool: a target mod replaced by a same-group move goes to the Catalog. */
    int kept_new = 0;
    const int dest_which = pe_record_type(dest);
    if (modifier >= 0 && modifier < n && dest_which >= 0) {
        const int old = pe_tool_overwritten(dest, mods[modifier]);
        if (old && old != (mods[modifier] & 0x1F) && !pe_unlocked(dest_which, old)) {
            s_cat.bits[dest_which] |= 1u << old;
            kept_new = 1;
        }
        if (kept_new && (cpu->gpr[6] & 1u)) {
            snprintf(msg, sizeof msg, "%s replaced: kept in the Mod Catalog",
                     pe_effect_name(dest_which, old));
            pe_toast(msg);
        }
    }
    if (cpu->gpr[6] & 1u) return;             /* Super Tool: donor survives */
    int16_t bonus[3];
    for (uint32_t i = 0; i < 3; ++i) bonus[i] = (int16_t)psx_mod_read_half(src + REC_BONUS + i * 2u);
    if (modifier >= 0) {                      /* that mod leaves if it fits */
        if (modifier < n && pe_tool_move_fits(dest, mods[modifier])) mods[modifier] = 0;
    } else {                                  /* the + bonuses leave */
        bonus[0] = bonus[1] = bonus[2] = 0;
    }
    int bonus_sum = 0;
    for (int i = 0; i < 3; ++i) if (bonus[i] > 0) bonus_sum += bonus[i];

    int unlocks = 0;
    const uint32_t payout = pe_salvage_payout(which, bonus_sum, mods, n, &unlocks);
    const uint8_t id = (uint8_t)psx_mod_read_byte(src + REC_ITEM_ID);
    pe_record_salvage(which, id, (uint8_t)psx_mod_read_byte(src + REC_MOD_COUNT),
                      bonus, mods, n, payout);
    pe_loot_give_junk(payout);
    snprintf(msg, sizeof msg, "Salvaged %s: +%u Junk, %d new effect(s)",
             pe_item_name(id), (unsigned)payout, unlocks + kept_new);
    pe_toast(msg);
}

/* ---- pe_loot.h: armor effects for the rare table -------------------------------- */
int pe_catalog_armor_unlocked(int effect) {
    return effect > 0 && effect <= PE_ARMOR_EFFECTS && pe_unlocked(PE_ARMOR, effect);
}
int pe_catalog_unlock_armor(int effect) {
    if (effect <= 0 || effect > PE_ARMOR_EFFECTS || pe_unlocked(PE_ARMOR, effect)) return 0;
    s_cat.bits[PE_ARMOR] |= 1u << effect;
    return 1;
}

/* ---- persistence ------------------------------------------------------------------ */
static uint32_t pe_checksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0x5A17C0DEu;
    for (uint32_t i = 0; i < n; ++i) s = (s << 5) + s + p[i];
    return s;
}

static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    const uint8_t *p = (const uint8_t *)&s_cat;
    psx_mod_write_word(at + 0u, PE_TAIL_MAGIC);
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, (uint32_t)sizeof s_cat);
    for (uint32_t i = 0; i < sizeof s_cat; ++i) psx_mod_write_byte(at + 16u + i, p[i]);
    psx_mod_write_word(at + 12u, pe_checksum(p, sizeof s_cat));
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    memset(&s_cat, 0, sizeof s_cat);
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC || psx_mod_read_word(at + 4u) != PE_TAIL_VERSION ||
        psx_mod_read_word(at + 8u) != sizeof s_cat)
        return;
    uint8_t buf[sizeof s_cat];
    for (uint32_t i = 0; i < sizeof s_cat; ++i) buf[i] = psx_mod_read_byte(at + 16u + i);
    if (pe_checksum(buf, sizeof s_cat) != psx_mod_read_word(at + 12u)) return;
    memcpy(&s_cat, buf, sizeof s_cat);
    if (s_cat.rebuild_count > PE_REBUILD_MAX) s_cat.rebuild_count = 0;
}

static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if ((int)cpu->gpr[4] == PE_INV_CMD_START_NEW_GAME) memset(&s_cat, 0, sizeof s_cat);
}

static void pe_catalog_state_loaded(int result, uint32_t saved_version,
                                    const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_cat, 0, sizeof s_cat);
    s_dis_armed = -1;
}

/* ---- menu ----------------------------------------------------------------------- */
#define PE_DIS_PAGE    "Workshop/Dismantle"
#define PE_CAT_PAGE    "Workshop/Mod Catalog"
#define PE_RB_PAGE     "Workshop/Rebuild"
#define PE_TUNING_PAGE "Debug/Loot tuning"

/* Called from pe_mod_menu.c after pe_loot_menu_register (Workshop rows first). */
void pe_loot_catalog_menu_register(void) {
    (void)pe_menu_register_live_int(PE_DIS_PAGE, "Your Junk",
        "Junk you have now.", 0, 99999, 1, pe_get_junk, pe_readonly);
    (void)pe_menu_register_dynamic_choice(PE_DIS_PAGE, "Item",
        "Unequipped gun or armor.", &s_dis_choice, 50,
        pe_dis_list_count, pe_dis_list_name);
    (void)pe_menu_register_live_int(PE_DIS_PAGE, "Pays (Junk)",
        "Junk this item gives.", 0, 999, 1, pe_get_dismantle_pays, pe_readonly);
    (void)pe_menu_register_live_int(PE_DIS_PAGE, "New effects",
        "Effects it adds to the Catalog.", 0, 10, 1, pe_get_dismantle_unlocks, pe_readonly);
    (void)pe_menu_register_action(PE_DIS_PAGE, "Dismantle",
        "Press twice. Effects go to the Catalog.", pe_dismantle);

    (void)pe_menu_register_live_int(PE_CAT_PAGE, "Your Junk",
        "Junk you have now.", 0, 99999, 1, pe_get_junk, pe_readonly);
    (void)pe_menu_register_dynamic_choice(PE_CAT_PAGE, "Item",
        "Your equipped gun or armor.", &s_target, 2,
        pe_target_list_count, pe_target_list_name);
    (void)pe_menu_register_grid(PE_CAT_PAGE, "Effects",
        "Grey: dismantle to unlock. Gold: +Tool.", &s_grid, 24, 5,
        pe_grid_count, pe_grid_cell, pe_grid_accept, pe_grid_moved);
    (void)pe_menu_register_dynamic_choice(PE_CAT_PAGE, "Selected",
        "The effect under the cursor.", &s_text_dummy[0], 1, pe_one, pe_sel_name);
    (void)pe_menu_register_dynamic_choice(PE_CAT_PAGE, "Slot",
        "Mod slot to fill or clear.", &s_slot_choice, PE_MAX_MODS,
        pe_slot_list_count, pe_slot_list_name);
    (void)pe_menu_register_dynamic_choice(PE_CAT_PAGE, "Replaces",
        "What installing removes.", &s_text_dummy[1], 1, pe_one, pe_repl_name);
    (void)pe_menu_register_action(PE_CAT_PAGE, "Install",
        "Put the effect in the slot.", pe_install);
    (void)pe_menu_register_action(PE_CAT_PAGE, "Remove",
        "Empty the slot. Stays unlocked.", pe_remove);
    (void)pe_menu_register_dynamic_choice(PE_CAT_PAGE, "Result",
        "What the last action did.", &s_text_dummy[2], 1, pe_one, pe_result_name);

    (void)pe_menu_register_live_int(PE_RB_PAGE, "Your Junk",
        "Junk you have now.", 0, 99999, 1, pe_get_junk, pe_readonly);
    (void)pe_menu_register_dynamic_choice(PE_RB_PAGE, "Item",
        "Dismantled gear, newest first.", &s_rb_choice, PE_REBUILD_MAX,
        pe_rb_list_count, pe_rb_list_name);
    (void)pe_menu_register_live_int(PE_RB_PAGE, "Cost (Junk)",
        "Price to rebuild this item.", 0, 999, 1, pe_get_rebuild_cost, pe_readonly);
    (void)pe_menu_register_action(PE_RB_PAGE, "Rebuild",
        "Buy it back with empty mod slots.", pe_rebuild);

    (void)pe_menu_register_int(PE_TUNING_PAGE, "Mod price common",
        "Junk for a common effect.", &s_price[0], 0, 500, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Mod price mid",
        "Junk for a mid effect.", &s_price[1], 0, 500, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Mod price premium",
        "Junk for a premium effect (+Tool).", &s_price[2], 0, 500, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Rebuild markup %",
        "Rebuild price vs dismantle payout.", &s_rebuild_markup, 100, 300, 10);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Unlock all effects",
        "Debug: fill the Mod Catalog.", pe_debug_unlock_all);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Clear catalog",
        "Debug: empty Catalog and Rebuild list.", pe_debug_clear_catalog);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Give test gun",
        "Debug: M9 +5, ROF3 Heat 3Cmd Steal.", pe_debug_test_gun);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Give test armor",
        "Debug: N Protector, 4 armor mods.", pe_debug_test_armor);

    (void)psx_mod_register_function_entry_plugin("pe.loot-catalog.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot-catalog.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot-catalog.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_function_entry_plugin("pe.loot-catalog.tool",
        PE_FN_TOOL_TRANSFER, pe_on_tool_transfer);
    (void)psx_mod_register_state("pe.loot.catalog", PE_CATALOG_STATE_VERSION,
        &s_cat, sizeof s_cat, pe_catalog_state_loaded);
}
