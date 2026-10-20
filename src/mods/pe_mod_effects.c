/*
 * Mod effects rebalance (bridge findings pe-mod-effects-overhaul-2026-10-14.md, proposal v2;
 * simulators tools/pe_mod_sim.py and tools/pe_combo_sim.py). F1 > PE Enhancements > Mod Effects.
 *
 * Rules: Vanilla (this file does nothing) / Rebalanced (default) / Custom (any row changed).
 *
 * Aya's gun hits: Battle_StepEnemyMovement (0x80028574, legacy name: the damage routine for
 * Aya's attack on one enemy; a0 = the target entity, its core at +0) computes
 *   damage = ((stat1E / 5 + action power bonus) x hit% - enemy defense / shots) x quality x element
 * and subtracts it in the same call. At its entry, for this one hit, we
 *   - scale BOTH the attack term (via the action's power bonus) and the enemy's defense by the
 *     element multiplier M we want, and clear the enemy's Heat / Frost bits so the game adds no
 *     element factor of its own: damage x M exactly (Heat / Frost: neutral / weak / resist, and
 *     Heat + Frost takes the better of the two);
 *   - scale the attack term alone for Rate of Fire 7 / 10 (per-shot 18% -> 20%, 13% -> 15%);
 *   - pre-roll Acid / Tranquilizer: the enemy's status bits for this hit become "always" (2) or
 *     "never" (1) from our chances (bosses unchanged; Cyanide unchanged);
 *   - with Critical on the gun, turn some normal hits (reaction 2) into criticals (reaction 3,
 *     x1.5): the game's +20 points become ~our setting;
 * and put back exactly the bits / fields we changed at the next hit or the next battle frame
 * (Battle_Update, 0x800299CC).
 *
 * Rebalance (2026-10-19, bridge findings pe-rebalance-economy-basis.md), same hook:
 *   - R-B / R-C: +45 / +15 attack while the equipped gun is the G3A3 (0x7D) / M203 4 (0x86);
 *   - R-F (2026-10-20): "Progression gun bonuses" ON: M16A1 +25, SG550 +15, M203 +20, M203 2 +15
 *     (bridge findings pe-gun-ladder-2026-10-20.md);
 *   - R-E (power): installed RoF 5 / 7 / 10 (effects 3-5 the base item doesn't have) deal
 *     "Installed RoF volley %" (200) for the whole volley instead of 25 / 18 / 13% a shot;
 *   - R-A: in a fight with 2+ enemies alive and no boss, an Attack All turn ((turn & 0xC0) ==
 *     0xC0) can take at most "Attack All crowd cap %" (75) of a target's max HP (hpAliveCap
 *     +0x88): its HP at the turn's first Attack All hit is the reference, and each hit's power
 *     bonus is lowered just enough that the game's damage (replicated: pe_hit_damage) keeps it
 *     above the line; a minimum-damage hit that still crosses is topped back up at the next hook.
 *
 * Per battle (VBlank tick): poison strength (each enemy's periodicDamage, +0x96, scaled once);
 * Quickdraw top-up at the start (the game's chance is Aya's stat22 %; we roll the difference up to
 * stat22 + bonus, max 90%); damage taken with Attack Down / Slow Active Time armor (HP refunded as
 * hits land). Counter is no longer boosted: the value its roll reads is Aya's HP level, and
 * raising it lost HP on level-ups (see pe_battle_tick, 2026-10-16). One
 * "modfx" play-log row per battle counts what was changed.
 */
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"
#include <stdio.h>

#define PE_FN_DAMAGE          0x80028574u   /* Battle_StepEnemyMovement */
#define PE_FN_BATTLE_UPDATE   0x800299CCu   /* Battle_Update */
#define PE_ACTIVE_ACTOR       0x8009D278u   /* g_ActiveActor (Combatant *) */
#define PE_PLAYER_ENTITY      0x8009D254u   /* D_8009D254: Aya's entity; core at +0 */
#define PE_GAME_BLOCK         0x8009D1A0u   /* & 2: in battle */
#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_AGILITY            0x800C0E28u   /* g_AyaStatAgility[0]: read only by the Counter roll */
#define PE_ENEMY_SLOT_BASE    0x800A5D58u   /* 7 x { u8 active; pad[3]; EnemyCombatant } */
#define PE_ENEMY_SLOT_STRIDE  0xDCu
#define PE_ENEMY_SLOTS        7
/* Combatant */
#define C_CORE_FLAGS  0x00u
#define C_HP          0x0Cu                 /* Aya: current HP (s16) */
#define C_GAUGE       0x10u                 /* Aya: ATB gauge, full at 9000; enemy: HP (s32) */
#define C_ENTITY_ID   0x07u
#define C_STAT1E      0x1Eu
#define C_STAT22      0x22u
#define C_STATE       0x4Cu
#define C_ACTION      0x68u
#define C_DEFENSE     0x8Cu
#define C_POISON      0x96u                 /* periodicDamage */
#define C_STATUS2     0xCCu
/* BattleAction */
#define A_POWER       0x00u
#define A_TURN        0x10u

enum { RULES_VANILLA, RULES_REBALANCED, RULES_CUSTOM };
static const char *const s_rules_names[] = { "VANILLA", "REBALANCED", "CUSTOM" };
static const char *const s_onoff[] = { "OFF", "ON" };

/* Settings: { vanilla, rebalanced } per row. */
typedef struct { const char *label, *desc; int vanilla, rebalanced, min, max, step; int *value; int choice; } PeRow;
static int s_rules = RULES_REBALANCED;
static int s_rules_shown = -1;
static int s_elem_neutral, s_elem_weak, s_elem_resist, s_elem_best;
static int s_rof7, s_rof10, s_status_resist, s_status_average, s_poison, s_crit, s_quickdraw;
static int s_guard_attack_down, s_guard_slow_at, s_counter;
/* Rebalance 2026-10-19 (bridge findings pe-rebalance-economy-basis.md R-A / R-B / R-C / R-E). */
static int s_g3a3_bonus, s_m203_4_bonus, s_aa_cap, s_rof_volley, s_fast_volley;
/* R-F (2026-10-20, bridge findings pe-gun-ladder-2026-10-20.md): progression gun bonuses. */
static int s_prog_bonus;
static PeRow s_rows[] = {
    { "Heat/Frost neutral %", "Damage vs enemies without a match.", 100, 115, 50, 200, 5, &s_elem_neutral, 0 },
    { "Heat/Frost weak %", "Damage vs enemies weak to it.", 150, 150, 50, 300, 5, &s_elem_weak, 0 },
    { "Heat/Frost resist %", "Damage vs enemies that resist it.", 20, 80, 10, 150, 5, &s_elem_resist, 0 },
    { "Heat+Frost best of two", "Both on: the better one counts.", 0, 1, 0, 1, 1, &s_elem_best, 1 },
    { "RoF 7 per-shot %", "Each of 7 shots (vanilla 18).", 18, 20, 10, 40, 1, &s_rof7, 0 },
    { "RoF 10 per-shot %", "Each of 10 shots (vanilla 13).", 13, 15, 8, 30, 1, &s_rof10, 0 },
    { "Installed RoF volley %", "RoF 5/7/10 mods: whole volley.", 0, 200, 0, 300, 10, &s_rof_volley, 0 },
    { "G3A3 attack bonus", "Added to the G3A3's attack.", 0, 45, 0, 100, 5, &s_g3a3_bonus, 0 },
    { "M203 4 attack bonus", "Added to the M203 4's attack.", 0, 15, 0, 100, 5, &s_m203_4_bonus, 0 },
    { "Progression gun bonuses", "Weaker guns: +15 to +25 attack.", 0, 1, 0, 1, 1, &s_prog_bonus, 1 },
    { "Attack All crowd cap %", "Max HP lost a turn (100 = off).", 100, 75, 25, 100, 5, &s_aa_cap, 0 },
    { "RoF mods: fast volleys", "Installed RoF 5/7/10 fire fast.", 0, 1, 0, 1, 1, &s_fast_volley, 1 },
    { "Acid/Tranq vs resist %", "Chance vs resistant (vanilla 0).", 0, 20, 0, 100, 5, &s_status_resist, 0 },
    { "Acid/Tranq vs normal %", "Chance vs others (vanilla 50).", 50, 60, 0, 100, 5, &s_status_average, 0 },
    { "Poison strength %", "Acid poison damage per tick.", 100, 125, 50, 300, 5, &s_poison, 0 },
    { "Critical bonus points", "Crit chance the effect adds (20).", 20, 40, 0, 80, 5, &s_crit, 0 },
    { "Quickdraw bonus points", "Added to the speed-stat chance.", 0, 15, 0, 90, 5, &s_quickdraw, 0 },
    { "Counter chance %", "Off for now (no effect).", 100, 100, 100, 100, 50, &s_counter, 0 },
    { "Attack Down dmg taken %", "Armor: damage Aya takes.", 100, 75, 50, 100, 5, &s_guard_attack_down, 0 },
    { "Slow AT dmg taken %", "Armor: damage Aya takes.", 100, 85, 50, 100, 5, &s_guard_slow_at, 0 },
};
#define PE_ROWS ((int)(sizeof s_rows / sizeof s_rows[0]))

/* ---- per-hit changes, restored after -------------------------------------------- */
static struct {
    int active;
    uint32_t enemy, action;
    uint32_t s2_mask, s2_orig;
    uint16_t def_orig, power_orig;
    int def_set, power_set;
    int32_t hp_before;                     /* self-check: the hit's predicted damage vs what happened */
    int pred;
} s_hit;

/* Hit quality by distance (any rules): the reaction the game picked (core flags >> 15: 1 graze,
 * 2 hit, 3 critical, 4 splash) per distance band, distance as % of the gun's range (as
 * Battle_CheckEvasion: enemy aim point +0x268 / +0x26C vs Aya +0x2A / +0x32). */
#define E_AIM_X   0x268u
#define E_AIM_Z   0x26Cu
#define P_POS_X   0x2Au
#define P_POS_Z   0x32u
#define A_RANGE   0x02u
static const int s_band_edge[5] = { 50, 80, 100, 120, 150 };
static unsigned s_hq[6][5];                       /* [band][reaction] */
static unsigned s_hq_range, s_hq_gun;
static unsigned s_reach[4], s_reach_frames;   /* reach: ready frames in range, graze (100-150%), far (150-180%), out */

static int pe_ptr(uint32_t p);
static unsigned pe_isqrt(unsigned v) { unsigned r = 0, b = 1u << 30; while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

static void pe_note_hit_quality(uint32_t entity, uint32_t enemy, uint32_t action) {
    const uint32_t player = psx_mod_read_word(PE_PLAYER_ENTITY);
    if (!pe_ptr(player)) return;
    const int dx = (int16_t)psx_mod_read_half(entity + E_AIM_X) - (int16_t)psx_mod_read_half(player + P_POS_X);
    const int dz = (int16_t)psx_mod_read_half(entity + E_AIM_Z) - (int16_t)psx_mod_read_half(player + P_POS_Z);
    const int range = (int16_t)psx_mod_read_half(action + A_RANGE);
    if (range <= 0) return;
    const unsigned pct = pe_isqrt((unsigned)(dx * dx + dz * dz)) * 100u / (unsigned)range;
    int band = 0;
    while (band < 5 && pct >= (unsigned)s_band_edge[band]) ++band;
    unsigned reaction = (psx_mod_read_word(enemy + C_CORE_FLAGS) >> 15) & 7u;
    if (reaction > 4u) reaction = 0;
    ++s_hq[band][reaction];
    s_hq_range = (unsigned)range;
    const uint32_t rec = pe_cat_record(0);
    s_hq_gun = rec ? psx_mod_read_byte(rec + 4u) : 0;
}

/* Per-battle counters for the play log. */
static struct {
    unsigned hits, elem_hits, elem_neutral, elem_weak, elem_resist, rof_boost;
    unsigned rof_volley, gun_bonus, aa_capped, aa_topup, fast_volley, pred_ok, pred_bad;
    unsigned status_on, status_off, crit_up, quickdraw, poison_scaled;
    unsigned hp_refunded;
} s_bt;
static int s_in_battle, s_battle_frames;
static uint32_t s_rng = 0x51ED270Bu;
static int16_t s_poison_written[PE_ENEMY_SLOTS];   /* what we last wrote; anything else is a new base */
static int s_prev_hp = -1, s_refund_frac;
/* Counter: agility raised for the battle, the game's value kept here (save states too). */
#define PE_FX_STATE_VERSION 1u
static struct { uint32_t boosted; uint32_t agility; } s_fx;

/* Only for save states taken mid-battle by an old build that had the value raised: put the
 * game's value back when that battle is over (as that build would); pe_hp_level_repair then
 * gives back any level the battle added. */
static void pe_agility_restore(void) {
    if (!s_fx.boosted) return;
    psx_mod_write_half(PE_AGILITY, (uint16_t)s_fx.agility);
    s_fx.boosted = 0;
}

/* HP level repair (2026-10-16). Aya's HP level (stats.levels[0], 0x800C0E28) only goes up:
 * 0 at a new game, +10 per level-up (Aya_SetTotalExp), so it is never below level x 10. Below
 * that, the old Counter boost ate level-ups' HP (saves and save states from before the fix
 * carry it): raise it to level x 10 and max HP as Inv_RecalcSlotStats computes it,
 * (scale[0] + 20) * levelRow.hp / 20, max 999; current HP gains the same. Field only. */
#define PE_AYA_LEVEL          0x800C0E0Au   /* u8, 0-based */
#define PE_AYA_CUR_HP         0x800C0E08u
#define PE_STAT_SCALE         0x800A1B30u   /* g_StatScaleBase[0]: armor HP step */
#define PE_LEVEL_TABLE_PTR    0x800A803Cu   /* Aya_LookupLevelStats: *ptr + 0x800A8028 + level * 0x18 */
static void pe_hp_level_repair(void) {
    const unsigned level = psx_mod_read_byte(PE_AYA_LEVEL);
    if (level > 98u) return;
    const unsigned track = psx_mod_read_half(PE_AGILITY), want = level * 10u;
    if (track >= want) return;
    const uint32_t row = psx_mod_read_word(PE_LEVEL_TABLE_PTR) + 0x800A8028u + level * 0x18u;
    if (!pe_ptr(row)) return;
    const int scale = (int32_t)psx_mod_read_word(PE_STAT_SCALE);
    int max = (scale + 20) * (int)psx_mod_read_half(row) / 20;
    if (max > 999) max = 999;
    const int old_max = psx_mod_read_half(PE_MAX_HP_ADDR), cur = psx_mod_read_half(PE_AYA_CUR_HP);
    if (max <= 0) return;
    psx_mod_write_half(PE_AGILITY, (uint16_t)want);
    psx_mod_write_half(PE_MAX_HP_ADDR, (uint16_t)max);
    int nc = cur + (max > old_max ? max - old_max : 0);
    if (nc > max) nc = max;
    psx_mod_write_half(PE_AYA_CUR_HP, (uint16_t)nc);
    pe_log("hp_level_fix", "level=%u track=%u->%u max_hp=%d->%d", level + 1u, track, want, old_max, max);
}

static uint32_t pe_rand(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static int pe_roll(int pct) { return (int)(pe_rand() % 100u) < pct; }
static int pe_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }
static int pe_loaded(void) { return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0; }
static int pe_in_battle(void) { return (psx_mod_read_word(PE_GAME_BLOCK) & 2u) != 0; }
static int pe_on(void) { return s_rules != RULES_VANILLA; }

static uint32_t pe_aya_core(void) {
    const uint32_t ent = psx_mod_read_word(PE_PLAYER_ENTITY);
    return pe_ptr(ent) ? psx_mod_read_word(ent) : 0;
}

static void pe_check_prediction(void);

static void pe_restore_hit(void) {
    if (!s_hit.active) return;
    pe_check_prediction();
    if (pe_ptr(s_hit.enemy)) {
        if (s_hit.s2_mask) {
            const uint32_t now = psx_mod_read_word(s_hit.enemy + C_STATUS2);
            psx_mod_write_word(s_hit.enemy + C_STATUS2, (now & ~s_hit.s2_mask) | (s_hit.s2_orig & s_hit.s2_mask));
        }
        if (s_hit.def_set) psx_mod_write_half(s_hit.enemy + C_DEFENSE, s_hit.def_orig);
    }
    if (s_hit.power_set && pe_ptr(s_hit.action)) psx_mod_write_half(s_hit.action + A_POWER, s_hit.power_orig);
    memset(&s_hit, 0, sizeof s_hit);
}

/* Element multiplier in % for 2-bit resist value v (0 neutral, 1 resists, 2 weak). */
static int pe_elem_pct(unsigned v) {
    return v == 1u ? s_elem_resist : v == 2u ? s_elem_weak : s_elem_neutral;
}

/* Pre-roll a status round: the enemy's 2 bits at `shift` become 2 (always) or 1 (never). */
static void pe_status(uint32_t *s2, uint32_t *mask, unsigned shift) {
    const unsigned v = (*s2 >> shift) & 3u;
    if (v == 2u) return;                          /* weak: the game always lands it */
    const int on = pe_roll(v == 0u ? s_status_average : s_status_resist);
    *s2 = (*s2 & ~(3u << shift)) | ((on ? 2u : 1u) << shift);
    *mask |= 3u << shift;
    if (on) ++s_bt.status_on; else ++s_bt.status_off;
}

/* ---- rebalance helpers (R-A / R-B / R-C / R-E) ------------------------------------ */
#define E_HP_CAP      0x88u                  /* hpAliveCap: the enemy's max HP */
#define GUN_G3A3      0x7Du
#define GUN_M203_4    0x86u
/* R-F: guns that trailed the earlier gun of their class where they're found (gun ladder with
 * stage-gated Catalog installs): { item, attack bonus }. */
static const uint8_t s_prog_guns[][2] = { { 0x79u, 25 } /* M16A1 */, { 0x7Bu, 15 } /* SG550 */,
                                          { 0x83u, 20 } /* M203 */, { 0x84u, 15 } /* M203 2 */ };

/* R-B / R-C / R-F: flat attack bonus for the equipped gun. */
static int pe_gun_bonus(unsigned gun) {
    if (gun == GUN_G3A3) return s_g3a3_bonus;
    if (gun == GUN_M203_4) return s_m203_4_bonus;
    if (s_prog_bonus)
        for (unsigned i = 0; i < sizeof s_prog_guns / sizeof s_prog_guns[0]; ++i)
            if (s_prog_guns[i][0] == gun) return s_prog_guns[i][1];
    return 0;
}
static const uint8_t s_shot_pct[11] = { 0, 100, 60, 41, 0, 25, 0, 18, 0, 0, 13 };

/* The damage Battle_StepEnemyMovement (battle/enemy_movement.c) deals for a gun hit, given the
 * attack term's power bonus `pb` (stat1E / 5 = s), the shots in the volley, the defense, the
 * reaction (core flags 0x38000: 0x8000 graze x0.3, 0x18000 critical x1.5, 0x20000 splash / 10),
 * the element bits it will read and the turn word; minimum 1 (2 on a critical). The
 * Acid / Tranq / disable rolls change no damage. */
static int pe_hit_damage(int s, int pb, unsigned shots, unsigned def, uint32_t cf, uint32_t s2, uint32_t turn) {
    if (shots < 1u || shots > 10u || !s_shot_pct[shots]) shots = 1u;
    int dmg = (s + pb) * s_shot_pct[shots] / 100 - (int)(def / shots);
    if (dmg > 0) {
        const uint32_t r = cf & 0x38000u;
        if (r == 0x8000u) dmg = dmg * 3 / 10;
        else if (r == 0x18000u) dmg = dmg * 3 / 2;
        else if (r == 0x20000u) dmg /= 10;
        const unsigned cls = (cf >> 18) & 3u;
        if (cls != 2u && cls != 3u && !(cf & 0x400u)) {
            int mode = 0;
            if (turn & 0x100u) { const unsigned v = (s2 >> 14) & 3u; if (v == 1u) mode = 1; else if (v == 2u) mode = 2; }
            if (mode != 2 && (turn & 0x200u)) { const unsigned v = (s2 >> 16) & 3u; if (v == 1u) mode = 1; else if (v == 2u) mode = 2; }
            if (mode == 1) dmg /= 5; else if (mode == 2) dmg = dmg * 3 / 2;
        }
    }
    if (dmg <= 0) dmg = (cf & 0x38000u) == 0x18000u ? 2 : 1;
    return dmg;
}

/* R-A applies in ordinary fights: two or more enemies alive and none of them a boss (bit 0x40000). */
static int pe_crowd_fight(void) {
    int alive = 0;
    for (int i = 0; i < PE_ENEMY_SLOTS; ++i) {
        const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)i * PE_ENEMY_SLOT_STRIDE;
        if (!psx_mod_read_byte(slot)) continue;
        const uint32_t core = slot + 4u;
        if ((int32_t)psx_mod_read_word(core + C_GAUGE) <= 0) continue;
        if (psx_mod_read_word(core + C_STATUS2) & 0x40000u) return 0;
        ++alive;
    }
    return alive >= 2;
}

/* R-A per-turn reference: each target's HP at its first Attack All hit of the turn. A turn's hits
 * (every volley of a 2 / 3 Commands turn too) come close together; 40 battle frames with no Attack
 * All hit start a new turn. */
static struct {
    uint32_t enemy[PE_ENEMY_SLOTS];
    int32_t ref[PE_ENEMY_SLOTS];
    int n, idle;
    uint32_t fix_enemy;                     /* a minimum-damage hit crossed the line: top HP back up */
    int32_t fix_floor;
} s_aa;

static int32_t pe_aa_ref(uint32_t enemy, int32_t hp) {
    if (s_aa.idle > 40) s_aa.n = 0;
    s_aa.idle = 0;
    for (int i = 0; i < s_aa.n; ++i) if (s_aa.enemy[i] == enemy) return s_aa.ref[i];
    if (s_aa.n < PE_ENEMY_SLOTS) { s_aa.enemy[s_aa.n] = enemy; s_aa.ref[s_aa.n] = hp; ++s_aa.n; }
    return hp;
}

static void pe_aa_fix(void) {
    if (!s_aa.fix_enemy) return;
    if (pe_ptr(s_aa.fix_enemy)) {
        const int32_t hp = (int32_t)psx_mod_read_word(s_aa.fix_enemy + C_GAUGE);
        if (hp > 0 && hp < s_aa.fix_floor) psx_mod_write_word(s_aa.fix_enemy + C_GAUGE, (uint32_t)s_aa.fix_floor);
    }
    s_aa.fix_enemy = 0;
}

/* Item record (equipped instance or base item, 0x20 bytes): mod count +0x14, mod bytes +0x15
 * ((group << 5) | effect; RoF is group 1, so every RoF byte has 0x20 set). */
static int pe_rec_has_effect(uint32_t rec, int effect) {
    int n = psx_mod_read_byte(rec + 0x14u);
    if (n > 10) n = 10;
    for (int m = 0; m < n; ++m)
        if ((int)(psx_mod_read_byte(rec + 0x15u + (uint32_t)m) & 0x1Fu) == effect) return 1;
    return 0;
}

/* R-E: the equipped gun's RoF for this many shots is an installed mod (effects 3 / 4 / 5 = RoF
 * 5 / 7 / 10): the gun has it and its base item (Item_LookupBaseData table) doesn't, so the
 * P90's own RoF 10 stays built in. (Until 2026-10-20 this tested bit 0x20, which every RoF byte
 * has, so no install ever counted.) */
#define PE_ITEM_TABLE_BASE_ADDR 0x800A8034u
#define PE_ITEM_TABLE_END_ADDR  0x800A8038u
#define PE_ITEM_TABLE_ORIGIN    0x800A8028u
static int pe_installed_rof(unsigned shots) {
    const int effect = shots == 5u ? 3 : shots == 7u ? 4 : shots == 10u ? 5 : 0;
    const uint32_t rec = pe_cat_record(0);
    if (!effect || !rec || !pe_rec_has_effect(rec, effect)) return 0;
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
    const uint32_t end = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
    const uint32_t index = (uint32_t)psx_mod_read_byte(rec + 4u) - 1u;
    if (end <= base || index >= ((end - base) >> 5)) return 0;
    return !pe_rec_has_effect(PE_ITEM_TABLE_ORIGIN + base + index * 0x20u, effect);
}

/* Self-check (2026-10-19): R-A trusts pe_hit_damage to match Battle_StepEnemyMovement, so every
 * gun hit's damage is predicted at the hook and compared with the HP the target lost by the next
 * hook. Per battle the modfx row counts matches / mismatches; the first mismatches get a "dmgchk"
 * row each (a Disable roll, which skips the element factor at random, is the known cause). */
static int s_chk_logged;
static void pe_check_prediction(void) {
    if (!s_hit.pred || !pe_ptr(s_hit.enemy)) return;
    const int32_t now = (int32_t)psx_mod_read_word(s_hit.enemy + C_GAUGE);
    const int32_t lost = s_hit.hp_before - now;
    if (lost == s_hit.pred) ++s_bt.pred_ok;
    else {
        ++s_bt.pred_bad;
        if (s_chk_logged < 8) {
            ++s_chk_logged;
            pe_log("dmgchk", "predicted=%d lost=%d hp_before=%d flags=0x%08X",
                   s_hit.pred, (int)lost, (int)s_hit.hp_before, (unsigned)psx_mod_read_word(s_hit.enemy + C_CORE_FLAGS));
        }
    }
    s_hit.pred = 0;
}

static void pe_on_damage(struct CPUState *cpu, uint32_t address) {
    (void)address;
    pe_restore_hit();
    pe_aa_fix();
    if (!pe_loaded()) return;
    const uint32_t entity = cpu->gpr[4];
    if (!pe_ptr(entity)) return;
    const uint32_t enemy = psx_mod_read_word(entity);
    const uint32_t actor = psx_mod_read_word(PE_ACTIVE_ACTOR);
    if (!pe_ptr(enemy) || !pe_ptr(actor) || actor != pe_aya_core() || enemy == actor) return;
    if (psx_mod_read_word(actor + C_STATE) & 0x180000u) return;      /* PE skills etc.: guns only */
    const uint32_t action = psx_mod_read_word(actor + C_ACTION);
    if (!pe_ptr(action)) return;
    pe_note_hit_quality(entity, enemy, action);
    if (!pe_on()) return;
    const uint32_t turn = psx_mod_read_word(action + A_TURN);
    const unsigned shots = turn & 0xFu;
    uint32_t s2 = psx_mod_read_word(enemy + C_STATUS2), mask = 0;
    const int boss = (s2 & 0x40000u) != 0;
    ++s_bt.hits;

    /* Attack scale (per mille) and both-term scale (element). */
    int atk_pm = 1000, both_pm = 1000;
    if (s_rof_volley > 0 && pe_installed_rof(shots)) {               /* R-E: whole volley % */
        atk_pm = s_rof_volley * 1000 / (int)(shots * s_shot_pct[shots]);
        ++s_bt.rof_volley;
    } else {
        if (shots == 7u && s_rof7 != 18) { atk_pm = s_rof7 * 1000 / 18; ++s_bt.rof_boost; }
        if (shots == 10u && s_rof10 != 13) { atk_pm = s_rof10 * 1000 / 13; ++s_bt.rof_boost; }
    }
    /* R-B / R-C / R-F: flat attack for some guns. */
    int bonus = 0;
    {
        const uint32_t rec = pe_cat_record(0);
        const unsigned gun = rec ? psx_mod_read_byte(rec + 4u) : 0u;
        bonus = pe_gun_bonus(gun);
        if (bonus) ++s_bt.gun_bonus;
    }
    if (turn & 0x300u) {
        const unsigned h = (s2 >> 14) & 3u, f = (s2 >> 16) & 3u;
        int pct;
        if (s_elem_best || !(turn & 0x100u) || !(turn & 0x200u)) {
            pct = 0;
            if (turn & 0x100u) pct = pe_elem_pct(h);
            if ((turn & 0x200u) && pe_elem_pct(f) > pct) pct = pe_elem_pct(f);
        } else {                                  /* vanilla order: Heat, then Frost unless weak */
            unsigned st = h == 1u ? 1u : h == 2u ? 2u : 0u;
            if (st != 2u && (f == 1u || f == 2u)) st = f;
            pct = pe_elem_pct(st);
        }
        const unsigned which = (turn & 0x100u) && pe_elem_pct(h) == pct ? h : f;
        if (which == 1u) ++s_bt.elem_resist; else if (which == 2u) ++s_bt.elem_weak; else ++s_bt.elem_neutral;
        ++s_bt.elem_hits;
        both_pm = pct * 10;
        s2 &= ~(0xFu << 14);                      /* the game adds no element factor itself */
        mask |= 0xFu << 14;
    }
    if (!boss) {
        if (turn & 0x800u) pe_status(&s2, &mask, 2);    /* Acid */
        if (turn & 0x1000u) pe_status(&s2, &mask, 4);   /* Tranquilizer */
    }
    /* Critical: some normal hits become criticals (reaction 3 = x1.5). */
    if ((turn & 0x20000u) && s_crit > 20) {
        const uint32_t core = psx_mod_read_word(enemy + C_CORE_FLAGS);
        if (((core >> 15) & 7u) == 2u && pe_roll((s_crit - 20) * 100 / 75)) {
            psx_mod_write_word(enemy + C_CORE_FLAGS, (core & ~0x38000u) | 0x18000u);
            ++s_bt.crit_up;
        }
    }

    s_hit.active = 1;
    s_hit.enemy = enemy;
    s_hit.action = action;
    if (mask) {
        s_hit.s2_mask = mask;
        s_hit.s2_orig = psx_mod_read_word(enemy + C_STATUS2);
        psx_mod_write_word(enemy + C_STATUS2, (s_hit.s2_orig & ~mask) | (s2 & mask));
    }
    const int s = psx_mod_read_half(actor + C_STAT1E) / 5;
    const int pb = (int16_t)psx_mod_read_half(action + A_POWER);
    const unsigned d = psx_mod_read_half(enemy + C_DEFENSE);
    int want = pb;
    unsigned nd = d;
    if (atk_pm != 1000 || both_pm != 1000 || bonus) {
        want = (int)(((int64_t)(s + pb + bonus) * atk_pm / 1000) * both_pm / 1000) - s;
        if (want < 0) want = 0;
        if (both_pm != 1000) {
            nd = (unsigned)((uint64_t)d * (unsigned)both_pm / 1000u);
            if (nd > 0xFFFFu) nd = 0xFFFFu;
        }
    }
    /* R-A: an Attack All turn takes at most s_aa_cap % of a target's max HP (crowds only). The hit's
     * power bonus is lowered just enough that the replicated damage stays above the line; a hit
     * whose minimum damage still crosses it is topped back up at the next hook. */
    if (s_aa_cap < 100 && (turn & 0xC0u) == 0xC0u && pe_crowd_fight()) {
        const int32_t hp = (int32_t)psx_mod_read_word(enemy + C_GAUGE);
        int32_t max = (int32_t)psx_mod_read_word(enemy + E_HP_CAP);
        if (max <= 0) max = hp;
        const int32_t floor = pe_aa_ref(enemy, hp) - (int32_t)((int64_t)max * s_aa_cap / 100);
        if (floor > 0 && hp > 0) {
            const uint32_t cf = psx_mod_read_word(enemy + C_CORE_FLAGS);
            const int allowed = hp - floor;
            if (pe_hit_damage(s, want, shots, nd, cf, s2, turn) > allowed) {
                int lo = -s, hi = want;
                if (pe_hit_damage(s, lo, shots, nd, cf, s2, turn) > allowed) {
                    want = lo;
                    s_aa.fix_enemy = enemy; s_aa.fix_floor = floor; ++s_bt.aa_topup;
                } else {
                    while (lo < hi) {                       /* the largest bonus that stays above the line */
                        const int mid = lo + (hi - lo + 1) / 2;
                        if (pe_hit_damage(s, mid, shots, nd, cf, s2, turn) <= allowed) lo = mid; else hi = mid - 1;
                    }
                    want = lo;
                }
                ++s_bt.aa_capped;
            }
        }
    }
    if (want > 0x7FFF) want = 0x7FFF;
    if (want < -0x8000) want = -0x8000;
    if (want != pb) {
        s_hit.power_orig = (uint16_t)pb;
        s_hit.power_set = 1;
        psx_mod_write_half(action + A_POWER, (uint16_t)(int16_t)want);
    }
    if (nd != d) {
        s_hit.def_orig = (uint16_t)d;
        s_hit.def_set = 1;
        psx_mod_write_half(enemy + C_DEFENSE, (uint16_t)nd);
    }
    s_hit.hp_before = (int32_t)psx_mod_read_word(enemy + C_GAUGE);
    s_hit.pred = pe_hit_damage(s, want, shots, nd, psx_mod_read_word(enemy + C_CORE_FLAGS), s2, turn);
}

static void pe_on_battle_update(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    pe_restore_hit();
    pe_aa_fix();
    if (s_aa.idle < 1000) ++s_aa.idle;
}

/* ---- per battle --------------------------------------------------------------------- */
static int pe_gun_has(int effect) {
    const uint32_t rec = pe_cat_record(0);
    if (!rec) return 0;
    int n = psx_mod_read_byte(rec + 0x14u);
    if (n > 10) n = 10;
    for (int m = 0; m < n; ++m)
        if ((psx_mod_read_byte(rec + 0x15u + (uint32_t)m) & 0x1F) == effect) return 1;
    return 0;
}

static void pe_battle_start(void) {
    memset(&s_bt, 0, sizeof s_bt);
    memset(&s_aa, 0, sizeof s_aa);
    s_chk_logged = 0;
    memset(s_hq, 0, sizeof s_hq);
    memset(s_reach, 0, sizeof s_reach);
    s_reach_frames = 0;
    memset(s_poison_written, 0, sizeof s_poison_written);
    s_battle_frames = 0;
    s_prev_hp = -1;
    s_refund_frac = 0;
}

/* Reach: while Aya's bar is full, how far the nearest live enemy is against the gun's
 * range (the hit rows only see shots that landed; this sees waiting for a target).
 * Enemies: g_FieldActorListHead (as Battle_ApplyAttack walks it), entityFlags +0x98
 * (targets as the game picks them: &0x2040 != 0x40 and not &0x4000), core hpAlive +0x10. */
#define PE_ACTOR_LIST   0x8009D20Cu
#define E_NEXT          0x04u
#define E_FLAGS         0x98u
#define C_HP_ALIVE      0x10u
static unsigned s_reach_range;

static void pe_note_reach(void) {
    ++s_reach_frames;
    const uint32_t player = psx_mod_read_word(PE_PLAYER_ENTITY);
    if (!pe_ptr(player)) return;
    const uint32_t aya = psx_mod_read_word(player);
    if (!pe_ptr(aya) || psx_mod_read_half(aya + C_GAUGE) < 9000u) return;
    const uint32_t action = psx_mod_read_word(aya + C_ACTION);
    if (!pe_ptr(action)) return;
    const int range = (int16_t)psx_mod_read_half(action + A_RANGE);
    if (range <= 0) return;
    const int px = (int16_t)psx_mod_read_half(player + P_POS_X), pz = (int16_t)psx_mod_read_half(player + P_POS_Z);
    unsigned best = 0xFFFFFFFFu;
    uint32_t e = psx_mod_read_word(PE_ACTOR_LIST);
    for (int guard = 0; pe_ptr(e) && guard < 64; ++guard, e = psx_mod_read_word(e + E_NEXT)) {
        if (e == player) continue;
        const uint32_t core = psx_mod_read_word(e);
        if (!pe_ptr(core)) continue;
        const uint32_t fl = psx_mod_read_word(e + E_FLAGS);
        if ((fl & 0x2040u) == 0x40u || (fl & 0x4000u) || (int32_t)psx_mod_read_word(core + C_HP_ALIVE) <= 0) continue;
        const int dx = (int16_t)psx_mod_read_half(e + E_AIM_X) - px, dz = (int16_t)psx_mod_read_half(e + E_AIM_Z) - pz;
        const unsigned pct = pe_isqrt((unsigned)(dx * dx + dz * dz)) * 100u / (unsigned)range;
        if (pct < best) best = pct;
    }
    if (best == 0xFFFFFFFFu) return;
    s_reach_range = (unsigned)range;
    ++s_reach[best <= 100u ? 0 : best <= 150u ? 1 : best <= 180u ? 2 : 3];
}

static void pe_battle_end(void) {
    pe_restore_hit();
    pe_agility_restore();
    {
        const unsigned ready = s_reach[0] + s_reach[1] + s_reach[2] + s_reach[3];
        const uint32_t rec = pe_cat_record(0);
        if (ready)
            pe_log("reach", "gun=0x%02X range=%u frames=%u ready=%u in=%u graze=%u far=%u out=%u",
                   rec ? psx_mod_read_byte(rec + 4u) : 0u, s_reach_range, s_reach_frames, ready, s_reach[0], s_reach[1], s_reach[2], s_reach[3]);
        memset(s_reach, 0, sizeof s_reach);
        s_reach_frames = 0;
    }
    {                                             /* hit quality, under any rules */
        unsigned total = 0;
        for (int b = 0; b < 6; ++b) for (int r = 0; r < 5; ++r) total += s_hq[b][r];
        if (total) {
            static const char *const names[6] = { "lt50", "50_80", "80_100", "100_120", "120_150", "gt150" };
            char buf[320];
            int n = snprintf(buf, sizeof buf, "gun=0x%02X range=%u", s_hq_gun, s_hq_range);
            for (int b = 0; b < 6 && n < (int)sizeof buf; ++b) {
                const unsigned *q = s_hq[b];
                if (q[0] + q[1] + q[2] + q[3] + q[4])
                    n += snprintf(buf + n, sizeof buf - (size_t)n, " %s=%u/%u/%u/%u", names[b], q[2], q[1], q[3], q[4]);
            }
            pe_log("hitq", "%s", buf);
        }
    }
    if (!pe_on()) return;
    pe_log("modfx", "rules=%s hits=%u elem=%u(n%u/w%u/r%u) rof_boost=%u status_on=%u status_off=%u crit_up=%u quickdraw=%u poison_scaled=%u hp_refunded=%u"
           " rof_volley=%u gun_bonus=%u aa_capped=%u aa_topup=%u fast_volley=%u dmg_pred=%u/%u",
           s_rules_names[s_rules], s_bt.hits, s_bt.elem_hits, s_bt.elem_neutral, s_bt.elem_weak, s_bt.elem_resist,
           s_bt.rof_boost, s_bt.status_on, s_bt.status_off, s_bt.crit_up, s_bt.quickdraw, s_bt.poison_scaled,
           s_bt.hp_refunded, s_bt.rof_volley, s_bt.gun_bonus, s_bt.aa_capped, s_bt.aa_topup, s_bt.fast_volley,
           s_bt.pred_ok, s_bt.pred_ok + s_bt.pred_bad);
}

static void pe_battle_tick(void) {
    const uint32_t aya = pe_aya_core();
    if (!pe_ptr(aya)) return;
    ++s_battle_frames;
    /* Quickdraw: top up the game's stat22 % chance, once, early in the battle. */
    if (s_battle_frames == 2 && s_quickdraw > 0 && pe_gun_has(15)) {
        const int st = psx_mod_read_half(aya + C_STAT22);
        int want = st + s_quickdraw;
        if (want > 90) want = 90;
        if (psx_mod_read_half(aya + C_GAUGE) < 9000u && st < 100 && want > st &&
            pe_roll((want - st) * 100 / (100 - st))) {
            psx_mod_write_half(aya + C_GAUGE, 9000);
            ++s_bt.quickdraw;
        }
    }
    /* Counter: NOT boosted any more (2026-10-16). The roll reads 0x800C0E28, which the decomp
     * calls g_AyaStatAgility[0] but is Aya's stat-level table (AyaSaveState.stats.levels[0], the
     * HP level). Raising it for the battle let the post-battle level-up commit the boosted value
     * and our restore then wiped the level's HP gain: a full run ended at level 33 with level 24's
     * HP (385 instead of 714). A Counter boost needs an entry hook on the roll itself. */
    /* Poison strength: each enemy's periodicDamage once. */
    if (s_poison != 100) {
        for (int s = 0; s < PE_ENEMY_SLOTS; ++s) {
            const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)s * PE_ENEMY_SLOT_STRIDE;
            if (!psx_mod_read_byte(slot)) { s_poison_written[s] = 0; continue; }
            const uint32_t core = slot + 4u;
            const int p = (int16_t)psx_mod_read_half(core + C_POISON);
            if (p <= 0 || p == s_poison_written[s]) continue;   /* scaled already: never compound */
            int np = p * s_poison / 100;
            if (np < 1) np = 1;
            if (np > 0x7FFF) np = 0x7FFF;
            psx_mod_write_half(core + C_POISON, (uint16_t)np);
            s_poison_written[s] = (int16_t)np;
            ++s_bt.poison_scaled;
        }
    }
    /* Damage taken with Attack Down / Slow Active Time armor. */
    int guard = 100;
    if (pe_loot_armor_has(15)) guard = guard * s_guard_attack_down / 100;
    if (pe_loot_armor_has(13)) guard = guard * s_guard_slow_at / 100;
    const int hp = (int16_t)psx_mod_read_half(aya + C_HP);
    if (guard < 100 && s_prev_hp > 0 && hp > 0 && hp < s_prev_hp) {
        s_refund_frac += (s_prev_hp - hp) * (100 - guard);
        const int back = s_refund_frac / 100;
        s_refund_frac %= 100;
        if (back > 0) {
            const int maxhp = (int16_t)psx_mod_read_half(PE_MAX_HP_ADDR);
            int nh = hp + back;
            if (nh > maxhp) nh = maxhp;
            psx_mod_write_half(aya + C_HP, (uint16_t)nh);
            s_bt.hp_refunded += (unsigned)(nh - hp);
            s_prev_hp = nh;
            return;
        }
    }
    s_prev_hp = hp;
}

/* ---- R-E speed / hold: fast volleys for installed RoF 5 / 7 / 10 ----------------------------
 * Each extra bullet of a volley replays Aya's follow-up clip (action modes 7 / 9 / 11, set by
 * Battle_SetupEnemyAnims), whose length depends on the gun kind; between queue slots the kind's
 * hold byte (g_BattleActionAnimRows 0x800106A4, 5 bytes a kind, byte 2) adds frames. While the
 * equipped gun has an installed RoF 5 / 7 / 10 and "RoF mods: fast volleys" is on:
 *   - in a follow-up mode Aya's animation step (entity +0x1C, 16.16) is raised so the clip plays in
 *     max(4, hit frame) frames (factor = follow frames / that; machine guns are already faster);
 *   - the kind's hold byte reads 0.
 * Both go back to the game's values the moment the condition ends (save states included: the
 * hold bytes are restored from the retail values below, not from memory). */
#define PE_ANIM_ROWS        0x800106A4u
#define E_ACTION_MODE       0x0Eu
#define E_ANIM_STEP         0x1Cu
static const uint8_t s_hold_retail[9] = { 0, 5, 0, 0, 0, 5, 20, 5, 0 };     /* byte 2 per kind */
static const uint16_t s_follow_x100[9] = { 100, 250, 750, 100, 375, 625, 250, 250, 100 };
static uint32_t s_vol_player, s_vol_written, s_vol_base;

static int pe_fast_volley_gun(unsigned *kind) {
    const uint32_t rec = pe_cat_record(0);
    if (!rec) return 0;
    *kind = psx_mod_read_byte(rec + 6u);
    return *kind < 9u && (pe_installed_rof(5u) || pe_installed_rof(7u) || pe_installed_rof(10u));
}

static void pe_fast_volley_tick(int battle) {
    unsigned kind = 0;
    const int on = battle && pe_on() && s_fast_volley && pe_fast_volley_gun(&kind);
    for (unsigned k = 1; k < 9u; ++k) {                  /* hold bytes: 0 for this kind, retail otherwise */
        const uint32_t at = PE_ANIM_ROWS + k * 5u + 2u;
        const uint8_t want = on && k == kind ? 0 : s_hold_retail[k];
        if (psx_mod_read_byte(at) != want) psx_mod_write_byte(at, want);
    }
    const uint32_t player = psx_mod_read_word(PE_PLAYER_ENTITY);
    if (s_vol_written && pe_ptr(s_vol_player)) {         /* put the game's step back when we're done */
        const unsigned mode = pe_ptr(player) ? psx_mod_read_byte(player + E_ACTION_MODE) : 0u;
        const int follow = mode == 7u || mode == 9u || mode == 11u;
        if (!on || !follow || player != s_vol_player) {
            if (psx_mod_read_word(s_vol_player + E_ANIM_STEP) == s_vol_written)
                psx_mod_write_word(s_vol_player + E_ANIM_STEP, s_vol_base);
            s_vol_written = 0;
        }
    }
    if (!on || !pe_ptr(player) || s_follow_x100[kind] <= 100u) return;
    const unsigned mode = psx_mod_read_byte(player + E_ACTION_MODE);
    if (mode != 7u && mode != 9u && mode != 11u) return;
    const uint32_t step = psx_mod_read_word(player + E_ANIM_STEP);
    if (s_vol_written && step == s_vol_written) return;  /* ours, still in place */
    s_vol_base = step;                                    /* the game set a new step: scale it */
    uint64_t fast = (uint64_t)step * s_follow_x100[kind] / 100u;
    if (fast > 0x7FFFFFFFu) fast = 0x7FFFFFFFu;
    s_vol_written = (uint32_t)fast;
    s_vol_player = player;
    psx_mod_write_word(player + E_ANIM_STEP, s_vol_written);
    ++s_bt.fast_volley;
}

/* For pe_weapon_power.c's DPS line: the rebalance's attack bonus for a gun and the installed-RoF
 * volley % (0 when off or under Vanilla rules). */
int pe_fx_attack_bonus(unsigned gun) {
    if (!pe_on()) return 0;
    return pe_gun_bonus(gun);
}
int pe_fx_rof_volley_pct(void) { return pe_on() ? s_rof_volley : 0; }

/* ---- rules rows ------------------------------------------------------------------------ */
static void pe_apply_rules(int rules) {
    if (rules == RULES_CUSTOM) return;
    for (int i = 0; i < PE_ROWS; ++i)
        *s_rows[i].value = rules == RULES_VANILLA ? s_rows[i].vanilla : s_rows[i].rebalanced;
}

static int pe_matches(int rules) {
    for (int i = 0; i < PE_ROWS; ++i)
        if (*s_rows[i].value != (rules == RULES_VANILLA ? s_rows[i].vanilla : s_rows[i].rebalanced)) return 0;
    return 1;
}

static void pe_mod_effects_tick(void) {
    if (s_rules != s_rules_shown) {               /* the Rules row was changed */
        pe_apply_rules(s_rules);
        s_rules_shown = s_rules;
    } else if (s_rules != RULES_CUSTOM && !pe_matches(s_rules)) {
        s_rules = s_rules_shown = RULES_CUSTOM;   /* a value row was changed */
    } else if (s_rules == RULES_CUSTOM && pe_matches(RULES_REBALANCED)) {
        s_rules = s_rules_shown = RULES_REBALANCED;
    } else if (s_rules == RULES_CUSTOM && pe_matches(RULES_VANILLA)) {
        s_rules = s_rules_shown = RULES_VANILLA;
    }
    if (!pe_loaded()) { s_in_battle = 0; return; }
    const int battle = pe_in_battle();
    if (battle && !s_in_battle) pe_battle_start();
    if (!battle && s_in_battle) pe_battle_end();
    s_in_battle = battle;
    if (!battle) pe_hp_level_repair();            /* any rules: undoes an old build's damage */
    if (battle) pe_note_reach();                  /* under any rules, like the hit rows */
    if (battle && pe_on()) pe_battle_tick();
    pe_fast_volley_tick(battle);                  /* also restores the game's values when off */
}

static void pe_fx_state_loaded(int result, uint32_t saved_version, const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_fx, 0, sizeof s_fx);
}

static void pe_mod_effects_restored(void) {
    memset(&s_hit, 0, sizeof s_hit);              /* a save state brings its own memory back */
    s_in_battle = pe_loaded() && pe_in_battle();
    if (!s_in_battle) pe_agility_restore();       /* a state saved mid-battle, loaded outside one */
    if (!s_in_battle && pe_loaded()) pe_hp_level_repair();
    if (s_in_battle) pe_battle_start();
}

/* Called from pe_mod_menu.c. */
void pe_mod_effects_register(void) {
    pe_apply_rules(RULES_REBALANCED);
    s_rules_shown = s_rules;
    (void)pe_menu_register_choice("Mod Effects", "Rules",
        "Vanilla, Rebalanced, or Custom.", &s_rules, s_rules_names, 3);
    for (int i = 0; i < PE_ROWS; ++i) {
        if (s_rows[i].choice)
            (void)pe_menu_register_choice("Mod Effects", s_rows[i].label, s_rows[i].desc,
                                          s_rows[i].value, s_onoff, 2);
        else
            (void)pe_menu_register_int("Mod Effects", s_rows[i].label, s_rows[i].desc, s_rows[i].value,
                                       s_rows[i].min, s_rows[i].max, s_rows[i].step);
    }
    (void)pe_menu_register_tick(pe_mod_effects_tick);
    (void)psx_mod_register_function_entry_plugin("pe.mod-effects.damage", PE_FN_DAMAGE, pe_on_damage);
    (void)psx_mod_register_function_entry_plugin("pe.mod-effects.battle", PE_FN_BATTLE_UPDATE, pe_on_battle_update);
    (void)psx_mod_register_state("pe.mod-effects", PE_FX_STATE_VERSION, &s_fx, sizeof s_fx, pe_fx_state_loaded);
    (void)psx_mod_register_restore_callback(pe_mod_effects_restored);
}
