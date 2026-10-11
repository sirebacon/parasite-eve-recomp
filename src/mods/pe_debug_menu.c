/*
 * Debug rows for the in-game menu (listed in F1's Mods section, "Debug: ...").
 *
 * Each action runs with the guest paused at a VBlank boundary and edits live
 * guest memory directly. These are development aids, not part of the shipped
 * enhancement set, and every one reports through psx_mod_osd_message so it is
 * obvious whether it did anything.
 *
 * Planned, each needing decomp research first: give items (AyaSaveState
 * inventory s16[50] at 0x800C0E48), edit stats, warp to a location.
 */
#include <stdint.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_MAX_HP_ADDR              0x800C0E06u
#define PE_BONUS_POINTS_ADDR        0x800C0E10u
#define PE_BONUS_POINTS_GAME_CLAMP  99999u /* the game's own clamp (aya.h) */
#define PE_BONUS_POINTS_GRANT       1000u

static void pe_debug_add_bonus_points(void) {
    /* Same "a real character is loaded" gate as pe_bonus_points_floor.c: max HP
     * is zero before a save is loaded, and writing then would be overwritten. */
    if (!psx_mod_game_started() || psx_mod_read_half(PE_MAX_HP_ADDR) == 0) {
        psx_mod_osd_message("No save loaded yet");
        return;
    }
    uint32_t points = psx_mod_read_word(PE_BONUS_POINTS_ADDR) + PE_BONUS_POINTS_GRANT;
    if (points > PE_BONUS_POINTS_GAME_CLAMP) points = PE_BONUS_POINTS_GAME_CLAMP;
    psx_mod_write_word(PE_BONUS_POINTS_ADDR, points);
    psx_mod_osd_message("Added bonus points");
}

PSX_MOD_CONSTRUCTOR(pe_register_debug_menu) {
    (void)pe_menu_register_action("Debug: add 1000 bonus points",
        "Spend them on the stat screen. Capped at the game's own 99999.",
        pe_debug_add_bonus_points);
}
