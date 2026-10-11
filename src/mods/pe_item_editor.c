/*
 * Item Editor: F1 > Mods > PE Enhancements > Item Editor.
 *
 * Edits the EQUIPPED gun and armor in place, with live rows (changes apply
 * immediately; the game is paused while F1 is open):
 *   - the three "+N" bonuses (the numbers bonus points and tools change; the
 *     item's base stats are left alone), capped at the game's 999;
 *   - the gun's loaded ammo, capped at its bullet capacity;
 *   - mods: pick a slot, then what is in it.
 *
 * RECORD (decomp inventory.h ItemDataRecord, checked live): the equipped
 * weapon/armor bag slots are s8 at 0x800C0E20 / 0x800C0E22; a bag slot holding
 * record r stores r + 256, and the record lives at 0x800C0EAC + 0x20 * r.
 *   +0x0A ammo, +0x07 baseStats[3], +0x0E s16 bonusStats[3]
 *   +0x14 mod slot count (<= 10), +0x15 mod bytes
 *
 * MOD BYTES (decomp BattleCmd_LoadWeaponModifiers, Inv_TransferItemBetweenLists,
 * Menu_Dialogs): low 5 bits = effect 1..20 (0 = empty slot), numbered
 * separately for guns and armor; high 3 bits = exclusivity group, at most one
 * mod per non-zero group on an item (the game's own transfer replaces a
 * same-group mod). Names are the game's own (string table 10, decoded from the
 * running game), shortened to fit; groups were read from every base item.
 *
 * SAFETY: armor "bag +1/+2/+4" mods change Aya's bag size. Shrinking it would
 * strand items past the new limit (possibly the equipped gun), so a change
 * that lowers the bonus is refused while any of those slots is occupied.
 * Refusals are silent (the value just does not change); the row's footer
 * note tells the player the rule.
 */
#include <stdint.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_item_table.h"

#define PE_SUB                  "Item Editor"
#define PE_MAX_HP_ADDR          0x800C0E06u
#define PE_SLOT_COUNT_ADDR      0x800C0E0Cu
#define PE_WEAPON_SLOT_ADDR     0x800C0E20u
#define PE_ARMOR_SLOT_ADDR      0x800C0E22u
#define PE_AYA_SLOTS_ADDR       0x800C0E48u
#define PE_EQUIP_RECORDS_ADDR   0x800C0EACu
#define PE_BONUS_SLOTS_ADDR     0x8009D018u

#define REC_ITEM_ID   0x04u
#define REC_BASE      0x07u
#define REC_AMMO      0x0Au
#define REC_BONUS     0x0Eu
#define REC_MOD_COUNT 0x14u
#define REC_MODS      0x15u
#define PE_MAX_MODS   10

enum { PE_GUN = 0, PE_ARMOR = 1 };

/* ---- names ---------------------------------------------------------------- */
static const char *const s_gun_mods[21] = {
    "EMPTY",
    "RATE OF FIRE 2", "RATE OF FIRE 3", "RATE OF FIRE 5", "RATE OF FIRE 7",
    "RATE OF FIRE 10", "RANDOM FIRE 1-5", "BURST SPRAY", "ATTACK ALL",
    "HEAT BULLETS", "FROST BULLETS", "CYANIDE BULLETS", "ACID BULLETS",
    "TRANQUILIZER", "COUNTERATTACK", "QUICKDRAW", "STEAL (HALF ATK)",
    "STEAL ON ATTACK", "CRITICAL UP", "2 COMMANDS", "3 COMMANDS",
};
/* Armor effects 16-20 are unused dummies in the game's own table. */
static const char *const s_armor_mods[16] = {
    "EMPTY",
    "RESIST POISON", "RESIST STIFFNESS", "RESIST DARKNESS", "LESS STOLEN",
    "RESIST CONFUSION", "AUTO MEDICINE", "AUTO CURE", "BAG +1", "BAG +2",
    "BAG +4", "MAX HP UP", "ATTACK UP", "SLOWER ACTIVE TIME", "PE BURDEN DOWN",
    "ATTACK DOWN",
};

static int pe_mod_group(int which, int effect) {
    if (which == PE_ARMOR) return (effect >= 8 && effect <= 10) ? 5 : 0;
    if (effect >= 1 && effect <= 5) return 1;   /* rate of fire */
    if (effect >= 6 && effect <= 8) return 2;   /* multi-shot */
    if (effect == 19 || effect == 20) return 3; /* extra commands */
    if (effect == 16 || effect == 17) return 4; /* steal */
    return 0;
}

static const char *pe_item_name(int id) {
    for (unsigned i = 0; i < PE_ITEM_COUNT; i++)
        if (pe_item_ids[i] == id) return pe_item_names[i];
    return "UNKNOWN";
}

/* ---- equipped records ----------------------------------------------------- */
static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

/* Record address of the equipped gun/armor, or 0. */
static uint32_t pe_record(int which) {
    if (!pe_loaded()) return 0;
    const int8_t slot = (int8_t)psx_mod_read_byte(
        which == PE_GUN ? PE_WEAPON_SLOT_ADDR : PE_ARMOR_SLOT_ADDR);
    if (slot < 0 || slot >= 50) return 0;
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u);
    if (v < 256 || v >= 384) return 0;
    return PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * 0x20u;
}

static int pe_mod_count(uint32_t rec) {
    int n = rec ? psx_mod_read_byte(rec + REC_MOD_COUNT) : 0;
    return n > PE_MAX_MODS ? PE_MAX_MODS : n;
}

/* ---- header rows: the equipped item's name -------------------------------- */
static int s_gun_header, s_armor_header;   /* single-entry lists; always 0 */
static unsigned pe_one(void) { return 1; }
static const char *pe_gun_name(unsigned i) {
    (void)i;
    const uint32_t rec = pe_record(PE_GUN);
    return rec ? pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)) : "NONE EQUIPPED";
}
static const char *pe_armor_name(unsigned i) {
    (void)i;
    const uint32_t rec = pe_record(PE_ARMOR);
    return rec ? pe_item_name(psx_mod_read_byte(rec + REC_ITEM_ID)) : "NONE EQUIPPED";
}

/* ---- +N bonuses ----------------------------------------------------------- */
static int pe_get_bonus(int which, int stat) {
    const uint32_t rec = pe_record(which);
    return rec ? (int16_t)psx_mod_read_half(rec + REC_BONUS + (uint32_t)stat * 2u) : 0;
}

/* Loaded ammo never exceeds base bullets + bonus (max 999), as ClampAmmo. */
static void pe_clamp_ammo(uint32_t rec) {
    int cap = psx_mod_read_byte(rec + REC_BASE + 2u) +
              (int16_t)psx_mod_read_half(rec + REC_BONUS + 4u);
    if (cap > 999) cap = 999;
    if (cap < 0) cap = 0;
    if (psx_mod_read_half(rec + REC_AMMO) > (uint16_t)cap)
        psx_mod_write_half(rec + REC_AMMO, (uint16_t)cap);
}

static int pe_set_bonus(int which, int stat, int value) {
    const uint32_t rec = pe_record(which);
    if (!rec) return 0;
    psx_mod_write_half(rec + REC_BONUS + (uint32_t)stat * 2u, (uint16_t)(int16_t)value);
    if (which == PE_GUN && stat == 2) pe_clamp_ammo(rec);
    return 1;
}

static int g0(void) { return pe_get_bonus(PE_GUN, 0); }
static int g1(void) { return pe_get_bonus(PE_GUN, 1); }
static int g2(void) { return pe_get_bonus(PE_GUN, 2); }
static int a0(void) { return pe_get_bonus(PE_ARMOR, 0); }
static int a1(void) { return pe_get_bonus(PE_ARMOR, 1); }
static int a2(void) { return pe_get_bonus(PE_ARMOR, 2); }
static int sg0(int v) { return pe_set_bonus(PE_GUN, 0, v); }
static int sg1(int v) { return pe_set_bonus(PE_GUN, 1, v); }
static int sg2(int v) { return pe_set_bonus(PE_GUN, 2, v); }
static int sa0(int v) { return pe_set_bonus(PE_ARMOR, 0, v); }
static int sa1(int v) { return pe_set_bonus(PE_ARMOR, 1, v); }
static int sa2(int v) { return pe_set_bonus(PE_ARMOR, 2, v); }

/* ---- ammo ----------------------------------------------------------------- */
static int pe_get_ammo(void) {
    const uint32_t rec = pe_record(PE_GUN);
    return rec ? psx_mod_read_half(rec + REC_AMMO) : 0;
}
static int pe_set_ammo(int value) {
    const uint32_t rec = pe_record(PE_GUN);
    if (!rec) return 0;
    psx_mod_write_half(rec + REC_AMMO, (uint16_t)value);
    pe_clamp_ammo(rec);
    return 1;
}

/* ---- mods ----------------------------------------------------------------- */
static int s_slot[2];                      /* selected mod slot, per item */
static char s_slot_names[2][PE_MAX_MODS][12]; /* up to "10 OF 10" */

static unsigned pe_slot_count(int which) {
    const int n = pe_mod_count(pe_record(which));
    return n > 0 ? (unsigned)n : 1;      /* "NO SLOTS" placeholder */
}
static const char *pe_slot_name(int which, unsigned i) {
    const int n = pe_mod_count(pe_record(which));
    if (n == 0) return "NO SLOTS";
    if (i >= PE_MAX_MODS) return "";
    char *s = s_slot_names[which][i];
    /* "1 OF 3" */
    s[0] = (char)('0' + (i + 1) % 10); int k = 1;
    if (i + 1 == 10) { s[0] = '1'; s[1] = '0'; k = 2; }
    s[k++] = ' '; s[k++] = 'O'; s[k++] = 'F'; s[k++] = ' ';
    if (n == 10) { s[k++] = '1'; s[k++] = '0'; } else s[k++] = (char)('0' + n);
    s[k] = '\0';
    return s;
}
static unsigned gsc(void) { return pe_slot_count(PE_GUN); }
static unsigned asc(void) { return pe_slot_count(PE_ARMOR); }
static const char *gsn(unsigned i) { return pe_slot_name(PE_GUN, i); }
static const char *asn(unsigned i) { return pe_slot_name(PE_ARMOR, i); }

static int pe_get_mod(int which) {
    const uint32_t rec = pe_record(which);
    if (!rec || s_slot[which] >= pe_mod_count(rec)) return 0;
    const int effect = psx_mod_read_byte(rec + REC_MODS + (uint32_t)s_slot[which]) & 0x1F;
    const int max = which == PE_GUN ? 20 : 15;
    return effect <= max ? effect : 0;
}

/* Bag bonus the armor's mods give: +1/+2/+4 for effects 8/9/10. */
static int pe_bag_bonus(const uint8_t *mods, int n) {
    for (int i = 0; i < n; i++) {
        const int e = mods[i] & 0x1F;
        if (e >= 8 && e <= 10) return 1 << (e - 8);
    }
    return 0;
}

static int pe_set_mod(int which, int effect) {
    const uint32_t rec = pe_record(which);
    const int n = pe_mod_count(rec);
    if (!rec || n == 0) return 0;
    if (s_slot[which] >= n) s_slot[which] = 0;

    uint8_t mods[PE_MAX_MODS];
    for (int i = 0; i < n; i++) mods[i] = psx_mod_read_byte(rec + REC_MODS + (uint32_t)i);
    uint8_t next[PE_MAX_MODS];
    for (int i = 0; i < n; i++) next[i] = mods[i];

    const int group = effect ? pe_mod_group(which, effect) : 0;
    if (group) {
        /* One mod per group, like the game's own transfer. */
        for (int i = 0; i < n; i++)
            if (i != s_slot[which] && (next[i] & 0x1F) && (next[i] >> 5) == group) {
                next[i] = 0;
            }
    }
    next[s_slot[which]] = effect ? (uint8_t)((group << 5) | effect) : 0;

    if (which == PE_ARMOR) {
        const int before = pe_bag_bonus(mods, n), after = pe_bag_bonus(next, n);
        if (after < before) {
            uint32_t base = psx_mod_read_byte(PE_SLOT_COUNT_ADDR);
            uint32_t new_limit = base + (uint32_t)after, old_limit = base + (uint32_t)before;
            if (new_limit > 50) new_limit = 50;
            if (old_limit > 50) old_limit = 50;
            for (uint32_t s = new_limit; s < old_limit; s++)
                if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + s * 2u))
                    return 0; /* would strand an item */
        }
        if (after != before)
            psx_mod_write_word(PE_BONUS_SLOTS_ADDR, (uint32_t)after); /* g_InvBonusSlotCount */
    }
    for (int i = 0; i < n; i++)
        if (next[i] != mods[i]) psx_mod_write_byte(rec + REC_MODS + (uint32_t)i, next[i]);
    return 1;
}
static int ggm(void) { return pe_get_mod(PE_GUN); }
static int agm(void) { return pe_get_mod(PE_ARMOR); }
static int sgm(int v) { return pe_set_mod(PE_GUN, v); }
static int sam(int v) { return pe_set_mod(PE_ARMOR, v); }

void pe_item_editor_register(void) {
    (void)pe_menu_register_dynamic_choice(PE_SUB, "Gun",
        "The equipped weapon.", &s_gun_header, 1, pe_one, pe_gun_name);
    (void)pe_menu_register_live_int(PE_SUB, "Gun: attack +",
        "Attack bonus. Applies now.", -999, 999, 1, g0, sg0);
    (void)pe_menu_register_live_int(PE_SUB, "Gun: range +",
        "Range bonus. Applies now.", -999, 999, 1, g1, sg1);
    (void)pe_menu_register_live_int(PE_SUB, "Gun: bullets +",
        "Bullet capacity bonus. Applies now.", -999, 999, 1, g2, sg2);
    (void)pe_menu_register_live_int(PE_SUB, "Gun: ammo loaded",
        "Capped at bullet capacity.", 0, 999, 1, pe_get_ammo, pe_set_ammo);
    (void)pe_menu_register_dynamic_choice(PE_SUB, "Gun: mod slot",
        "Pick a slot, then set its mod below.", &s_slot[PE_GUN], PE_MAX_MODS,
        gsc, gsn);
    (void)pe_menu_register_live_choice(PE_SUB, "Gun: mod",
        "One per kind; replaces a match.", s_gun_mods, 21, ggm, sgm);

    (void)pe_menu_register_dynamic_choice(PE_SUB, "Armor",
        "The equipped armor.", &s_armor_header, 1, pe_one, pe_armor_name);
    (void)pe_menu_register_live_int(PE_SUB, "Armor: defense +",
        "Defense bonus. Applies now.", -999, 999, 1, a0, sa0);
    (void)pe_menu_register_live_int(PE_SUB, "Armor: PE defense +",
        "PE defense bonus. Applies now.", -999, 999, 1, a1, sa1);
    (void)pe_menu_register_live_int(PE_SUB, "Armor: crit evasion +",
        "Critical evasion bonus. Applies now.", -999, 999, 1, a2, sa2);
    (void)pe_menu_register_dynamic_choice(PE_SUB, "Armor: mod slot",
        "Pick a slot, then set its mod below.", &s_slot[PE_ARMOR], PE_MAX_MODS,
        asc, asn);
    (void)pe_menu_register_live_choice(PE_SUB, "Armor: mod",
        "Bag mods cannot shrink a full bag.", s_armor_mods, 16, agm, sam);
}
