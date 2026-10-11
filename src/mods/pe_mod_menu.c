/*
 * In-game mod menu: PE-side registration for the runtime's F1 menu.
 *
 * An earlier version drew its own overlay with raw GP0 commands and masked the
 * pad buffer. That could not pause the game, did not show over FMVs and leaked
 * input into the game, all of which the runtime's own pause menu already
 * handles, so the drawing and input code is gone. What remains is the registry
 * (pe_mod_menu.h), one VBlank plugin that runs each toggle's per-frame tick,
 * and the run speed setting.
 *
 * State is plain C statics (not guest memory, not in any save structure), so it
 * resets on every launch. Persistence is an open question.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"

/* ---- registry ------------------------------------------------------------ */
#define PE_MENU_MAX_TICKS 16

static PeMenuPerFrame s_ticks[PE_MENU_MAX_TICKS];
static int            s_tick_count;

/* "PE Enhancements" or "PE Enhancements/<sub>". The runtime copies the
 * string, so one buffer serves every call. */
static const char *pe_page(const char *sub) {
    static char page[96];
    if (!sub || !*sub) return PE_MENU_PAGE;
    snprintf(page, sizeof(page), "%s/%s", PE_MENU_PAGE, sub);
    return page;
}

int pe_menu_register_toggle(const char *sub, const char *label,
                            const char *description, int *flag,
                            PeMenuPerFrame per_frame) {
    if (per_frame && s_tick_count >= PE_MENU_MAX_TICKS) return -1;
    if (!psx_mod_menu_add_bool(pe_page(sub), label, description, flag)) return -1;
    if (per_frame) s_ticks[s_tick_count++] = per_frame;
    return 0;
}

int pe_menu_register_tick(PeMenuPerFrame fn) {
    if (!fn || s_tick_count >= PE_MENU_MAX_TICKS) return -1;
    s_ticks[s_tick_count++] = fn;
    return 0;
}

int pe_menu_register_int(const char *sub, const char *label,
                         const char *description, int *value,
                         int minimum, int maximum, int step) {
    return psx_mod_menu_add_int(pe_page(sub), label, description, value,
                                minimum, maximum, step) ? 0 : -1;
}

int pe_menu_register_action(const char *sub, const char *label,
                            const char *description, PeMenuAction action) {
    return psx_mod_menu_add_action(pe_page(sub), label, description, action)
        ? 0 : -1;
}

int pe_menu_register_choice(const char *sub, const char *label,
                            const char *description, int *index,
                            const char *const *names, unsigned count) {
    return psx_mod_menu_add_choice(pe_page(sub), label, description, index,
                                   names, count) ? 0 : -1;
}

int pe_menu_register_live_int(const char *sub, const char *label,
                              const char *description, int minimum,
                              int maximum, int step, PeMenuGet get,
                              PeMenuSet set) {
    return psx_mod_menu_add_live_int(pe_page(sub), label, description, minimum,
                                     maximum, step, get, set) ? 0 : -1;
}

int pe_menu_register_live_choice(const char *sub, const char *label,
                                 const char *description,
                                 const char *const *names, unsigned count,
                                 PeMenuGet get, PeMenuSet set) {
    return psx_mod_menu_add_live_choice(pe_page(sub), label, description,
                                        names, count, get, set) ? 0 : -1;
}

int pe_menu_register_dynamic_choice(const char *sub, const char *label,
                                    const char *description, int *index,
                                    unsigned max_count,
                                    PeMenuChoiceCount count_fn,
                                    PeMenuChoiceName name_fn) {
    return psx_mod_menu_add_dynamic_choice(pe_page(sub), label, description,
                                           index, max_count,
                                           (PSXModMenuChoiceCount)count_fn,
                                           (PSXModMenuChoiceName)name_fn)
        ? 0 : -1;
}

static void pe_mod_menu_vblank(void) {
    if (!psx_mod_game_started()) return;
    for (int i = 0; i < s_tick_count; i++) s_ticks[i]();
}

/* ---- run speed ------------------------------------------------------------ */
/* Aya's field run handler (decomp Scene_PlayerMovement.c, the function named
 * Scene_UpdateEntityFacing at 0x8007136C) moves her at 22.5 * moveFactor per
 * logic frame; walking (Scene_UpdatePlayerEntity) is 5.0 * moveFactor. The
 * player's moveFactor is 1.0 (0x10000, set by Scene_LoadMap). Measured live:
 * walk 2.50 and run 11.25 units per 60 Hz frame (logic runs at 30 Hz).
 *
 * Patching the 22.5 constant does NOT work: the main EXE is statically
 * recompiled, and neither a RAM write nor psx_mod_write_code_word reaches the
 * native function (both verified: the words changed, the speed did not). That
 * is also why the old "faster movement" patch never did anything. Data does
 * reach it, so this scales moveFactor while Aya runs instead:
 *   - running = field input word (0x8009D26C) has the run bit 0x40000000 and a
 *     direction, and the battle flag (0x8009D1A0 & 2) is clear;
 *   - while running, moveFactor = 1.0 * the chosen factor; otherwise it goes
 *     back to 1.0, but only from one of our own values, so a value a script
 *     set is left alone. No remembered state, so a savestate taken mid-run
 *     cannot compound the boost. */
#define PE_PLAYER_PTR_ADDR    0x8009D254u
#define PE_FIELD_INPUT_ADDR   0x8009D26Cu
#define PE_BATTLE_FLAGS_ADDR  0x8009D1A0u
#define PE_ACTOR_MOVE_FACTOR  0x20u
#define PE_FIELD_PAD_RUN      0x40000000u
#define PE_FIELD_PAD_DIRS     0x78u   /* down 0x08, right 0x10, up 0x20, left 0x40 */
#define PE_MOVE_FACTOR_STOCK  0x10000u

static const char *const s_run_names[] = { "NORMAL", "1.5X", "2X", "3X" };
static const uint32_t s_run_factor[] = { 0x10000u, 0x18000u, 0x20000u, 0x30000u };
#define PE_RUN_CHOICES 4u
static int s_run_choice = 1; /* default 1.5x: the stock run felt too slow */

static int pe_is_boost_value(uint32_t mf) {
    for (unsigned i = 1; i < PE_RUN_CHOICES; i++)
        if (mf == s_run_factor[i]) return 1;
    return 0;
}

/* Experiments before the decomp patched a scripted-motion opcode
 * (Entity_ComputeVelocity, lui $a0 5 -> 8 or 20), not Aya's movement.
 * Savestates made back then still carry it; put the stock instruction back. */
#define PE_OLD_MOVE_ADDR   0x800185C0u
#define PE_OLD_MOVE_RETAIL 0x3C040005u

static void pe_run_tick(void) {
    const uint32_t old = psx_mod_read_word(PE_OLD_MOVE_ADDR);
    if (old == 0x3C040008u || old == 0x3C040014u)
        psx_mod_write_code_word(PE_OLD_MOVE_ADDR, PE_OLD_MOVE_RETAIL);

    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (player < 0x80000000u || player >= 0x80200000u) return;
    const uint32_t addr = player + PE_ACTOR_MOVE_FACTOR;
    const uint32_t mf = psx_mod_read_word(addr);

    const uint32_t input = psx_mod_read_word(PE_FIELD_INPUT_ADDR);
    const int running = (input & PE_FIELD_PAD_RUN) && (input & PE_FIELD_PAD_DIRS) &&
                        !(psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 2u);
    const unsigned choice = (unsigned)s_run_choice < PE_RUN_CHOICES ? (unsigned)s_run_choice : 0;

    uint32_t want = mf;
    if (running && choice > 0 && (mf == PE_MOVE_FACTOR_STOCK || pe_is_boost_value(mf)))
        want = s_run_factor[choice];
    else if (pe_is_boost_value(mf))
        want = PE_MOVE_FACTOR_STOCK;
    if (want != mf) psx_mod_write_word(addr, want);
}

/* Every PE row is registered from here, in one fixed order, so the page reads
 * the same regardless of the order the other files' constructors run in:
 *
 *   Mods > PE Enhancements
 *     Run speed
 *     God mode
 *     Stats        >
 *     Item Editor  >
 *     Debug        >
 *
 * (A sub-page's folder row sits where its first row was registered.) */
void pe_god_mode_menu_register(void);   /* pe_god_mode.c */
void pe_stats_menu_register(void);      /* pe_stats_menu.c */
void pe_item_editor_register(void);     /* pe_item_editor.c */
void pe_debug_menu_register(void);      /* pe_debug_menu.c */

PSX_MOD_CONSTRUCTOR(pe_register_mod_menu_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.mod-menu", pe_mod_menu_vblank);
    (void)pe_menu_register_choice(0, "Run speed",
        "Field running only. Walk is unchanged.",
        &s_run_choice, s_run_names, PE_RUN_CHOICES);
    (void)pe_menu_register_tick(pe_run_tick);
    pe_god_mode_menu_register();
    pe_stats_menu_register();
    pe_item_editor_register();
    pe_debug_menu_register();
}
