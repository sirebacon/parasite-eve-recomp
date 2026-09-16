/*
 * Bonus Points floor: guarantees Aya always has at least this many unspent
 * bonus points to allocate on the stat-growth screen.
 *
 * BACKGROUND: unlike max_hp/current_hp (derived caches recomputed from
 * level/EXP), g_AyaBonusPoints (0x800C0E10, u32) is documented as genuinely
 * PERSISTENT save state (include/pe1/aya.h: "[CONFIRMED 0 -> \"BONUS POINT
 * 0\"]; clamp 0x1869F" -- the game itself clamps to 99999, so this mod's own
 * ceiling matches that rather than inventing a new one). Being persistent
 * rather than recomputed means a plain floor-on-write is safe here for the
 * same reason it wasn't for HP: nothing else is going to silently revert it
 * back down except the player actually spending points, which this mod
 * should not fight -- it only raises the value back up when the game (or the
 * player's own spending) has dropped it below the configured floor, exactly
 * like pe_inventory_floor.c's own floor semantics.
 *
 * Every VBlank rather than a one-shot New Game hook, so this is
 * savestate-safe by construction (see pe_inventory_floor.c's retirement note
 * on why a one-time activation plugin is the wrong shape for this kind of
 * fix) and keeps working across every future level-up, not just the first.
 *
 * GATING PITFALL AVOIDED: bonus_points reading 0 is NOT a safe "character
 * not loaded yet" signal here, unlike the inventory floor's slot count --
 * 0 is the genuine, documented value at New Game ("[CONFIRMED 0 -> \"BONUS
 * POINT 0\"]"), so skipping on current==0 would silently do nothing for
 * every fresh save, exactly the case this mod exists for. psx_mod_game_
 * started() alone isn't precise enough either: it flips true from early
 * boot (title screen included), well before Aya's save struct is
 * populated -- a title-screen read and a real New-Game-with-zero-points
 * read are otherwise indistinguishable. Gate on max_hp (0x800C0E06)
 * instead: confirmed non-zero (45) as soon as a real character exists,
 * zero beforehand, and unaffected by whether the player has spent every
 * bonus point down to zero.
 */
#include "mod_plugins.h"

#define PE_MAX_HP_ADDR 0x800C0E06u
#define PE_BONUS_POINTS_ADDR 0x800C0E10u
#define PE_BONUS_POINTS_GAME_CLAMP 99999u

static uint32_t pe_bonus_points_floor_value(void) {
    char text[16] = "";
    if (!psx_mod_option_value("pe.enhancement.bonus-points-floor",
                              "bonus-points-floor", "floor", text,
                              sizeof text) || !text[0]) {
        return 999u;
    }
    uint32_t value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return 999u;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value == 0u || value > PE_BONUS_POINTS_GAME_CLAMP) return 999u;
    return value;
}

static void pe_bonus_points_floor_vblank(void) {
    if (!psx_mod_game_started()) return;

    /* See the file header: max_hp, not bonus_points itself, is the reliable
     * "a real character is loaded" signal here. */
    if (psx_mod_read_half(PE_MAX_HP_ADDR) == 0) return;

    const uint32_t current = psx_mod_read_word(PE_BONUS_POINTS_ADDR);
    const uint32_t floor_value = pe_bonus_points_floor_value();
    if (current < floor_value) {
        psx_mod_write_word(PE_BONUS_POINTS_ADDR, floor_value);
    }
}

PSX_MOD_CONSTRUCTOR(pe_register_bonus_points_floor_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.bonus-points-floor",
                                         pe_bonus_points_floor_vblank);
}
