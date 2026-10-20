/*
 * Items & Parasite Energy rebalance (bridge findings pe-parasite-energy-rebalance-2026-10-15.md).
 * F1 > PE Enhancements > Items & Parasite Energy (and its "Quick powers" page).
 *
 * Rules: Vanilla / Rebalanced (default) / Custom (any row changed).
 *
 *   - Half-bar turns (pe_atb_items.c, the "gold bar"): which battle-menu tabs can act from a
 *     half bar for half a bar's cost. Rebalanced: Items, and the Parasite Energy powers set as
 *     quick on the Quick powers page (Heal 1, Heal 2, Detox, Medic, Scan, Slow, Confuse). A
 *     full-bar power cast from a half bar is allowed but holds the next refill back by half a
 *     bar (debt), so it always costs a full bar.
 *   - PE heals scale with Aya's max HP (Heal 1 / 2 / 3: 15 / 30 / 65%, never below the vanilla
 *     30 / 60 / 280), and Detox / Scan cost 60 / 30 PE (vanilla 100 / 50). Both from an entry
 *     hook on Battle_ApplySpellEffect (0x80024250, a0 = power index, in battle and from the
 *     menu): the extra HP is added before the game's own heal (it clamps to max HP after), and
 *     the PE difference is refunded before the game subtracts its fixed cost (PE Light armor,
 *     effect flag 0x200, takes a third off both, as the game does).
 *   - Panacea (item 0x19, pe_custom.c and pe_native_menu.c): Combine Medicine 4 + Full Cure;
 *     restores 400 HP and cures every ailment.
 *   - Scan summary (2026-10-19): vanilla Scan only flashes one "Susceptible to ..." line per
 *     weakness and never shows resistances; Rebalanced adds one on-screen line with both.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

#define PE_FN_APPLY_SPELL   0x80024250u   /* Battle_ApplySpellEffect(idx, target) */
#define PE_PLAYER_ENTITY    0x8009D254u
#define C_PE                0x08u          /* exp_or_acc: PE, 16.16 */
#define C_HP                0x0Cu
#define C_MAX_HP            0x1Cu
#define C_ATTRIBUTES        0x6Cu          /* -> +0x04 effect flags; 0x200 = PE Light */

/* pe_atb_items.c */
extern int g_pe_half_enabled;
extern int g_pe_half_allow[5];            /* Items, Parasite Energy, weapon, armor, escape */
extern uint32_t g_pe_quick_mask;          /* bit i: power i is quick (half bar) */
/* pe_native_menu.c / pe_custom.c */
extern int g_pe_panacea;

enum { RULES_VANILLA, RULES_REBALANCED, RULES_CUSTOM };
static const char *const s_rules_names[] = { "VANILLA", "REBALANCED", "CUSTOM" };
static int s_rules = RULES_REBALANCED, s_rules_shown = -1;

static int s_heal1, s_heal2, s_heal3, s_cost_detox, s_cost_scan, s_scan_summary;
static int s_quick[20];

typedef struct { const char *label, *desc; int vanilla, rebalanced, min, max, step; int *value; int toggle; } PeRow;
static PeRow s_rows[] = {
    { "Half-bar turns", "Gold bar: chosen actions at half.", 0, 1, 0, 1, 1, &g_pe_half_enabled, 1 },
    { "Half bar: Items", "Use items from a half bar.", 0, 1, 0, 1, 1, &g_pe_half_allow[0], 1 },
    { "Half bar: PE quick powers", "Quick powers from a half bar.", 0, 1, 0, 1, 1, &g_pe_half_allow[1], 1 },
    { "Half bar: change weapon", "Swap weapons from a half bar.", 0, 1, 0, 1, 1, &g_pe_half_allow[2], 1 },
    { "Half bar: change armor", "Swap armor from a half bar.", 0, 0, 0, 1, 1, &g_pe_half_allow[3], 1 },
    { "Half bar: escape", "Try to escape from a half bar.", 0, 0, 0, 1, 1, &g_pe_half_allow[4], 1 },
    { "Heal 1 % of max HP", "0 = vanilla (+30 HP).", 0, 15, 0, 100, 5, &s_heal1, 0 },
    { "Heal 2 % of max HP", "0 = vanilla (+60 HP).", 0, 30, 0, 100, 5, &s_heal2, 0 },
    { "Heal 3 % of max HP", "0 = vanilla (+280 HP).", 0, 65, 0, 100, 5, &s_heal3, 0 },
    { "Detox PE cost", "Vanilla 100.", 100, 60, 0, 300, 10, &s_cost_detox, 0 },
    { "Scan PE cost", "Vanilla 50.", 50, 30, 0, 300, 10, &s_cost_scan, 0 },
    { "Scan: weak & strong", "Scan lists weaknesses and resists.", 0, 1, 0, 1, 1, &s_scan_summary, 1 },
    { "Panacea", "Combine Medicine 4 + Full Cure.", 0, 1, 0, 1, 1, &g_pe_panacea, 1 },
};
#define PE_ROWS ((int)(sizeof s_rows / sizeof s_rows[0]))

/* Quick powers (power index, Battle_ApplySpellEffect). Mito14-18 (13-17) don't exist. */
static const struct { int index; const char *label; int rebalanced; } s_powers[] = {
    { 0, "Heal 1", 1 }, { 1, "Heal 2", 1 }, { 2, "Heal 3", 0 }, { 3, "Detox", 1 }, { 4, "Medic", 1 },
    { 5, "Barrier", 0 }, { 6, "Energy Shot", 0 }, { 7, "Scan", 1 }, { 8, "Slow", 1 }, { 9, "Haste", 0 },
    { 10, "Confuse", 1 }, { 11, "Gene Heal", 0 }, { 12, "Preraise", 0 }, { 18, "Full Recover", 0 },
    { 19, "Liberate", 0 },
};
#define PE_POWERS ((int)(sizeof s_powers / sizeof s_powers[0]))
static const char *const s_quick_names[] = { "FULL BAR", "HALF BAR" };

static int pe_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

/* ---- rules ---------------------------------------------------------------------------- */
static void pe_apply(int rules) {
    if (rules == RULES_CUSTOM) return;
    for (int i = 0; i < PE_ROWS; ++i)
        *s_rows[i].value = rules == RULES_VANILLA ? s_rows[i].vanilla : s_rows[i].rebalanced;
    for (int i = 0; i < PE_POWERS; ++i)
        s_quick[s_powers[i].index] = rules == RULES_VANILLA ? 0 : s_powers[i].rebalanced;
}

static int pe_matches(int rules) {
    for (int i = 0; i < PE_ROWS; ++i)
        if (*s_rows[i].value != (rules == RULES_VANILLA ? s_rows[i].vanilla : s_rows[i].rebalanced)) return 0;
    for (int i = 0; i < PE_POWERS; ++i)
        if (s_quick[s_powers[i].index] != (rules == RULES_VANILLA ? 0 : s_powers[i].rebalanced)) return 0;
    return 1;
}

static void pe_balance_tick(void) {
    if (s_rules != s_rules_shown) { pe_apply(s_rules); s_rules_shown = s_rules; }
    else if (s_rules != RULES_CUSTOM && !pe_matches(s_rules)) s_rules = s_rules_shown = RULES_CUSTOM;
    else if (s_rules == RULES_CUSTOM && pe_matches(RULES_REBALANCED)) s_rules = s_rules_shown = RULES_REBALANCED;
    else if (s_rules == RULES_CUSTOM && pe_matches(RULES_VANILLA)) s_rules = s_rules_shown = RULES_VANILLA;
    uint32_t mask = 0;
    for (int i = 0; i < 20; ++i) if (s_quick[i]) mask |= 1u << i;
    g_pe_quick_mask = mask;
}

/* ---- Scan summary ---------------------------------------------------------------------- */
/* Vanilla Scan (Battle_ApplySpellEffect case 7, battle/attack.c:642) shows the target's HP, then
 * one "Susceptible to ..." line per weakness for 75 frames each (Save_DrawSlotMetadata,
 * menu/save_slot.c), and never what the target resists. This adds one line listing both, from the
 * target's statusFlags2 pairs (EnemyCombatant +0xCC; 0 average, 1 resists, 2 weak): bits 0-1
 * poison, 2-3 acid, 4-5 tranquilizer, 8-9 slow, 10-11 confusion, 12-13 steal, 14-15 heat,
 * 16-17 cold (bits 6-7: whether Scan may show HP). */
#define E_STATUS2 0xCCu
static void pe_scan_summary(uint32_t target) {
    static const struct { int shift; const char *name; } pairs[] = {
        { 14, "Heat" }, { 16, "Cold" }, { 0, "Poison" }, { 2, "Acid" }, { 4, "Tranq" },
        { 10, "Confuse" }, { 8, "Slow" }, { 12, "Steal" },
    };
    if (!pe_ptr(target) || target == psx_mod_read_word(PE_PLAYER_ENTITY)) return;
    const uint32_t core = psx_mod_read_word(target);
    if (!pe_ptr(core)) return;
    const uint32_t s2 = psx_mod_read_word(core + E_STATUS2);
    char weak[96] = "", strong[96] = "", text[200];
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
        const unsigned v = (s2 >> pairs[i].shift) & 3u;
        char *dst = v == 2u ? weak : v == 1u ? strong : NULL;
        if (!dst) continue;
        if (dst[0]) strncat(dst, ", ", 95 - strlen(dst));
        strncat(dst, pairs[i].name, 95 - strlen(dst));
    }
    snprintf(text, sizeof text, "Scan  Weak: %s   Strong: %s", weak[0] ? weak : "none", strong[0] ? strong : "none");
    psx_mod_osd_message(text);
    pe_log("scan", "status2=0x%05X weak=%s strong=%s", (unsigned)(s2 & 0x3FFFFu), weak, strong);
}

/* ---- PE numbers: Battle_ApplySpellEffect entry ----------------------------------------- */
static void pe_on_apply_spell(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t idx = cpu->gpr[4];
    if (idx == 7u && s_scan_summary) pe_scan_summary(cpu->gpr[5]);
    const uint32_t ent = psx_mod_read_word(PE_PLAYER_ENTITY);
    if (!pe_ptr(ent)) return;
    const uint32_t core = psx_mod_read_word(ent);
    if (!pe_ptr(core)) return;
    /* Heals: a share of max HP, never below vanilla. */
    static const int vanilla_heal[3] = { 30, 60, 280 };
    if (idx <= 2u) {
        const int pct = idx == 0 ? s_heal1 : idx == 1 ? s_heal2 : s_heal3;
        if (pct > 0) {
            const int maxhp = psx_mod_read_half(core + C_MAX_HP);
            const int want = maxhp * pct / 100;
            if (want > vanilla_heal[idx]) {
                const int hp = psx_mod_read_half(core + C_HP);
                psx_mod_write_half(core + C_HP, (uint16_t)(hp + (want - vanilla_heal[idx])));
            }
        }
    }
    /* Costs: refund the difference before the game subtracts its own. */
    int vanilla_cost = 0, cost = 0;
    if (idx == 3u) { vanilla_cost = 100; cost = s_cost_detox; }
    if (idx == 7u) { vanilla_cost = 50; cost = s_cost_scan; }
    if (vanilla_cost && cost != vanilla_cost) {
        int32_t delta = (int32_t)(vanilla_cost - cost) << 16;
        const uint32_t attr = psx_mod_read_word(core + C_ATTRIBUTES);
        if (pe_ptr(attr) && (psx_mod_read_word(attr + 4u) & 0x200u)) delta = delta * 2 / 3;
        psx_mod_write_word(core + C_PE, (uint32_t)((int32_t)psx_mod_read_word(core + C_PE) + delta));
    }
}

/* Called from pe_mod_menu.c. */
#define PE_PAGE "Items & Parasite Energy"
void pe_pe_balance_register(void) {
    pe_apply(RULES_REBALANCED);
    s_rules_shown = s_rules;
    (void)pe_menu_register_choice(PE_PAGE, "Rules", "Vanilla, Rebalanced, or Custom.", &s_rules, s_rules_names, 3);
    for (int i = 0; i < PE_ROWS; ++i) {
        if (s_rows[i].toggle)
            (void)pe_menu_register_toggle(PE_PAGE, s_rows[i].label, s_rows[i].desc, s_rows[i].value, 0);
        else
            (void)pe_menu_register_int(PE_PAGE, s_rows[i].label, s_rows[i].desc, s_rows[i].value,
                                       s_rows[i].min, s_rows[i].max, s_rows[i].step);
    }
    for (int i = 0; i < PE_POWERS; ++i)
        (void)pe_menu_register_choice(PE_PAGE "/Quick powers", s_powers[i].label,
                                      "Half bar = like an item.", &s_quick[s_powers[i].index], s_quick_names, 2);
    (void)pe_menu_register_tick(pe_balance_tick);
    (void)psx_mod_register_function_entry_plugin("pe.pe-balance.spell", PE_FN_APPLY_SPELL, pe_on_apply_spell);
}
