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
 * Per battle (VBlank tick): poison strength (each enemy's periodicDamage, +0x96, scaled once);
 * Quickdraw top-up at the start (the game's chance is Aya's stat22 %; we roll the difference up to
 * stat22 + bonus, max 90%); damage taken with Attack Down / Slow Active Time armor (HP refunded as
 * hits land); Counter (the game's roll reads Aya's agility, g_AyaStatAgility[0], only there:
 * raised for the battle while Counter is on the gun, put back after and in save states). One
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
static PeRow s_rows[] = {
    { "Heat/Frost neutral %", "Damage vs enemies without a match.", 100, 115, 50, 200, 5, &s_elem_neutral, 0 },
    { "Heat/Frost weak %", "Damage vs enemies weak to it.", 150, 150, 50, 300, 5, &s_elem_weak, 0 },
    { "Heat/Frost resist %", "Damage vs enemies that resist it.", 20, 80, 10, 150, 5, &s_elem_resist, 0 },
    { "Heat+Frost best of two", "Both on: the better one counts.", 0, 1, 0, 1, 1, &s_elem_best, 1 },
    { "RoF 7 per-shot %", "Each of 7 shots (vanilla 18).", 18, 20, 10, 40, 1, &s_rof7, 0 },
    { "RoF 10 per-shot %", "Each of 10 shots (vanilla 13).", 13, 15, 8, 30, 1, &s_rof10, 0 },
    { "Acid/Tranq vs resist %", "Chance vs resistant (vanilla 0).", 0, 20, 0, 100, 5, &s_status_resist, 0 },
    { "Acid/Tranq vs normal %", "Chance vs others (vanilla 50).", 50, 60, 0, 100, 5, &s_status_average, 0 },
    { "Poison strength %", "Acid poison damage per tick.", 100, 125, 50, 300, 5, &s_poison, 0 },
    { "Critical bonus points", "Crit chance the effect adds (20).", 20, 40, 0, 80, 5, &s_crit, 0 },
    { "Quickdraw bonus points", "Added to the speed-stat chance.", 0, 15, 0, 90, 5, &s_quickdraw, 0 },
    { "Counter chance %", "Of the game's (agility; ~2-8%).", 100, 500, 100, 1000, 50, &s_counter, 0 },
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

static void pe_agility_restore(void) {
    if (!s_fx.boosted) return;
    psx_mod_write_half(PE_AGILITY, (uint16_t)s_fx.agility);
    s_fx.boosted = 0;
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

static void pe_restore_hit(void) {
    if (!s_hit.active) return;
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

static void pe_on_damage(struct CPUState *cpu, uint32_t address) {
    (void)address;
    pe_restore_hit();
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
    if (shots == 7u && s_rof7 != 18) { atk_pm = s_rof7 * 1000 / 18; ++s_bt.rof_boost; }
    if (shots == 10u && s_rof10 != 13) { atk_pm = s_rof10 * 1000 / 13; ++s_bt.rof_boost; }
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
    if (atk_pm != 1000 || both_pm != 1000) {
        const int s = psx_mod_read_half(actor + C_STAT1E) / 5;
        const int pb = (int16_t)psx_mod_read_half(action + A_POWER);
        int want = (int)(((int64_t)(s + pb) * atk_pm / 1000) * both_pm / 1000) - s;
        if (want < 0) want = 0;
        if (want > 0x7FFF) want = 0x7FFF;
        s_hit.power_orig = (uint16_t)pb;
        s_hit.power_set = 1;
        psx_mod_write_half(action + A_POWER, (uint16_t)want);
        if (both_pm != 1000) {
            const unsigned d = psx_mod_read_half(enemy + C_DEFENSE);
            unsigned nd = (unsigned)((uint64_t)d * (unsigned)both_pm / 1000u);
            if (nd > 0xFFFFu) nd = 0xFFFFu;
            s_hit.def_orig = (uint16_t)d;
            s_hit.def_set = 1;
            psx_mod_write_half(enemy + C_DEFENSE, (uint16_t)nd);
        }
    }
}

static void pe_on_battle_update(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    pe_restore_hit();
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
    memset(s_hq, 0, sizeof s_hq);
    memset(s_poison_written, 0, sizeof s_poison_written);
    s_battle_frames = 0;
    s_prev_hp = -1;
    s_refund_frac = 0;
}

static void pe_battle_end(void) {
    pe_restore_hit();
    pe_agility_restore();
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
    pe_log("modfx", "rules=%s hits=%u elem=%u(n%u/w%u/r%u) rof_boost=%u status_on=%u status_off=%u crit_up=%u quickdraw=%u poison_scaled=%u hp_refunded=%u",
           s_rules_names[s_rules], s_bt.hits, s_bt.elem_hits, s_bt.elem_neutral, s_bt.elem_weak, s_bt.elem_resist,
           s_bt.rof_boost, s_bt.status_on, s_bt.status_off, s_bt.crit_up, s_bt.quickdraw, s_bt.poison_scaled,
           s_bt.hp_refunded);
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
    /* Counter: the roll reads agility (chance ~ agility / 1000 .. / 5000 by rank). */
    if (s_battle_frames == 2 && s_counter != 100 && !s_fx.boosted && pe_gun_has(14)) {
        const unsigned a = psx_mod_read_half(PE_AGILITY);
        unsigned na = a * (unsigned)s_counter / 100u;
        if (na > 0xFFFFu) na = 0xFFFFu;
        s_fx.agility = a;
        s_fx.boosted = 1;
        psx_mod_write_half(PE_AGILITY, (uint16_t)na);
    }
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
    if (battle && pe_on()) pe_battle_tick();
}

static void pe_fx_state_loaded(int result, uint32_t saved_version, const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_fx, 0, sizeof s_fx);
}

static void pe_mod_effects_restored(void) {
    memset(&s_hit, 0, sizeof s_hit);              /* a save state brings its own memory back */
    s_in_battle = pe_loaded() && pe_in_battle();
    if (!s_in_battle) pe_agility_restore();       /* a state saved mid-battle, loaded outside one */
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
