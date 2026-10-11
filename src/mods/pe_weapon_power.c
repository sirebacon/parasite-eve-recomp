/*
 * Weapon power in the game's own menu.
 *
 * When a gun is highlighted, the game's menu title bar shows its name
 * (Menu_DrawContextHelpText 0x8004C608 -> Draw_PrintRawText(Str_LookupTableC(
 * itemId - 1))). A build-time function-entry hook on Draw_PrintRawText
 * (game.toml [recompiler] mod_function_entry_funcs) lets this plugin swap the
 * text pointer, only for calls coming from that function, for a copy of the
 * name with "  PWR n" appended. The copy lives in mod memory (Expansion 1),
 * which the game's own loads can read.
 *
 * Power per turn, from the battle damage code (Battle_StepEnemyMovement):
 *   per shot = (Aya attack / 5 + gun attack) * shotPercent[shots] / 100
 *   per turn = shots * per shot
 * before the enemy's defense (subtracted as defense / shots per shot) and
 * weaknesses. Aya attack = her Combatant stat1E (player -> core + 0x1E, kept
 * current in the field by Inv_RecalcSlotStats); gun attack = base[0] +
 * bonus[0] (max 999), halved by mod effect 16 (Inv_BuildWeaponList); shots =
 * 1, or D_800923D0[effect] for a group-1 mod (effects 1-5: 2/3/5/7/10).
 *
 * The gun's record: the equipped gun if the name matches it, else the first
 * matching gun in Aya's bag, else the item's base data.
 */
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_DRAW_PRINT_RAW_TEXT   0x8005F27Cu
#define PE_HELP_TEXT_FN_START    0x8004C608u /* Menu_DrawContextHelpText */
#define PE_HELP_TEXT_FN_END      0x8004CC50u

#define PE_STR_ORIGIN            0x800A8028u
#define PE_STR_DIRECTORY_OFFSET  0x800A802Cu
#define PE_ITEM_TABLE_BASE_ADDR  0x800A8034u
#define PE_ITEM_COUNT            0xEBu
#define PE_SHOTS_TABLE           0x800923D0u

#define PE_MAX_HP_ADDR           0x800C0E06u
#define PE_WEAPON_SLOT_ADDR      0x800C0E20u
#define PE_AYA_SLOTS_ADDR        0x800C0E48u
#define PE_AYA_SLOT_COUNT        50
#define PE_EQUIP_RECORDS_ADDR    0x800C0EACu
#define PE_PLAYER_PTR_ADDR       0x8009D254u
#define COMBATANT_ATTACK         0x1Eu

#define REC_ITEM_ID   0x04u
#define REC_KIND      0x06u
#define REC_BASE0     0x07u
#define REC_BONUS0    0x0Eu
#define REC_MOD_COUNT 0x14u
#define REC_MODS      0x15u
#define REC_MAX_MODS  11

#define PE_TEXT_MAX   64

static int s_show_power = 1;
static uint32_t s_text_buf;            /* mod memory for the swapped string */

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_ram_pointer(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u;
}

/* ---- game text ------------------------------------------------------------- */
/* Address of Str_LookupTableC(index), or 0. */
static uint32_t pe_item_name_ptr(unsigned index) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory)) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + 0x0Cu);
    if (!pe_ram_pointer(table) || index >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + index * 2u);
}

/* Item id whose name lives at `text`, or 0. Names don't move once loaded, so
 * the last hit is checked first. */
static unsigned pe_item_for_name(uint32_t text) {
    static unsigned s_last;
    if (s_last && pe_item_name_ptr(s_last - 1u) == text) return s_last;
    for (unsigned id = 1; id <= PE_ITEM_COUNT; ++id) {
        if (pe_item_name_ptr(id - 1u) == text) return s_last = id;
    }
    return 0;
}

static unsigned pe_put_number(uint8_t *out, unsigned n, unsigned value) {
    char digits[6];
    unsigned count = 0;
    do { digits[count++] = (char)(value % 10u); value /= 10u; } while (value && count < 6);
    while (count && n < PE_TEXT_MAX - 1) out[n++] = (uint8_t)digits[--count];
    return n;
}

/* ---- the gun's numbers ------------------------------------------------------ */
static uint32_t pe_base_record(unsigned id) {
    return PE_STR_ORIGIN + psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR) + (id - 1u) * 0x20u;
}

static uint32_t pe_equip_record_in_slot(int slot) {
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u);
    if (v < 256 || v >= 384) return 0;
    return PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * 0x20u;
}

static uint32_t pe_record_for_gun(unsigned id) {
    const int8_t equipped = (int8_t)psx_mod_read_byte(PE_WEAPON_SLOT_ADDR);
    if (equipped >= 0 && equipped < PE_AYA_SLOT_COUNT) {
        const uint32_t rec = pe_equip_record_in_slot(equipped);
        if (rec && psx_mod_read_byte(rec + REC_ITEM_ID) == id) return rec;
    }
    for (int slot = 0; slot < PE_AYA_SLOT_COUNT; ++slot) {
        const uint32_t rec = pe_equip_record_in_slot(slot);
        if (rec && psx_mod_read_byte(rec + REC_ITEM_ID) == id) return rec;
    }
    return pe_base_record(id);
}

static unsigned pe_power_per_turn(uint32_t rec) {
    static const uint8_t shot_percent[11] = { 0, 100, 60, 41, 0, 25, 0, 18, 0, 0, 13 };
    int attack = psx_mod_read_byte(rec + REC_BASE0) +
                 (int16_t)psx_mod_read_half(rec + REC_BONUS0);
    if (attack > 999) attack = 999;
    unsigned shots = 1;
    int count = psx_mod_read_byte(rec + REC_MOD_COUNT);
    if (count > REC_MAX_MODS) count = REC_MAX_MODS;
    for (int i = 0; i < count; ++i) {
        const unsigned effect = psx_mod_read_byte(rec + REC_MODS + (uint32_t)i) & 31u;
        if (effect >= 1 && effect <= 5) shots = psx_mod_read_byte(PE_SHOTS_TABLE + effect);
        if (effect == 16) attack >>= 1;
    }
    if (shots > 10 || shot_percent[shots] == 0) shots = 1;

    unsigned aya_attack = 0;
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (pe_ram_pointer(player)) {
        const uint32_t core = psx_mod_read_word(player);
        if (pe_ram_pointer(core)) aya_attack = psx_mod_read_half(core + COMBATANT_ATTACK);
    }
    const int per_shot = ((int)(aya_attack / 5u) + attack) * shot_percent[shots] / 100;
    return per_shot > 0 ? (unsigned)per_shot * shots : 0u;
}

/* ---- the hook --------------------------------------------------------------- */
static void pe_on_print_raw_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t ra = cpu->gpr[31];
    if (ra < PE_HELP_TEXT_FN_START || ra >= PE_HELP_TEXT_FN_END) return;
    if (!s_show_power || !pe_loaded()) return;
    const uint32_t text = cpu->gpr[4];
    if (!pe_ram_pointer(text)) return;
    const unsigned id = pe_item_for_name(text);
    if (!id) return;
    const unsigned kind = psx_mod_read_byte(pe_base_record(id) + REC_KIND);
    if (kind < 1 || kind > 8) return;                 /* guns only */
    if (!s_text_buf) s_text_buf = psx_mod_alloc_guest_memory(PE_TEXT_MAX, 4);
    if (!s_text_buf) return;

    uint8_t out[PE_TEXT_MAX];
    unsigned n = 0;
    for (uint8_t c; n < PE_TEXT_MAX - 12 && (c = psx_mod_read_byte(text + n)) != 0xFF; ) out[n++] = c;
    static const uint8_t label[] = { 0x0F, 0x0F, 0x1F, 0x26, 0x21, 0x0F }; /* "  PWR " */
    for (unsigned i = 0; i < sizeof label; ++i) out[n++] = label[i];
    n = pe_put_number(out, n, pe_power_per_turn(pe_record_for_gun(id)));
    out[n++] = 0xFF;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_byte(s_text_buf + i, out[i]);
    cpu->gpr[4] = s_text_buf;
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_weapon_power_menu_register(void) {
    (void)pe_menu_register_toggle(0, "Weapon power",
        "Shows gun power per turn in menus.", &s_show_power, 0);
}

PSX_MOD_CONSTRUCTOR(pe_register_weapon_power_hook) {
    (void)psx_mod_register_function_entry_plugin("pe.weapon-power",
        PE_DRAW_PRINT_RAW_TEXT, pe_on_print_raw_text);
}
