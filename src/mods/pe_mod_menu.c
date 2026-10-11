/*
 * In-game mod menu: PE-side registration for the runtime's F1 menu.
 *
 * An earlier version drew its own overlay with raw GP0 commands and masked the
 * pad buffer. That could not pause the game, did not show over FMVs and leaked
 * input into the game, all of which the runtime's own pause menu already
 * handles, so the drawing and input code is gone. What remains is the registry
 * (pe_mod_menu.h), one VBlank plugin that runs each toggle's per-frame tick,
 * and the first toggle, faster field movement.
 *
 * State is plain C statics (not guest memory, not in any save structure), so it
 * resets on every launch. Persistence is an open question.
 */
#include <stdint.h>
#include <stddef.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"

/* ---- registry ------------------------------------------------------------ */
#define PE_MENU_MAX_TICKS 16

static PeMenuPerFrame s_ticks[PE_MENU_MAX_TICKS];
static int            s_tick_count;

int pe_menu_register_toggle(const char *label, const char *description,
                            int *flag, PeMenuPerFrame per_frame) {
    if (per_frame && s_tick_count >= PE_MENU_MAX_TICKS) return -1;
    if (!psx_mod_menu_add_bool(label, description, flag)) return -1;
    if (per_frame) s_ticks[s_tick_count++] = per_frame;
    return 0;
}

int pe_menu_register_action(const char *label, const char *description,
                            PeMenuAction action) {
    return psx_mod_menu_add_action(label, description, action) ? 0 : -1;
}

static void pe_mod_menu_vblank(void) {
    if (!psx_mod_game_started()) return;
    for (int i = 0; i < s_tick_count; i++) s_ticks[i]();
}

/* ---- first toggle: faster field movement (live code write) --------------- */
/* Entity_ComputeVelocity (0x80018598): lui $a0,0x0005 (5.0x) -> 0x0014 (20.0x).
 * Written with psx_mod_write_code_word so a restored savestate cannot leave a
 * stale compiled instruction. Only the two known encodings are ever replaced;
 * anything else means another mod owns the word, so it is left alone. */
#define PE_MOVE_ADDR   0x800185C0u
#define PE_MOVE_RETAIL 0x3C040005u
#define PE_MOVE_FAST   0x3C040014u

static int s_move_fast = 1; /* default on: matches the shipped live patch */

static void pe_move_tick(void) {
    const uint32_t want = s_move_fast ? PE_MOVE_FAST : PE_MOVE_RETAIL;
    const uint32_t cur  = psx_mod_read_word(PE_MOVE_ADDR);
    if (cur == want) return;
    if (cur != PE_MOVE_RETAIL && cur != PE_MOVE_FAST) return;
    psx_mod_write_code_word(PE_MOVE_ADDR, want);
}

PSX_MOD_CONSTRUCTOR(pe_register_mod_menu_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.mod-menu", pe_mod_menu_vblank);
    (void)pe_menu_register_toggle("Faster movement (20x)",
        "Field movement speed: 20x instead of the stock 5x.",
        &s_move_fast, pe_move_tick);
}
