/*
 * Half-bar ATB: chosen battle actions become usable once Aya's turn gauge is
 * half full, and cost half a bar (also when used from a full bar). Each action
 * on the battle menu's tabs can be allowed separately; by default only Items.
 * Attacks always need, and use, a full bar.
 *
 * The game (decomp):
 *   - Aya's turn gauge is Combatant +0x10 (the decomp's "hpAlive"), 0..9000.
 *   - Battle_Update (0x800299CC), battle phase 0 (D_8009D28C), starts a turn
 *     only when gauge >= 9000: pressed bit 0x200 starts targeting, bit 0x80
 *     (Triangle) opens the battle menu (item menu state 0x8009D1F0 = 1). Both
 *     move to phase 1 (Battle_HandleItemMenu); FinishAction() empties the gauge
 *     and returns to phase 0.
 *   - The battle menu's tabs come from g_MenuItemContextFlag (0x8009CEF0) bits
 *     0..4: Use Item, Parasite Energy, Change Weapon, Change Armor, Escape
 *     (Menu_CreateInventoryTabView counts the set bits; story scripts set it).
 *   - The chosen action is queued (queue 0x800BE830, 8-byte entries, command at
 *     +4, count D_8009CE3C); Battle_StepScriptEntry: 1..2 shots, 3..0x182
 *     items, 0x183..0x196 Parasite Energy, 0x197..0x198 equipment change,
 *     0x199+ escape.
 *
 * The mod, from a build-time hook on Battle_Update entry (game.toml
 * mod_function_entry_funcs), which runs after this frame's pressed bits are
 * known and right before the gauge check:
 *   1. Borrow. Phase 0, Triangle pressed (attack not), gauge 4500..8999 and at
 *      least one action allowed: save the gauge, Aya's bag counts and equipped
 *      slots, clear the tab bits of actions that aren't allowed (saving the
 *      flag), set the gauge to 9000 and pre-set the "gauge full" chime latch.
 *      The game opens the menu with only the allowed tabs.
 *   2. During the turn, note the most significant queued action. On a borrowed
 *      bar, closing the menu without doing anything presses cancel for the
 *      player and drops attack, so the borrowed bar can't be spent on shots.
 *   3. Turn end (back in phase 0): restore the tab flag, then
 *        borrowed + allowed action    -> gauge = saved - 4500
 *        borrowed + nothing done      -> gauge = saved
 *        borrowed + anything else     -> gauge = 0 (no free actions)
 *        full bar + allowed action    -> gauge = 4500 (half cost)
 *   The bar is tinted gold while it's between half and full.
 *   4. The fill rate. The gauge fills by scaledOffense (+0x24) a battle frame,
 *      set from the queued action as stat22 (+0x22) * scale[action id] / 10
 *      (Battle_SetupEnemyAnims; the scale table at 0x800106D4 is 0, 10, 8,
 *      10, 8, 8, 4, 10, 20). Opening the battle menu leaves Aya with a blank
 *      action, id 0, whose scale is 0: harmless on a full bar, but a borrowed
 *      bar put back below full then never filled again (user report
 *      2026-10-13: frozen at 7840 after backing out of the menu). The rate at
 *      the borrow is saved and put back when the turn ends, and any free
 *      moment with the gauge below full and a zero rate gets the last real
 *      rate back.
 *   The turn bookkeeping rides in save states, so loading an F7 state saved
 *   mid-borrow refunds and restores the tabs exactly as if play had continued.
 *
 * Parasite Energy (2026-10-15, pe_pe_balance.c): only the powers in g_pe_quick_mask (the
 * queued command 0x183 + power index) cost half a bar; a full-bar power cast from a half bar
 * empties the gauge and leaves a debt of half a bar that the next refill pays first, so it
 * always costs a full bar. The settings live on F1 > Items & Parasite Energy.
 */
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

#define PE_BATTLE_UPDATE       0x800299CCu
#define PE_PLAYER_PTR_ADDR     0x8009D254u
#define PE_BATTLE_PHASE        0x8009D28Cu /* 0 = free, 1 = turn (targeting/menu) */
#define PE_PRESSED_BITS        0x8009D1F4u /* this frame's newly pressed battle bits */
#define PE_ITEM_MENU_STATE     0x8009D1F0u /* g_BattleItemMenuState */
#define PE_READY_CHIME_LATCH   0x8009D288u /* u8 */
#define PE_MENU_TAB_FLAGS      0x8009CEF0u /* g_MenuItemContextFlag */
#define PE_QUEUE_ADDR          0x800BE830u
#define PE_QUEUE_STRIDE        8u
#define PE_QUEUE_COMMAND       4u
#define PE_QUEUE_COUNT         0x8009CE3Cu /* u8 */
#define PE_GAME_BLOCK_ADDR     0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE   0x2u
#define PE_AYA_SLOTS_ADDR      0x800C0E48u
#define PE_AYA_SLOT_COUNT      50
#define PE_WEAPON_SLOT_ADDR    0x800C0E20u
#define PE_ARMOR_SLOT_ADDR     0x800C0E22u

#define COMBATANT_TURN_GAUGE   0x10u
#define COMBATANT_STAT22       0x22u
#define COMBATANT_FILL_RATE    0x24u               /* scaledOffense: gauge step a battle frame */
#define COMBATANT_STATE_FLAGS  0x4Cu               /* 0x10000: battle ended */
#define PE_GAUGE_FULL          9000u
#define PE_GAUGE_HALF          4500u

#define PRESS_ITEM    0x0080u
#define PRESS_ATTACK  0x0200u
#define PRESS_CANCEL  0x2000u

/* Battle-menu actions, in tab-bit order. */
enum { ACT_ITEM, ACT_PE, ACT_WEAPON, ACT_ARMOR, ACT_ESCAPE, ACT_COUNT,
       ACT_NONE = -1, ACT_ATTACK = -2, ACT_OTHER = -3 };

/* Settings (F1 > Items & Parasite Energy, pe_pe_balance.c). */
int g_pe_half_enabled = 1;                      /* master switch */
int g_pe_half_allow[ACT_COUNT] = { 1, 1, 0, 0, 0 };
uint32_t g_pe_quick_mask = 0x59Bu;              /* Heal 1/2, Detox, Medic, Scan, Slow, Confuse */
#define s_enabled g_pe_half_enabled
#define s_allow g_pe_half_allow
static const char *const s_power_names[20] = {
    "Heal1", "Heal2", "Heal3", "Detox", "Medic", "Barrier", "EnergyShot", "Scan", "Slow",
    "Haste", "Confuse", "GeneHeal", "Preraise", "Mito14", "Mito15", "Mito16", "Mito17",
    "Mito18", "FullRecover", "Liberate",
};

/* Bookkeeping that moves in step with guest RAM (the borrowed gauge, the
 * hidden tabs, the tinted bar). It is save-state data (psx_mod_register_state),
 * so an F7 state saved mid-turn loads with the bookkeeping that matches it.
 * Bump PE_ATB_STATE_VERSION when the layout changes. */
#define PE_ATB_STATE_VERSION 3u
static struct {
    int borrowed, borrow_pending, turn_active, track_full;
    unsigned saved_gauge;
    unsigned saved_rate, last_rate;              /* fill rate at the borrow / last nonzero */
    int action;                                  /* most significant action this turn */
    int tab_saved;
    uint32_t saved_tabs;
    uint16_t bag[PE_AYA_SLOT_COUNT];
    uint8_t weapon_slot, armor_slot;
    uint8_t normal[2][4][3];                     /* the game's bar colours */
    int have_normal, tinted;
    int pe_index;                                /* the PE power queued this turn, or -1 */
    unsigned debt, last_gauge;                   /* half bar owed by a full power cast early */
} s_st;

static int pe_ram_pointer(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u;
}

/* ---- half-bar indicator ----------------------------------------------------
 * The ATB bar is a Gouraud quad in two draw buffers at 0x800B00E8 + slot*0x24
 * (Battle_DrawActiveStatus); its four vertex colours sit at +4/+0xC/+0x14/+0x1C
 * (left-top, right-top, left-bottom, right-bottom). The game only rewrites
 * them while the bar is full (its flash), so between half and full the mod
 * tints the bar gold, and puts the game's own colours back below half. */
#define PE_ATB_QUAD_BASE    0x800B00E8u
#define PE_ATB_QUAD_STRIDE  0x24u
static const uint32_t s_vertex_color_offsets[4] = { 0x04u, 0x0Cu, 0x14u, 0x1Cu };
static const uint8_t s_gold[4][3] = {
    { 0x60, 0x40, 0x00 }, { 0xFF, 0xD8, 0x40 },   /* left dark, right bright */
    { 0x60, 0x40, 0x00 }, { 0xFF, 0xD8, 0x40 },
};

static void pe_bar_colors(int save, const uint8_t (*src)[3]) {
    for (uint32_t slot = 0; slot < 2; ++slot)
        for (int v = 0; v < 4; ++v)
            for (uint32_t c = 0; c < 3; ++c) {
                const uint32_t a = PE_ATB_QUAD_BASE + slot * PE_ATB_QUAD_STRIDE +
                                   s_vertex_color_offsets[v] + c;
                if (save) s_st.normal[slot][v][c] = psx_mod_read_byte(a);
                else psx_mod_write_byte(a, src ? src[v][c] : s_st.normal[slot][v][c]);
            }
}

static int pe_any_allowed(void) {
    for (int a = 0; a < ACT_COUNT; ++a) if (s_allow[a]) return 1;
    return 0;
}

static void pe_update_indicator(unsigned gauge) {
    const int want = s_enabled && pe_any_allowed() &&
                     gauge >= PE_GAUGE_HALF && gauge < PE_GAUGE_FULL;
    if (want) {
        if (!s_st.tinted && !s_st.have_normal) { pe_bar_colors(1, 0); s_st.have_normal = 1; }
        pe_bar_colors(0, s_gold);
        s_st.tinted = 1;
    } else if (s_st.tinted) {
        if (gauge < PE_GAUGE_FULL && s_st.have_normal) pe_bar_colors(0, 0); /* full: game flashes */
        s_st.tinted = 0;
    } else if (gauge < PE_GAUGE_HALF) {
        pe_bar_colors(1, 0);                     /* remember the game's colours */
        s_st.have_normal = 1;
    }
}

/* ---- turn bookkeeping ------------------------------------------------------ */
/* Item counts, ignoring order (Move doesn't count as using an item). */
static void pe_bag_read(uint16_t *out) {
    for (int i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        out[i] = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)i * 2u);
}

static int pe_bag_contents_changed(void) {
    uint16_t now[PE_AYA_SLOT_COUNT];
    pe_bag_read(now);
    for (int i = 0; i < PE_AYA_SLOT_COUNT; ++i) {
        int before = 0, after = 0;
        for (int j = 0; j < PE_AYA_SLOT_COUNT; ++j) {
            before += s_st.bag[j] == s_st.bag[i];
            after += now[j] == s_st.bag[i];
        }
        if (before != after) return 1;
    }
    return 0;
}

static void pe_snapshot(void) {
    pe_bag_read(s_st.bag);
    s_st.weapon_slot = psx_mod_read_byte(PE_WEAPON_SLOT_ADDR);
    s_st.armor_slot = psx_mod_read_byte(PE_ARMOR_SLOT_ADDR);
    s_st.action = ACT_NONE;
    s_st.pe_index = -1;
}

static int pe_classify(unsigned command) {
    if (command >= 1 && command <= 2) return ACT_ATTACK;
    if (command >= 3 && command < 0x183) return ACT_ITEM;
    if (command >= 0x183 && command < 0x197) return ACT_PE;
    if (command >= 0x197 && command < 0x199)
        return psx_mod_read_byte(PE_ARMOR_SLOT_ADDR) != s_st.armor_slot ? ACT_ARMOR : ACT_WEAPON;
    if (command >= 0x199) return ACT_ESCAPE;
    return ACT_OTHER;
}

/* Fold in the queue and the equipment slots. Attacks win over everything. */
static void pe_note_actions(void) {
    const unsigned count = psx_mod_read_byte(PE_QUEUE_COUNT);
    for (unsigned i = 0; i < count && i < 32; ++i) {
        const unsigned command = psx_mod_read_half(PE_QUEUE_ADDR + i * PE_QUEUE_STRIDE + PE_QUEUE_COMMAND);
        const int act = pe_classify(command);
        if (act == ACT_PE) s_st.pe_index = (int)(command - 0x183u);
        if (act == ACT_ATTACK || s_st.action == ACT_NONE) s_st.action = act;
    }
    if (s_st.action == ACT_NONE) {
        if (psx_mod_read_byte(PE_WEAPON_SLOT_ADDR) != s_st.weapon_slot) s_st.action = ACT_WEAPON;
        else if (psx_mod_read_byte(PE_ARMOR_SLOT_ADDR) != s_st.armor_slot) s_st.action = ACT_ARMOR;
        else if (pe_bag_contents_changed()) s_st.action = ACT_ITEM;
    }
}

static int pe_action_allowed(int act) {
    if (act == ACT_PE)                           /* only the quick powers */
        return s_allow[ACT_PE] && s_st.pe_index >= 0 && s_st.pe_index < 20 &&
               ((g_pe_quick_mask >> s_st.pe_index) & 1u);
    return act >= 0 && act < ACT_COUNT && s_allow[act];
}

static void pe_log_pe_turn(const char *cost, unsigned from, unsigned to) {
    if (s_st.action != ACT_PE || s_st.pe_index < 0 || s_st.pe_index >= 20) return;
    pe_log("pe_turn", "power=%s cost=%s gauge=%u->%u", s_power_names[s_st.pe_index], cost, from, to);
}

static void pe_restore_tabs(void) {
    if (s_st.tab_saved) psx_mod_write_word(PE_MENU_TAB_FLAGS, s_st.saved_tabs);
    s_st.tab_saved = 0;
}

static void pe_reset(void) {
    pe_restore_tabs();
    s_st.borrowed = s_st.borrow_pending = s_st.turn_active = s_st.track_full = 0;
}

/* A gauge below full must be filling: put back a zero rate (see 4. above). */
static void pe_fix_rate(uint32_t core, unsigned preferred) {
    const uint32_t rate_addr = core + COMBATANT_FILL_RATE;
    const unsigned rate = psx_mod_read_half(rate_addr);
    if (rate) { s_st.last_rate = rate; return; }
    if (psx_mod_read_half(core + COMBATANT_TURN_GAUGE) >= PE_GAUGE_FULL) return;
    if (psx_mod_read_word(core + COMBATANT_STATE_FLAGS) & 0x10000u) return;
    unsigned fix = preferred ? preferred : s_st.last_rate;
    if (!fix) fix = psx_mod_read_half(core + COMBATANT_STAT22) * 4u / 10u;   /* a gun's scale */
    if (!fix) fix = 1;
    psx_mod_write_half(rate_addr, (uint16_t)fix);
}

/* ---- the hook --------------------------------------------------------------- */
static void pe_on_battle_update(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE)) {
        pe_reset();
        s_st.tinted = s_st.have_normal = 0;     /* HUD packets are rebuilt per battle */
        s_st.debt = 0;
        return;
    }
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (!pe_ram_pointer(player)) return;
    const uint32_t core = psx_mod_read_word(player);
    if (!pe_ram_pointer(core)) return;
    const uint32_t gauge_addr = core + COMBATANT_TURN_GAUGE;
    const uint32_t phase = psx_mod_read_word(PE_BATTLE_PHASE);
    pe_update_indicator(s_st.borrowed ? s_st.saved_gauge : psx_mod_read_half(gauge_addr));

    if (phase == 1) {
        if (!s_st.turn_active) {
            s_st.turn_active = 1;
            s_st.track_full = !s_st.borrowed && s_enabled;   /* a normal full-bar turn */
            if (s_st.track_full) pe_snapshot();
        }
        s_st.borrow_pending = 0;
        if (s_st.borrowed || s_st.track_full) pe_note_actions();
        /* Menu closed without doing anything: end the borrowed turn instead
         * of letting the borrowed bar be spent on shots. */
        if (s_st.borrowed && s_st.action == ACT_NONE &&
            psx_mod_read_byte(PE_ITEM_MENU_STATE) == 0) {
            const uint32_t pressed = psx_mod_read_word(PE_PRESSED_BITS);
            psx_mod_write_word(PE_PRESSED_BITS, (pressed | PRESS_CANCEL) & ~PRESS_ATTACK);
        }
        return;
    }
    if (phase != 0) return;

    if (s_st.turn_active) {                             /* the turn just ended */
        if (s_st.borrowed || s_st.track_full) pe_note_actions();
        if (s_st.borrowed) {
            unsigned gauge = 0;
            if (s_st.action == ACT_NONE) gauge = s_st.saved_gauge;
            else if (pe_action_allowed(s_st.action))
                gauge = s_st.saved_gauge > PE_GAUGE_HALF ? s_st.saved_gauge - PE_GAUGE_HALF : 0;
            else if (s_st.action == ACT_PE) {    /* a full power from a half bar: the rest is owed */
                const unsigned paid = s_st.saved_gauge;
                s_st.debt = paid < PE_GAUGE_FULL ? PE_GAUGE_FULL - paid : 0;
            }
            psx_mod_write_half(gauge_addr, (uint16_t)gauge);
            pe_log_pe_turn(pe_action_allowed(s_st.action) ? "half" : "full_debt", s_st.saved_gauge, gauge);
        } else if (s_st.track_full && pe_action_allowed(s_st.action)) {
            psx_mod_write_half(gauge_addr, (uint16_t)PE_GAUGE_HALF);
            pe_log_pe_turn("half", PE_GAUGE_FULL, PE_GAUGE_HALF);
        } else if (s_st.track_full) {
            pe_log_pe_turn("full", PE_GAUGE_FULL, 0);
        }
        s_st.last_gauge = psx_mod_read_half(gauge_addr);
        pe_fix_rate(core, s_st.borrowed && s_st.action == ACT_NONE ? s_st.saved_rate : 0);
        pe_reset();
        return;
    }
    if (s_st.borrow_pending) {                          /* the game didn't take it */
        psx_mod_write_half(gauge_addr, (uint16_t)s_st.saved_gauge);
        pe_fix_rate(core, s_st.saved_rate);
        pe_reset();
        return;
    }
    pe_fix_rate(core, 0);                               /* every free moment */
    {                                                   /* a debt is paid from the refill first */
        unsigned g = psx_mod_read_half(gauge_addr);
        if (s_st.debt && g > s_st.last_gauge && g < PE_GAUGE_FULL) {
            unsigned take = g - s_st.last_gauge;
            if (take > s_st.debt) take = s_st.debt;
            g -= take;
            s_st.debt -= take;
            psx_mod_write_half(gauge_addr, (uint16_t)g);
        }
        s_st.last_gauge = g;
    }
    if (!s_enabled || !pe_any_allowed()) return;

    const unsigned gauge = psx_mod_read_half(gauge_addr);
    const uint32_t pressed = psx_mod_read_word(PE_PRESSED_BITS);
    if ((pressed & PRESS_ITEM) && !(pressed & PRESS_ATTACK) &&
        gauge >= PE_GAUGE_HALF && gauge < PE_GAUGE_FULL) {
        s_st.saved_gauge = gauge;
        s_st.saved_rate = psx_mod_read_half(core + COMBATANT_FILL_RATE);
        pe_snapshot();
        s_st.saved_tabs = psx_mod_read_word(PE_MENU_TAB_FLAGS);
        s_st.tab_saved = 1;
        uint32_t tabs = s_st.saved_tabs;
        for (int a = 0; a < ACT_COUNT; ++a)
            if (!s_allow[a]) tabs &= ~(1u << a);
        psx_mod_write_word(PE_MENU_TAB_FLAGS, tabs);
        psx_mod_write_half(gauge_addr, (uint16_t)PE_GAUGE_FULL);
        psx_mod_write_byte(PE_READY_CHIME_LATCH, 1);   /* u8 in the decomp */
        s_st.borrowed = s_st.borrow_pending = 1;
    }
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
/* The rows moved to F1 > Items & Parasite Energy (pe_pe_balance.c, 2026-10-15). */
void pe_atb_items_menu_register(void) {}

/* A state without our block (taken before this build, or an older layout) has
 * no bookkeeping that matches its RAM: start clean and leave the game alone. */
static void pe_atb_state_loaded(int result, uint32_t saved_version,
                                const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_st, 0, sizeof s_st);
}

PSX_MOD_CONSTRUCTOR(pe_register_atb_items_hook) {
    (void)psx_mod_register_function_entry_plugin("pe.half-bar-atb",
        PE_BATTLE_UPDATE, pe_on_battle_update);
    (void)psx_mod_register_state("pe.half-bar-atb", PE_ATB_STATE_VERSION,
        &s_st, sizeof s_st, pe_atb_state_loaded);
}
