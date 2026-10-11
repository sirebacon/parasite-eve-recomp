/*
 * Debug page: F1 > Mods > PE Enhancements > Debug.
 *
 * Each action runs with the guest paused at a VBlank boundary and edits live
 * guest memory directly. These are development aids, not part of the shipped
 * enhancement set, and every one reports through psx_mod_osd_message so it is
 * obvious whether it did anything.
 *
 * GIVE ITEM mirrors the game's own Inv_AddItem (decomp src/main/item/
 * Inv_InsertItem.c) against Aya's bag, because plugins cannot call guest code:
 *   kinds 1-9   weapons/armor: copy the 0x20-byte base record into a free
 *               equipment record (iconId == 0) and store record+256 in a slot
 *   kinds 10,12-15  plain items: store the item ID in a free slot
 *   kinds 16-18 ammo: add the rounds to that category's pool and make sure a
 *               category+512 slot exists
 *   kind 11 (and anything else): the game itself does nothing, so neither do we
 * The game's version works on whichever list is "active" (Aya or Wayne's
 * storage); this always targets Aya's 50-slot list at 0x800C0E48.
 *
 * ABILITIES are bits in g_AyaParasiteSpellFlags (0x800C0E24). The game sets
 * bit i when Aya's level reaches entry i of the 20-entry unlock table
 * (Aya_UnlockParasiteSpellById); entry i is the tech with ID 0xEC + i.
 *
 * Planned, still needing decomp research: edit stats, warp to a location.
 */
#include <stdint.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_item_table.h"

#define PE_MAX_HP_ADDR              0x800C0E06u
#define PE_SLOT_COUNT_ADDR          0x800C0E0Cu /* u8 base capacity */
#define PE_BONUS_POINTS_ADDR        0x800C0E10u
#define PE_SPELL_FLAGS_ADDR         0x800C0E24u
#define PE_AYA_SLOTS_ADDR           0x800C0E48u /* s16[50] */
#define PE_EQUIP_RECORDS_ADDR       0x800C0EACu /* ItemDataRecord[128] */
#define PE_EQUIP_RECORD_COUNT       128u
#define PE_RECORD_SIZE              0x20u
#define PE_BONUS_SLOTS_ADDR         0x8009D018u /* g_InvBonusSlotCount */
#define PE_ITEM_TABLE_BASE_ADDR     0x800A8034u /* Item_LookupBaseData */
#define PE_ITEM_TABLE_END_ADDR      0x800A8038u
#define PE_ITEM_TABLE_ORIGIN        0x800A8028u /* D_800A8038 - 0x10 */
#define PE_AMMO_POOLS_ADDR          0x800A1E64u /* ItemDataRecord[3] */
#define PE_BONUS_POINTS_GAME_CLAMP  99999u /* the game's own clamp (aya.h) */
#define PE_BONUS_POINTS_GRANT       1000u

/* ItemDataRecord field offsets (decomp include/pe1/inventory.h). */
#define REC_ICON      0x00u
#define REC_KIND      0x06u
#define REC_BASE2     0x09u /* baseStats[2]: bullet capacity */
#define REC_AMMO      0x0Au
#define REC_BONUS2    0x12u /* bonusStats[2] */

/* Same "a real character is loaded" gate as pe_bonus_points_floor.c: max HP
 * is zero before a save is loaded, and writing then would be overwritten. */
static int pe_character_loaded(void) {
    if (psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0)
        return 1;
    psx_mod_osd_message("No save loaded yet");
    return 0;
}

/* ---- bonus points -------------------------------------------------------- */
static void pe_debug_add_bonus_points(void) {
    if (!pe_character_loaded()) return;
    uint32_t points = psx_mod_read_word(PE_BONUS_POINTS_ADDR) + PE_BONUS_POINTS_GRANT;
    if (points > PE_BONUS_POINTS_GAME_CLAMP) points = PE_BONUS_POINTS_GAME_CLAMP;
    psx_mod_write_word(PE_BONUS_POINTS_ADDR, points);
    psx_mod_osd_message("Added bonus points");
}

/* ---- give item ----------------------------------------------------------- */
/* "item type" picks a group; "item" lists only that group (index within it). */
static int s_type_choice;
static int s_item_choice;

static unsigned pe_item_list_count(void) {
    if (s_type_choice < 0 || (unsigned)s_type_choice >= PE_TYPE_COUNT) return 0;
    return pe_type_count[s_type_choice];
}

static const char *pe_item_list_name(unsigned index) {
    if (index >= pe_item_list_count()) return 0;
    return pe_item_names[pe_type_first[s_type_choice] + index];
}

/* Inv_GetAyaSlotLimit: base capacity + armor bonus slots, capped at 50. */
static uint32_t pe_slot_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) +
                 psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

static int pe_find_slot(int16_t value) {
    const uint32_t limit = pe_slot_limit();
    for (uint32_t i = 0; i < limit; i++)
        if ((int16_t)psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == value)
            return (int)i;
    return -1;
}

static void pe_set_slot(int slot, int16_t value) {
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, (uint16_t)value);
}

/* Item_LookupBaseData(id - 1); 0 when the ID is past the table. */
static uint32_t pe_base_record(int id) {
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
    const uint32_t end  = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
    const uint32_t index = (uint32_t)(id - 1);
    if (end <= base || index >= ((end - base) >> 5)) return 0;
    return PE_ITEM_TABLE_ORIGIN + base + index * PE_RECORD_SIZE;
}

static void pe_copy_record(uint32_t dst, uint32_t src) {
    for (uint32_t o = 0; o < PE_RECORD_SIZE; o += 4)
        psx_mod_write_word(dst + o, psx_mod_read_word(src + o));
}

/* Returns the OSD message describing the outcome. */
static const char *pe_give_item(int id) {
    const uint32_t rec = pe_base_record(id);
    if (!rec) return "Unknown item";
    const uint8_t kind = psx_mod_read_byte(rec + REC_KIND);
    const int free_slot = pe_find_slot(0);

    if (kind >= 1 && kind <= 9) {
        uint32_t r = 0;
        while (r < PE_EQUIP_RECORD_COUNT &&
               psx_mod_read_byte(PE_EQUIP_RECORDS_ADDR + r * PE_RECORD_SIZE + REC_ICON))
            r++;
        if (r >= PE_EQUIP_RECORD_COUNT) return "No free equipment record";
        if (free_slot < 0) return "Inventory full";
        pe_copy_record(PE_EQUIP_RECORDS_ADDR + r * PE_RECORD_SIZE, rec);
        pe_set_slot(free_slot, (int16_t)(r + 256));
        return "Item added";
    }
    if (kind == 10 || (kind >= 12 && kind <= 15)) {
        if (free_slot < 0) return "Inventory full";
        pe_set_slot(free_slot, (int16_t)id);
        return "Item added";
    }
    if (kind >= 16 && kind <= 18) {
        const uint32_t category = kind - 16u;
        const uint32_t pool = PE_AMMO_POOLS_ADDR + category * PE_RECORD_SIZE;
        int added_slot = 1;
        if (pe_find_slot((int16_t)(category + 512)) < 0) {
            if (free_slot >= 0) pe_set_slot(free_slot, (int16_t)(category + 512));
            else added_slot = 0;
        }
        uint32_t ammo = psx_mod_read_half(pool + REC_AMMO) +
                        psx_mod_read_half(rec + REC_AMMO);
        const int32_t cap = psx_mod_read_byte(pool + REC_BASE2) +
                            (int16_t)psx_mod_read_half(pool + REC_BONUS2);
        const uint32_t limit = cap < 1000 ? (uint32_t)(cap < 0 ? 0 : cap) : 999u;
        if (ammo > limit) ammo = limit;
        psx_mod_write_half(pool + REC_AMMO, (uint16_t)ammo);
        return added_slot ? "Ammo added" : "Ammo added (no slot to show it)";
    }
    return "The game can't add this item";
}

static void pe_debug_give_item(void) {
    if (!pe_character_loaded()) return;
    if (s_item_choice < 0 || (unsigned)s_item_choice >= pe_item_list_count()) return;
    psx_mod_osd_message(pe_give_item(
        pe_item_ids[pe_type_first[s_type_choice] + s_item_choice]));
}

/* ---- abilities ----------------------------------------------------------- */
static int s_tech_choice;

static void pe_unlock_tech_bits(uint32_t bits) {
    psx_mod_write_word(PE_SPELL_FLAGS_ADDR,
                       psx_mod_read_word(PE_SPELL_FLAGS_ADDR) | bits);
}

static void pe_debug_unlock_ability(void) {
    if (!pe_character_loaded()) return;
    if (s_tech_choice < 0 || (unsigned)s_tech_choice >= PE_TECH_COUNT) return;
    pe_unlock_tech_bits(1u << (pe_tech_ids[s_tech_choice] - 0xEC));
    psx_mod_osd_message("Ability unlocked");
}

static void pe_debug_unlock_all_abilities(void) {
    if (!pe_character_loaded()) return;
    uint32_t bits = 0;
    for (unsigned i = 0; i < PE_TECH_COUNT; i++)
        bits |= 1u << (pe_tech_ids[i] - 0xEC);
    pe_unlock_tech_bits(bits);
    psx_mod_osd_message("All abilities unlocked");
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_debug_menu_register(void) {
    (void)pe_menu_register_action("Debug", "Add 1000 bonus points",
        "Spend on the stat screen (max 99999).",
        pe_debug_add_bonus_points);
    (void)pe_menu_register_choice("Debug", "Item type",
        "Left/right picks the item group.",
        &s_type_choice, pe_type_names, PE_TYPE_COUNT);
    (void)pe_menu_register_dynamic_choice("Debug", "Item",
        "Left/right picks, then give item.",
        &s_item_choice, PE_TYPE_MAX_ITEMS, pe_item_list_count, pe_item_list_name);
    (void)pe_menu_register_action("Debug", "Give item",
        "Adds the item above to Aya's bag.",
        pe_debug_give_item);
    (void)pe_menu_register_choice("Debug", "Ability",
        "Left/right picks a Parasite Energy.",
        &s_tech_choice, pe_tech_names, PE_TECH_COUNT);
    (void)pe_menu_register_action("Debug", "Unlock ability",
        "Unlocks the ability chosen above.",
        pe_debug_unlock_ability);
    (void)pe_menu_register_action("Debug", "Unlock all abilities",
        "Unlocks every Parasite Energy.",
        pe_debug_unlock_all_abilities);
}
