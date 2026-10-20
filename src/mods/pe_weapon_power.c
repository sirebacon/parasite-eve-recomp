/*
 * Weapon damage per second in the game's own menu.
 *
 * When a gun is highlighted, the game's menu title bar shows its line
 * (Menu_DrawContextHelpText 0x8004C608 -> Draw_PrintRawText(Str_LookupTableC(
 * itemId - 1))). A build-time function-entry hook on Draw_PrintRawText
 * (game.toml [recompiler] mod_function_entry_funcs) lets this plugin swap the
 * text pointer, only for calls coming from that function, for a copy with
 * "  DPS n" appended. The copy lives in mod memory (Expansion 1), which the
 * game's own loads can read.
 *
 * Damage, from the battle damage code (Battle_StepEnemyMovement):
 *   per shot = (Aya attack / 5 + gun attack) * shotPercent[shots] / 100
 *   per volley = shots * per shot; a turn is 1, 2 or 3 volleys (commands,
 *   mod effects 19 / 20: Battle_Init)
 * before the enemy's defense (subtracted as defense / shots per shot) and
 * weaknesses. Aya attack = her Combatant stat1E (player -> core + 0x1E, kept
 * current in the field by Inv_RecalcSlotStats); gun attack = base[0] +
 * bonus[0] (max 999), halved by mod effect 16 (Inv_BuildWeaponList); shots =
 * 1, or D_800923D0[effect] for a group-1 mod (effects 1-5: 2/3/5/7/10).
 *
 * Reloads (Battle_StepCharacterAction): a volley spends one loaded round a
 * shot (an empty chamber only clicks), and a command that starts on an empty
 * magazine reloads (BattleCmd_CommitAmmoAndUpdate, action mode 0xC) and deals
 * nothing. So with capacity C (base[2] + bonus[2]) a cycle is ceil(C / shots)
 * firing commands plus one reload, and deals C rounds' damage: sustained
 * damage a turn = commands * C * per shot / (ceil(C / shots) + 1). Melee
 * (kind 8) never reloads.
 *
 * Time (Menu_MainUpdate): Aya's turn gauge fills 0..9000 by scaledOffense a
 * battle frame, which an action sets to stat22 * scale[action] / 10
 * (Battle_SetupEnemyAnims); a gun attack is action 6, scale 4, the same for
 * every gun. stat22 = 50 + her Active Time row (Aya_LookupLevelStats). So a
 * turn after attacking takes C / (stat22 * 0.4) seconds, where C = fill time
 * x fill rate = 9000 / battle frames a second.
 *
 * C is measured, not assumed: on every guest VBlank (game time, whatever the
 * host speed) during a battle, a fill of Aya's gauge from empty to full at a
 * steady rate (no Haste or Slow: stateFlags +0x4C & 0x1C0, no pause) gives
 * C = seconds x scaledOffense. Until the first such fill it is 300 (the
 * battle loop's VSync(2): 30 frames a second). So a 60 fps logic patch, or a
 * speed patch that changes the gauge's step, is picked up by itself. Haste
 * and Slow (battle-only) aren't counted in the figure.
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
#define COMBATANT_STAT22         0x22u     /* 50 + Active Time row: the gauge speed */
#define COMBATANT_GAUGE          0x10u     /* turn gauge 0..9000 */
#define COMBATANT_FILL_RATE      0x24u     /* scaledOffense: gauge step a battle frame */
#define COMBATANT_STATE_FLAGS    0x4Cu     /* & 0xC0: Slow, & 0x100: Haste */
#define PE_GAUGE_FULL            9000u
#define PE_GAME_BLOCK_ADDR       0x8009D1A0u   /* & 2: in battle */
#define PE_GAME_STATE_FLAGS      0x8009D1A0u
#define PE_VBLANKS_X10_PER_SEC   599u          /* NTSC 59.94 */
#define PE_DEFAULT_CLOCK_X10     3000u         /* C x10: 9000 / 30 fps */

#define REC_ITEM_ID   0x04u
#define REC_KIND      0x06u
#define REC_BASE0     0x07u
#define REC_BASE2     0x09u                  /* capacity (bullets) */
#define REC_BONUS0    0x0Eu
#define REC_BONUS2    0x12u
#define REC_MOD_COUNT 0x14u
#define REC_MODS      0x15u
#define REC_MAX_MODS  11

#define PE_TEXT_MAX   64

static int s_show_power = 1;
static uint32_t s_text_buf;            /* mod memory for the swapped string */
static uint32_t s_clock_x10 = PE_DEFAULT_CLOCK_X10;   /* C x10, measured in battle */
static uint32_t s_clock_samples;

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

/* pe_mod_effects.c: the rebalance's G3A3 / M203 4 attack bonus and installed-RoF volley %. */
int pe_fx_attack_bonus(unsigned gun);
int pe_fx_rof_volley_pct(void);

/* Damage per second x10 (sustained: commands, reloads, Aya's turn speed). */
static unsigned pe_dps_x10(uint32_t rec) {
    static const uint8_t shot_percent[11] = { 0, 100, 60, 41, 0, 25, 0, 18, 0, 0, 13 };
    int attack = psx_mod_read_byte(rec + REC_BASE0) +
                 (int16_t)psx_mod_read_half(rec + REC_BONUS0);
    if (attack > 999) attack = 999;
    attack += pe_fx_attack_bonus(psx_mod_read_byte(rec + REC_ITEM_ID));
    int installed_rof = 0;
    int capacity = psx_mod_read_byte(rec + REC_BASE2) + (int16_t)psx_mod_read_half(rec + REC_BONUS2);
    if (capacity > 999) capacity = 999;
    unsigned shots = 1, commands = 1;
    int count = psx_mod_read_byte(rec + REC_MOD_COUNT);
    if (count > REC_MAX_MODS) count = REC_MAX_MODS;
    for (int i = 0; i < count; ++i) {
        const unsigned code = psx_mod_read_byte(rec + REC_MODS + (uint32_t)i);
        const unsigned effect = code & 31u;
        if (effect >= 1 && effect <= 5) shots = psx_mod_read_byte(PE_SHOTS_TABLE + effect);
        if (effect >= 3 && effect <= 5 && !(code & 0x20u)) installed_rof = 1;   /* R-E applies */
        if (effect == 16) attack >>= 1;
        if (effect == 19) commands = 2;
        if (effect == 20) commands = 3;
    }
    if (shots > 10 || shot_percent[shots] == 0) shots = 1;

    unsigned aya_attack = 0, stat22 = 50;
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (pe_ram_pointer(player)) {
        const uint32_t core = psx_mod_read_word(player);
        if (pe_ram_pointer(core)) {
            aya_attack = psx_mod_read_half(core + COMBATANT_ATTACK);
            const unsigned s = psx_mod_read_half(core + COMBATANT_STAT22);
            if (s >= 1 && s < 1000) stat22 = s;
        }
    }
    const int volley = installed_rof && shots >= 5u ? pe_fx_rof_volley_pct() : 0;
    const int per_shot = volley > 0 ? ((int)(aya_attack / 5u) + attack) * volley / (int)(shots * 100u)
                                    : ((int)(aya_attack / 5u) + attack) * shot_percent[shots] / 100;
    if (per_shot <= 0) return 0;

    /* Damage a turn, x10, with reloads for guns. */
    uint64_t turn_x10;
    const unsigned kind = psx_mod_read_byte(rec + REC_KIND);
    if (kind == 8 || capacity <= 0) {
        turn_x10 = (uint64_t)commands * shots * (unsigned)per_shot * 10u;
    } else {
        const unsigned cycle = ((unsigned)capacity + shots - 1u) / shots + 1u;   /* + the reload */
        turn_x10 = (uint64_t)commands * (unsigned)capacity * (unsigned)per_shot * 10u / cycle;
    }
    /* DPS = a turn's damage / (C / (stat22 * 0.4)) */
    return (unsigned)(turn_x10 * stat22 * 4u / s_clock_x10);
}

/* ---- the battle clock ------------------------------------------------------- */
/* Every guest VBlank: time a steady fill of Aya's gauge from empty to full. */
static void pe_clock_vblank(void) {
    static uint32_t vblanks, start, last_change;
    static unsigned prev_gauge, rate;
    static int timing, clean;
    ++vblanks;
    if (!pe_loaded() || !(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 2u)) { timing = 0; return; }
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (!pe_ram_pointer(player)) { timing = 0; return; }
    const uint32_t core = psx_mod_read_word(player);
    if (!pe_ram_pointer(core)) { timing = 0; return; }
    const unsigned gauge = psx_mod_read_half(core + COMBATANT_GAUGE);
    const unsigned fill = psx_mod_read_half(core + COMBATANT_FILL_RATE);
    const uint32_t flags = psx_mod_read_word(core + COMBATANT_STATE_FLAGS);

    if (gauge < prev_gauge || (gauge == 0 && !timing)) {     /* emptied: an action */
        timing = 1; clean = 1; start = vblanks; last_change = vblanks; rate = fill;
    } else if (timing) {
        if (gauge != prev_gauge) last_change = vblanks;
        if ((flags & 0x1C0u) || fill != rate || fill == 0) clean = 0;   /* Haste / Slow / new rate */
        if (vblanks - last_change > 6u) clean = 0;                     /* paused (a menu) */
        if (gauge >= PE_GAUGE_FULL) {
            if (clean && vblanks > start) {
                /* C x10 = seconds x rate x10 = vblanks / 59.94 x rate x10 */
                const uint32_t c_x10 = (uint32_t)((uint64_t)(vblanks - start) * rate * 100u /
                                                  PE_VBLANKS_X10_PER_SEC);
                if (c_x10 >= 500u && c_x10 <= 20000u) {
                    s_clock_x10 = s_clock_samples ? (s_clock_x10 * 3u + c_x10) / 4u : c_x10;
                    ++s_clock_samples;
                }
            }
            timing = 0;
        }
    }
    prev_gauge = gauge;
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
    static const uint8_t label[] = { 0x0F, 0x0F, 0x13, 0x1F, 0x22, 0x0F }; /* "  DPS " */
    for (unsigned i = 0; i < sizeof label; ++i) out[n++] = label[i];
    const unsigned dps = pe_dps_x10(pe_record_for_gun(id));
    if (dps < 100u) {                         /* one decimal below 10: "2.7" */
        n = pe_put_number(out, n, dps / 10u);
        out[n++] = 0x2F;                      /* '.' */
        n = pe_put_number(out, n, dps % 10u);
    } else {
        n = pe_put_number(out, n, (dps + 5u) / 10u);
    }
    out[n++] = 0xFF;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_byte(s_text_buf + i, out[i]);
    cpu->gpr[4] = s_text_buf;
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
static int pe_get_turn_tenths(void) { return (int)(s_clock_x10 * 10u / (50u * 4u)); }
static int pe_readonly(int value) { (void)value; return 0; }

void pe_weapon_power_menu_register(void) {
    (void)pe_menu_register_toggle(0, "Weapon power",
        "Shows gun damage per second in menus.", &s_show_power, 0);
    (void)pe_menu_register_live_int("Debug", "DPS clock (turn, 0.1 s)",
        "After an attack at Active Time 1; measured in battle.", 0, 100000, 1,
        pe_get_turn_tenths, pe_readonly);
}

PSX_MOD_CONSTRUCTOR(pe_register_weapon_power_hook) {
    (void)psx_mod_register_function_entry_plugin("pe.weapon-power",
        PE_DRAW_PRINT_RAW_TEXT, pe_on_print_raw_text);
    (void)psx_mod_register_vblank_plugin("pe.weapon-power.clock", pe_clock_vblank);
}
