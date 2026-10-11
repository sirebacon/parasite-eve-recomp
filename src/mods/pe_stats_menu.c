/*
 * Stats editor rows for the F1 Mods menu ("Stats: ...").
 *
 * Every row shows the same 1-based number as the in-game status screen and is
 * changed with left/right; edits apply when F1 closes. While the menu is shut,
 * a VBlank tick mirrors the game's values into the rows, so they always open
 * showing what the game currently has.
 *
 * WHAT IS STORED (decomp aya.h / Aya_StatDerivation.c, tables checked live):
 *   - Character level: u8 at 0x800C0E0A (0-based) plus total EXP at 0x800C0E00.
 *     Setting a level writes both, using the game's EXP table, so "next level"
 *     stays consistent. It does not grant the stats/abilities a real level-up
 *     would (use the stat rows and the ability rows for those).
 *   - Seven per-stat EXP totals, u16 at 0x800C0E28 + 2*k. Each stat's level is
 *     a lookup in its growth table (Stat_QueryLevelAndSubLevel): 10 EXP per
 *     level for all but active time, which has its own curve; max level 99.
 *     Inv_RecalcSlotStats turns those levels into battle values:
 *       0 HP, 1 offense, 2 defense, 3 PE, 4 status recovery, 5 active time,
 *       6 item capacity.
 *     Setting a stat level writes that level's threshold. For HP and item
 *     capacity this also updates the save-side values the game derives from
 *     them (max HP; Aya's slot count), the same way Inv_RecalcSlotStats does;
 *     the in-battle copies are recomputed by the game itself.
 */
#include <stdint.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_TOTAL_EXP_ADDR      0x800C0E00u
#define PE_MAX_HP_ADDR         0x800C0E06u
#define PE_CUR_HP_ADDR         0x800C0E08u
#define PE_LEVEL_ADDR          0x800C0E0Au
#define PE_SLOT_COUNT_ADDR     0x800C0E0Cu
#define PE_STAT_EXP_ADDR       0x800C0E28u /* u16[7] */
#define PE_TABLE_ORIGIN        0x800A8028u
#define PE_GROWTH_OFFSET_ADDR  0x800A8038u /* g_StatGrowthTable: int[128] per stat */
#define PE_LEVELROW_OFFSET_ADDR 0x800A803Cu /* AyaLevelStats[99], 0x18 each */
#define PE_EXP_OFFSET_ADDR     0x800A8040u /* u32 cumulative EXP per level */
#define PE_STAT_MODS_ADDR      0x800A1B30u /* D_800A1B30: per-stat level offsets */
#define PE_MAX_LEVEL           99          /* status-screen numbering */

static int pe_character_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static uint32_t pe_growth_table(int kind) {
    return PE_TABLE_ORIGIN + psx_mod_read_word(PE_GROWTH_OFFSET_ADDR) +
           (uint32_t)kind * 0x200u;
}

static uint32_t pe_level_row(int level0) {
    return PE_TABLE_ORIGIN + psx_mod_read_word(PE_LEVELROW_OFFSET_ADDR) +
           (uint32_t)level0 * 0x18u;
}

/* Stat_QueryLevelAndSubLevel's level: the highest L <= 98 whose threshold
 * is <= value. */
static int pe_stat_level0(int kind, uint32_t value) {
    const uint32_t table = pe_growth_table(kind);
    int level = 0;
    for (int l = 1; l < PE_MAX_LEVEL; l++) {
        if ((int32_t)psx_mod_read_word(table + (uint32_t)l * 4u) > (int32_t)value) break;
        level = l;
    }
    return level;
}

/* ---- the seven stats ----------------------------------------------------- */
typedef struct {
    int value;      /* row value, 1-based level */
    int shadow;     /* last level mirrored from / written to the game */
    int raw;        /* last raw EXP seen, -1 = unknown */
} PeStatRow;

/* Editable stats: 0..5. Item capacity (6) is left to the game; see below. */
#define PE_EDITABLE_STATS 6
static PeStatRow s_stats[PE_EDITABLE_STATS];

/* max HP = (mod + 20) * levelRow(HP stat level).hp / 20, as Inv_RecalcSlotStats;
 * current HP is clamped to the new max, as the game does. */
static void pe_recalc_max_hp(void) {
    const int level0 = pe_stat_level0(0, psx_mod_read_half(PE_STAT_EXP_ADDR));
    const int32_t mod = (int32_t)psx_mod_read_word(PE_STAT_MODS_ADDR);
    const uint32_t row_hp = psx_mod_read_half(pe_level_row(level0));
    const uint16_t max_hp = (uint16_t)(((mod + 20) * (int32_t)row_hp) / 20);
    psx_mod_write_half(PE_MAX_HP_ADDR, max_hp);
    if (psx_mod_read_half(PE_CUR_HP_ADDR) > max_hp)
        psx_mod_write_half(PE_CUR_HP_ADDR, max_hp);
}

static void pe_apply_stat(int kind, int level1) {
    const int level0 = level1 - 1;
    const uint32_t threshold = psx_mod_read_word(pe_growth_table(kind) + (uint32_t)level0 * 4u);
    psx_mod_write_half(PE_STAT_EXP_ADDR + (uint32_t)kind * 2u, (uint16_t)threshold);

    if (kind == 0) pe_recalc_max_hp();
    /*
     * Item capacity (kind 6) is deliberately NOT editable. Lowering it is
     * unsafe: the game never moves items when capacity drops, so anything in
     * a slot past the new limit becomes unreachable, possibly including the
     * equipped weapon/armor, which the game assumes always sits in a valid
     * slot. The game re-applies the stat itself (Inv_RecalcSlotStats ->
     * Inv_SetAyaSlotCount) at its next recalc, so even writing only the stat
     * would shrink the bag. Bag size stays with the game and items.
     *
     * } else if (kind == 6) {
     *     uint8_t cap = psx_mod_read_byte(pe_level_row(level0) + 7u);
     *     if (cap > 50) cap = 50;
     *     psx_mod_write_byte(PE_SLOT_COUNT_ADDR, cap);
     * }
     */
}

/* ---- character level ----------------------------------------------------- */
static int s_level = 1, s_level_shadow = 1;

#define PE_PE_BONUS_POOL_ADDR  0x800C0E1Eu /* banked battle bonus points */
#define PE_BONUS_POINTS_ADDR   0x800C0E10u
#define PE_SPELL_FLAGS_ADDR    0x800C0E24u
#define PE_SPELL_OFFSET_ADDR   0x800A8048u /* {u16 level, u16 cost}[20] */

/*
 * Raising the level does what the game's level-up does (Aya_SetTotalExp and
 * the level-up screen): every stat gains 10 EXP per level gained, the banked
 * bonus-point pool is paid out (capped at 99999) and cleared, and every
 * ability whose unlock level has been reached is unlocked
 * (Aya_UnlockParasiteSpellById walks the same table). Lowering the level has
 * no in-game equivalent, so it only rewinds level and EXP; stats stay put.
 */
static void pe_apply_level(int level1) {
    const int new0 = level1 - 1;
    const int old0 = psx_mod_read_byte(PE_LEVEL_ADDR);
    const uint32_t exp = psx_mod_read_word(PE_TABLE_ORIGIN +
        psx_mod_read_word(PE_EXP_OFFSET_ADDR) + (uint32_t)new0 * 4u);
    psx_mod_write_word(PE_TOTAL_EXP_ADDR, exp);
    psx_mod_write_byte(PE_LEVEL_ADDR, (uint8_t)new0);
    if (new0 <= old0) return;

    const uint32_t gain = (uint32_t)(new0 - old0) * 10u;
    for (int k = 0; k < 7; k++) {
        const uint32_t addr = PE_STAT_EXP_ADDR + (uint32_t)k * 2u;
        uint32_t v = psx_mod_read_half(addr) + gain;
        psx_mod_write_half(addr, (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v));
    }
    pe_recalc_max_hp();

    uint32_t bp = psx_mod_read_word(PE_BONUS_POINTS_ADDR) +
                  psx_mod_read_half(PE_PE_BONUS_POOL_ADDR);
    psx_mod_write_word(PE_BONUS_POINTS_ADDR, bp > 99999u ? 99999u : bp);
    psx_mod_write_half(PE_PE_BONUS_POOL_ADDR, 0);

    const uint32_t spells = PE_TABLE_ORIGIN + psx_mod_read_word(PE_SPELL_OFFSET_ADDR);
    uint32_t flags = psx_mod_read_word(PE_SPELL_FLAGS_ADDR);
    for (uint32_t i = 0; i < 20; i++) {
        const uint32_t e = psx_mod_read_word(spells + i * 4u);
        const uint32_t unlock_level = e & 0xFFFFu, cost = e >> 16;
        if (cost != 0 && unlock_level <= (uint32_t)new0) flags |= 1u << i;
    }
    psx_mod_write_word(PE_SPELL_FLAGS_ADDR, flags);
}

/* Apply rows the player changed in F1, then mirror the game back into them. */
static void pe_stats_tick(void) {
    if (!pe_character_loaded()) {
        /* Drop edits made with no save loaded, so they can't land on the next
         * save that loads. */
        s_level = s_level_shadow;
        for (int k = 0; k < PE_EDITABLE_STATS; k++) s_stats[k].value = s_stats[k].shadow;
        return;
    }

    if (s_level != s_level_shadow) pe_apply_level(s_level);
    s_level = s_level_shadow = psx_mod_read_byte(PE_LEVEL_ADDR) + 1;

    for (int k = 0; k < PE_EDITABLE_STATS; k++) {
        PeStatRow *row = &s_stats[k];
        if (row->value != row->shadow) {
            pe_apply_stat(k, row->value);
            row->raw = -1;
        }
        const int raw = psx_mod_read_half(PE_STAT_EXP_ADDR + (uint32_t)k * 2u);
        if (raw != row->raw) {           /* only search the table on change */
            row->raw = raw;
            row->shadow = pe_stat_level0(k, (uint32_t)raw) + 1;
        }
        row->value = row->shadow;
    }
}

void pe_stats_menu_register(void) {
    /* No "item capacity" row: see the note in pe_apply_stat. */
    static const char *const labels[PE_EDITABLE_STATS] = {
        "Stats: HP level", "Stats: offense level", "Stats: defense level",
        "Stats: PE level", "Stats: status recovery level",
        "Stats: active time level",
    };
    static const char *const descriptions[PE_EDITABLE_STATS] = {
        "Max HP. Applies when F1 closes.",
        "Attack power. Applies when F1 closes.",
        "Defense. Applies when F1 closes.",
        "PE and recharge. Applies on close.",
        "Status recovery. Applies on close.",
        "Turn gauge speed. Applies on close.",
    };
    for (int k = 0; k < PE_EDITABLE_STATS; k++) {
        s_stats[k].value = s_stats[k].shadow = 1;
        s_stats[k].raw = -1;
    }
    (void)pe_menu_register_int("Stats: level",
        "Up = real level-up. Applies on close.",
        &s_level, 1, PE_MAX_LEVEL, 1);
    for (int k = 0; k < PE_EDITABLE_STATS; k++)
        (void)pe_menu_register_int(labels[k], descriptions[k],
                                   &s_stats[k].value, 1, PE_MAX_LEVEL, 1);
    (void)pe_menu_register_tick(pe_stats_tick);
}
