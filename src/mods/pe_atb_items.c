/*
 * Half-bar items: Aya can use an item once her turn gauge is half full, and an
 * item costs half a bar. Attacks and Parasite Energy still need a full bar.
 *
 * The game (decomp, Battle_Update.c / Battle_TargetAndEnemyTurnFlow.c):
 *   - Aya's turn gauge is Combatant +0x10 (the decomp's "hpAlive"), 0..9000.
 *   - Battle_Update (0x800299CC), in battle phase 0 (D_8009D28C), lets a turn
 *     start only when gauge >= 9000: pressed bit 0x200 starts targeting, bit
 *     0x80 (Triangle) opens the item list (item menu state 0x8009D1F0 = 1).
 *     Both move to phase 1 (Battle_HandleItemMenu).
 *   - FinishAction() empties the gauge when the turn ends and returns to phase 0.
 *
 * The mod ("borrow and refund"), from a build-time hook on Battle_Update entry
 * (game.toml mod_function_entry_funcs), so it runs after the pad routine has
 * computed this frame's pressed bits (D_8009D1F4) and right before the check:
 *   1. Phase 0, item button pressed (attack not), gauge 4500..8999: save the
 *      gauge and Aya's bag, set the gauge to 9000, and pre-set the "gauge full"
 *      chime latch (D_8009D288) so no chime plays. The game opens the item list.
 *   2. Phase 1 on a borrowed bar: if the item list was closed without using an
 *      item (state back to 0, bag unchanged, nothing queued), press cancel for
 *      the player and drop the attack-confirm bit, so the borrowed bar can't be
 *      spent on an attack.
 *   3. Back in phase 0: an item was used (bag changed) -> gauge = saved - 4500;
 *      nothing used -> gauge = saved. An item turn started from a full bar also
 *      leaves 4500 when an item was used.
 *   If the game didn't open the list on the borrow frame, the gauge is put back.
 */
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_BATTLE_UPDATE       0x800299CCu
#define PE_PLAYER_PTR_ADDR     0x8009D254u
#define PE_BATTLE_PHASE        0x8009D28Cu /* 0 = free, 1 = turn (targeting/items) */
#define PE_PRESSED_BITS        0x8009D1F4u /* this frame's newly pressed battle bits */
#define PE_ITEM_MENU_STATE     0x8009D1F0u /* g_BattleItemMenuState */
#define PE_READY_CHIME_LATCH   0x8009D288u
#define PE_QUEUE_COUNT         0x8009CE3Cu /* commands queued this turn (u8) */
#define PE_GAME_BLOCK_ADDR     0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE   0x2u
#define PE_AYA_SLOTS_ADDR      0x800C0E48u
#define PE_AYA_SLOT_COUNT      50

#define COMBATANT_TURN_GAUGE   0x10u
#define PE_GAUGE_FULL          9000u
#define PE_GAUGE_HALF          4500u

#define PRESS_ITEM    0x0080u
#define PRESS_ATTACK  0x0200u
#define PRESS_CANCEL  0x2000u

static int s_half_bar_items;           /* menu toggle */

static int s_borrowed;                 /* gauge raised to full for an item turn */
static int s_borrow_pending;           /* borrow made this frame; turn not seen yet */
static int s_turn_active;              /* phase 1 seen */
static unsigned s_saved_gauge;         /* real gauge before the turn */
static uint16_t s_bag[PE_AYA_SLOT_COUNT];

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
static uint8_t s_normal[2][4][3];
static int s_have_normal, s_tinted;

static void pe_bar_colors(int save, const uint8_t (*src)[3]) {
    for (uint32_t slot = 0; slot < 2; ++slot)
        for (int v = 0; v < 4; ++v)
            for (uint32_t c = 0; c < 3; ++c) {
                const uint32_t a = PE_ATB_QUAD_BASE + slot * PE_ATB_QUAD_STRIDE +
                                   s_vertex_color_offsets[v] + c;
                if (save) s_normal[slot][v][c] = psx_mod_read_byte(a);
                else psx_mod_write_byte(a, src ? src[v][c] : s_normal[slot][v][c]);
            }
}

static void pe_update_indicator(unsigned gauge, int battle) {
    const int want = battle && s_half_bar_items &&
                     gauge >= PE_GAUGE_HALF && gauge < PE_GAUGE_FULL;
    if (want) {
        if (!s_tinted && !s_have_normal) { pe_bar_colors(1, 0); s_have_normal = 1; }
        pe_bar_colors(0, s_gold);
        s_tinted = 1;
    } else if (s_tinted) {
        if (gauge < PE_GAUGE_FULL && s_have_normal) pe_bar_colors(0, 0); /* full: game flashes */
        s_tinted = 0;
    } else if (battle && gauge < PE_GAUGE_HALF) {
        pe_bar_colors(1, 0);                     /* remember the game's colours */
        s_have_normal = 1;
    }
}

static int pe_ram_pointer(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u;
}

static void pe_snapshot_bag(void) {
    for (int i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        s_bag[i] = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)i * 2u);
}

static int pe_bag_changed(void) {
    for (int i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        if (s_bag[i] != psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)i * 2u)) return 1;
    return 0;
}

static void pe_reset(void) {
    s_borrowed = s_borrow_pending = s_turn_active = 0;
}

static void pe_on_battle_update(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE)) {
        pe_reset();
        s_tinted = s_have_normal = 0;     /* HUD packets are rebuilt per battle */
        return;
    }
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (!pe_ram_pointer(player)) return;
    const uint32_t core = psx_mod_read_word(player);
    if (!pe_ram_pointer(core)) return;
    const uint32_t gauge_addr = core + COMBATANT_TURN_GAUGE;
    const uint32_t phase = psx_mod_read_word(PE_BATTLE_PHASE);
    pe_update_indicator(s_borrowed ? s_saved_gauge : psx_mod_read_half(gauge_addr), 1);

    if (phase == 1) {
        if (!s_turn_active) {
            s_turn_active = 1;
            if (!s_borrowed && s_half_bar_items) {   /* a normal full-bar turn */
                s_saved_gauge = PE_GAUGE_FULL;
                pe_snapshot_bag();
            }
        }
        s_borrow_pending = 0;
        /* Item list closed without using anything: end the turn instead of
         * letting the borrowed bar be spent on an attack. */
        if (s_borrowed && psx_mod_read_byte(PE_ITEM_MENU_STATE) == 0 &&
            psx_mod_read_byte(PE_QUEUE_COUNT) == 0 && !pe_bag_changed()) {
            uint32_t pressed = psx_mod_read_word(PE_PRESSED_BITS);
            psx_mod_write_word(PE_PRESSED_BITS, (pressed | PRESS_CANCEL) & ~PRESS_ATTACK);
        }
        return;
    }
    if (phase != 0) return;

    if (s_turn_active) {                             /* the turn just ended */
        if (s_half_bar_items && (s_borrowed || s_saved_gauge == PE_GAUGE_FULL)) {
            unsigned gauge = s_saved_gauge;
            if (pe_bag_changed()) gauge = gauge > PE_GAUGE_HALF ? gauge - PE_GAUGE_HALF : 0;
            else if (!s_borrowed) gauge = psx_mod_read_half(gauge_addr); /* normal turn: leave it */
            psx_mod_write_half(gauge_addr, (uint16_t)gauge);
        }
        s_saved_gauge = 0;
        pe_reset();
        return;
    }
    if (s_borrow_pending) {                          /* the game didn't take it */
        psx_mod_write_half(gauge_addr, (uint16_t)s_saved_gauge);
        pe_reset();
        return;
    }
    if (!s_half_bar_items) return;

    const unsigned gauge = psx_mod_read_half(gauge_addr);
    const uint32_t pressed = psx_mod_read_word(PE_PRESSED_BITS);
    if ((pressed & PRESS_ITEM) && !(pressed & PRESS_ATTACK) &&
        gauge >= PE_GAUGE_HALF && gauge < PE_GAUGE_FULL) {
        s_saved_gauge = gauge;
        pe_snapshot_bag();
        psx_mod_write_half(gauge_addr, (uint16_t)PE_GAUGE_FULL);
        psx_mod_write_byte(PE_READY_CHIME_LATCH, 1);   /* u8 in the decomp */
        s_borrowed = s_borrow_pending = 1;
    }
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_atb_items_menu_register(void) {
    (void)pe_menu_register_toggle(0, "Half-bar items",
        "Gold bar: items usable, cost half.", &s_half_bar_items, 0);
}

PSX_MOD_CONSTRUCTOR(pe_register_atb_items_hook) {
    (void)psx_mod_register_function_entry_plugin("pe.half-bar-items",
        PE_BATTLE_UPDATE, pe_on_battle_update);
}
