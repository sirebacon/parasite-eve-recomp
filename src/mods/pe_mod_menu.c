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

/* Every PE row is registered from here, in one fixed order, so the page reads
 * the same regardless of the order the other files' constructors run in:
 *
 *   Mods > PE Enhancements
 *     Faster movement (20x)
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
    (void)pe_menu_register_toggle(0, "Faster movement (20x)",
        "Field speed 20x instead of stock 5x.",
        &s_move_fast, pe_move_tick);
    pe_god_mode_menu_register();
    pe_stats_menu_register();
    pe_item_editor_register();
    pe_debug_menu_register();
}
