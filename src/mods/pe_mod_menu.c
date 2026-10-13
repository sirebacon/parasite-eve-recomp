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

int pe_menu_register_grid(const char *sub, const char *label,
                          const char *description, int *cursor,
                          unsigned max_cells, unsigned columns,
                          PeMenuChoiceCount count_fn, PeMenuCell cell_fn,
                          PeMenuAction on_accept, PeMenuAction on_move) {
    return psx_mod_menu_add_grid(pe_page(sub), label, description, cursor,
                                 max_cells, columns,
                                 (PSXModMenuChoiceCount)count_fn,
                                 (PSXModMenuCell)cell_fn, on_accept, on_move)
        ? 0 : -1;
}

static void pe_mod_menu_vblank(void) {
    if (!psx_mod_game_started()) return;
    for (int i = 0; i < s_tick_count; i++) s_ticks[i]();
}

/* ---- movement: always run, run speed, walk speed -------------------------- */
/* Aya's field movement (decomp Scene_PlayerMovement.c): walking
 * (Scene_UpdatePlayerEntity) moves her at 5.0 * moveFactor per logic frame,
 * running (the function named Scene_UpdateEntityFacing, 0x8007136C) at
 * 22.5 * moveFactor. The player's moveFactor is 1.0 (0x10000, Scene_LoadMap).
 * Measured live: walk 2.50 and run 11.25 units per 60 Hz frame (logic is
 * 30 Hz).
 *
 * Patching code does NOT work here: the main EXE is statically recompiled, and
 * neither a RAM write nor psx_mod_write_code_word reaches the native function
 * (verified: the words changed, the speed did not). Everything below changes
 * DATA the code reads instead.
 *
 * SPEED: while Aya moves in the field, moveFactor = 1.0 * the walk or run
 * factor; otherwise it goes back to 1.0, but only from one of our own values,
 * so a value a script set is left alone. No remembered state, so a savestate
 * taken mid-run cannot compound it.
 *
 * ALWAYS RUN: the game builds its field input word (0x8009D26C) from a
 * button-mapping table, one u32 per input bit (D_800A76F0, written by the
 * button config through Gte_StoreTableEntry). The bit that makes Aya run is
 * bit 0 (0x1), mapped to Circle (internal pad bit 0x4000, after the game's
 * Circle/Cross swap); full analog tilt sets it too. Found live: remapping that
 * entry to the d-pad made plain d-pad movement run, while remapping bit 30 (the
 * header's FIELD_PAD_RUN) or bit 10, also mapped to Circle, changed nothing.
 * With Always run on, in free field movement entry 0 maps to the d-pad, so
 * moving means running, and to nothing while the run button is held, so holding
 * it walks. Menus and battle get the original mapping back. Analog mode rebuilds
 * bit 0 from the stick alone (full tilt runs), so Always run is d-pad only.
 *
 * Walk/run is decided from the raw pad (refreshed every frame) rather than the
 * field input word, whose value between frames is not reliable. */
#define PE_PLAYER_PTR_ADDR    0x8009D254u
#define PE_BATTLE_FLAGS_ADDR  0x8009D1A0u   /* game-state block: 2 battle, 4 menu, 0x2000 lock */
#define PE_GAME_STATE_FLAGS   0x800B0CD8u   /* g_GameState.flags; menus etc., see below */
#define PE_PAD_ADDR           0x800BE9A0u   /* status, id, buttons (active low) */
#define PE_PAD_STICK_X        0x800BE9A6u
#define PE_PAD_STICK_Y        0x800BE9A7u
#define PE_RUN_MAP_ADDR       0x800A76F0u   /* D_800A76F0[0]: buttons that make Aya run */
#define PE_RUN_MAP_DPAD       0x00F0u       /* up/right/down/left, internal pad bits */
#define PE_ACTOR_MOVE_FACTOR  0x20u
#define PE_MOVE_FACTOR_STOCK  0x10000u

static const char *const s_speed_names[] = {
    "NORMAL", "1.25X", "1.5X", "1.75X", "2X", "2.25X", "2.5X", "3X",
};
static const uint32_t s_speed_factor[] = {
    0x10000u, 0x14000u, 0x18000u, 0x1C000u, 0x20000u, 0x24000u, 0x28000u, 0x30000u,
};
#define PE_SPEED_CHOICES 8u
static int s_run_choice = 2;  /* default 1.5x: the stock run felt too slow */
static int s_walk_choice = 2; /* default 1.5x, to match */

/* Combat: Aya's own movement in battle uses the same moveFactor (live trace
 * 2026-10-12: at 3x she covered ~1800 units/s against ~440 vanilla). Applied
 * while the battle phase (D_8009D28C) is 0, the phase in which she can move;
 * the walk to her starting spot as a fight opens is phase 7 and does not use
 * moveFactor, so it is not sped up by this. Default normal: faster movement
 * makes dodging easier. */
#define PE_BATTLE_PHASE_ADDR 0x8009D28Cu
static int s_combat_choice = 0;

/* Battle start: as a fight opens (battle phase 7) Aya walks to her spot in
 * action mode 5 with a fixed velocity the game steers toward the spot
 * (BattleEntity +0x68 / +0x70, ~14.5 units per 30 Hz logic frame) -- it does
 * not use moveFactor (live trace 2026-10-12). Each 60 Hz tick adds half of
 * (factor - 1) x that velocity to her position, and her walk animation step
 * (+0x1C) is raised to match. The push stops for the rest of that walk as
 * soon as her heading turns more than ~25 degrees from the one she set out on:
 * the game steers her in gradually near the spot, and pushing along a turning
 * heading made her orbit the spot at 3x (user report 2026-10-13). */
#define PE_ENT_ACTION_MODE   0x0Eu
#define PE_ENT_ANIM_STEP     0x1Cu
#define PE_ENT_POS_X         0x28u
#define PE_ENT_POS_Z         0x30u
#define PE_ENT_MOTION_X      0x68u
#define PE_ENT_MOTION_Z      0x70u
#define PE_ENTRY_ACTION_MODE 5u
#define PE_ANIM_STEP_STOCK   0x10000u
static const char *const s_entry_names[] = { "NORMAL", "1.5X", "2X", "3X", "4X" };
static const uint32_t s_entry_factor[] = { 0x10000u, 0x18000u, 0x20000u, 0x30000u, 0x40000u };
#define PE_ENTRY_CHOICES 5u
static int s_entry_choice = 3;   /* 3x */

static void pe_entry_walk_tick(uint32_t player) {
    static int s_pushing, s_passed;
    static int32_t s_first_mx, s_first_mz;   /* the heading she set out on */
    static uint32_t s_our_step;
    const uint32_t factor = s_entry_factor[(unsigned)s_entry_choice < PE_ENTRY_CHOICES ? (unsigned)s_entry_choice : 0];
    const int walk = (psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 0x2u) &&
                     psx_mod_read_word(PE_BATTLE_PHASE_ADDR) == 7 &&
                     psx_mod_read_byte(player + PE_ENT_ACTION_MODE) == PE_ENTRY_ACTION_MODE;
    const uint32_t step_addr = player + PE_ENT_ANIM_STEP;
    if (!walk || factor == 0x10000u) {
        if (s_pushing && s_our_step && psx_mod_read_word(step_addr) == s_our_step)
            psx_mod_write_word(step_addr, PE_ANIM_STEP_STOCK);
        s_pushing = s_passed = 0;
        s_our_step = 0;
        return;
    }
    const int32_t mx = (int32_t)psx_mod_read_word(player + PE_ENT_MOTION_X);
    const int32_t mz = (int32_t)psx_mod_read_word(player + PE_ENT_MOTION_Z);
    if (!s_pushing) { s_pushing = 1; s_passed = 0; s_first_mx = s_first_mz = 0; }
    if (!mx && !mz) return;
    if (!s_first_mx && !s_first_mz) { s_first_mx = mx; s_first_mz = mz; }
    /* Steering in (heading > ~25 degrees off the first one, cos^2 < 0.81) or
     * turned back: she is near (or past) the spot -- the game finishes alone. */
    {
        const int64_t ax = mx >> 8, az = mz >> 8, bx = s_first_mx >> 8, bz = s_first_mz >> 8;
        const int64_t dot = ax * bx + az * bz;
        if (dot <= 0 || dot * dot * 100 < (ax * ax + az * az) * (bx * bx + bz * bz) * 81) s_passed = 1;
    }
    if (s_passed) return;
    const int64_t extra = (int64_t)factor - 0x10000;          /* 16.16 */
    const int32_t dx = (int32_t)(((int64_t)mx * extra) >> 17);  /* half per 60 Hz tick */
    const int32_t dz = (int32_t)(((int64_t)mz * extra) >> 17);
    psx_mod_write_word(player + PE_ENT_POS_X, (uint32_t)((int32_t)psx_mod_read_word(player + PE_ENT_POS_X) + dx));
    psx_mod_write_word(player + PE_ENT_POS_Z, (uint32_t)((int32_t)psx_mod_read_word(player + PE_ENT_POS_Z) + dz));
    const uint32_t step = psx_mod_read_word(step_addr);
    if (step == PE_ANIM_STEP_STOCK || (s_our_step && step == s_our_step)) {
        s_our_step = factor;                  /* animation at the same speed */
        if (step != s_our_step) psx_mod_write_word(step_addr, s_our_step);
    }
}
static int s_always_run = 1;
static uint32_t s_run_mask = 0x4000u; /* the run bit's own buttons (Circle) */

static int pe_is_boost_value(uint32_t mf) {
    for (unsigned i = 1; i < PE_SPEED_CHOICES; i++)
        if (mf == s_speed_factor[i]) return 1;
    return 0;
}

static uint32_t pe_speed(int choice) {
    return s_speed_factor[(unsigned)choice < PE_SPEED_CHOICES ? (unsigned)choice : 0];
}

/* Experiments before the decomp patched a scripted-motion opcode
 * (Entity_ComputeVelocity, lui $a0 5 -> 8 or 20), not Aya's movement.
 * Savestates made back then still carry it; put the stock instruction back. */
#define PE_OLD_MOVE_ADDR   0x800185C0u
#define PE_OLD_MOVE_RETAIL 0x3C040005u

static void pe_move_tick(void) {
    const uint32_t old = psx_mod_read_word(PE_OLD_MOVE_ADDR);
    if (old == 0x3C040008u || old == 0x3C040014u)
        psx_mod_write_code_word(PE_OLD_MOVE_ADDR, PE_OLD_MOVE_RETAIL);

    /* Raw pad -> the game's internal button bits (Circle/Cross swapped, as in
     * the game's own pad read). The buffer is all zero before the game sets it
     * up, which would read as every button held, so require a valid header. */
    const uint32_t pad = psx_mod_read_word(PE_PAD_ADDR);
    const uint32_t id = (pad >> 8) & 0xFFu;
    const int pad_ok = (pad & 0xFFu) == 0 && id != 0 && id != 0xFFu;
    const uint32_t inv = pad_ok ? (~(pad >> 16) & 0xFFFFu) : 0;
    uint32_t bits = inv & 0x9FFFu;
    if (inv & 0x2000u) bits |= 0x4000u;
    if (inv & 0x4000u) bits |= 0x2000u;
    const int dpad = (bits & PE_RUN_MAP_DPAD) != 0;
    int stick = 0, stick_full = 0;
    if (pad_ok && (pad & 0xF000u) == 0x7000u) {
        const uint32_t x = psx_mod_read_byte(PE_PAD_STICK_X);
        const uint32_t y = psx_mod_read_byte(PE_PAD_STICK_Y);
        stick = x < 0x5Au || x >= 0xA1u || y < 0x5Au || y >= 0xA1u;
        stick_full = x < 0x14u || x >= 0xE7u || y < 0x14u || y >= 0xE7u;
    }

    /* Free field movement: no battle (block 2), no menu (block 4, state 0x9000,
     * set by Entity_FrameUpdate when the item menu opens) and none of the
     * states the game itself requires clear before opening that menu (state
     * 0x3400, block 0x2000); 0x200/0x400 also gate the pad read for menus. */
    const int in_field = !(psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 0x2006u) &&
                         !(psx_mod_read_word(PE_GAME_STATE_FLAGS) & 0xB600u);

    /* Always run: retarget the run entry of the mapping table. Any value that
     * is not one of ours is the game's (or the player's button config) and is
     * remembered as the original. */
    const uint32_t map = psx_mod_read_word(PE_RUN_MAP_ADDR);
    if (map != PE_RUN_MAP_DPAD && map != 0) s_run_mask = map;
    const int run_held = (bits & s_run_mask) != 0;
    uint32_t want_map = s_run_mask;
    if (s_always_run && in_field) want_map = run_held ? 0 : PE_RUN_MAP_DPAD;
    if (want_map != map) psx_mod_write_word(PE_RUN_MAP_ADDR, want_map);

    /* Speed. */
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (player < 0x80000000u || player >= 0x80200000u) return;
    pe_entry_walk_tick(player);
    const uint32_t addr = player + PE_ACTOR_MOVE_FACTOR;
    const uint32_t mf = psx_mod_read_word(addr);
    const int moving = in_field && (dpad || stick);
    int running;
    if (dpad) running = s_always_run ? !run_held : run_held;
    else      running = run_held || stick_full;

    const int combat = (psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 0x2u) &&
                       psx_mod_read_word(PE_BATTLE_PHASE_ADDR) == 0;
    uint32_t want = mf;
    if (combat && (mf == PE_MOVE_FACTOR_STOCK || pe_is_boost_value(mf)))
        want = pe_speed(s_combat_choice);
    else if (moving && (mf == PE_MOVE_FACTOR_STOCK || pe_is_boost_value(mf)))
        want = pe_speed(running ? s_run_choice : s_walk_choice);
    else if (!moving && pe_is_boost_value(mf))
        want = PE_MOVE_FACTOR_STOCK;
    if (want != mf) psx_mod_write_word(addr, want);
}
/* Every PE row is registered from here, in one fixed order, so the page reads
 * the same regardless of the order the other files' constructors run in:
 *
 *   Mods > PE Enhancements
 *     Always run
 *     Run speed
 *     Walk speed
 *     God mode
 *     Stats        >
 *     Item Editor  >
 *     Debug        >
 *
 * (A sub-page's folder row sits where its first row was registered.) */
void pe_god_mode_menu_register(void);   /* pe_god_mode.c */
void pe_cheats_menu_register(void);     /* pe_cheats.c */
void pe_weapon_power_menu_register(void); /* pe_weapon_power.c */
void pe_atb_items_menu_register(void);  /* pe_atb_items.c */
void pe_wayne_refund_menu_register(void); /* pe_wayne_refund.c */
void pe_loot_menu_register(void);       /* pe_loot.c */
void pe_loot_catalog_menu_register(void); /* pe_loot_catalog.c */
void pe_loot_shop_menu_register(void);    /* pe_loot_shop.c */
void pe_native_menu_register(void);       /* pe_native_menu.c */
void pe_wayne_menu_register(void);        /* pe_wayne_menu.c */
void pe_catalog_menu_register(void);      /* pe_catalog_menu.c */
void pe_loot_hooks_register(void);      /* pe_loot.c, after the loot logger */
void pe_stats_menu_register(void);      /* pe_stats_menu.c */
void pe_item_editor_register(void);     /* pe_item_editor.c */
void pe_debug_menu_register(void);      /* pe_debug_menu.c */
void pe_loot_logger_menu_register(void); /* pe_loot_logger.c (Debug page) */
void pe_custom_register(void);           /* pe_custom.c, after the loot hooks */

PSX_MOD_CONSTRUCTOR(pe_register_mod_menu_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.mod-menu", pe_mod_menu_vblank);
    (void)pe_menu_register_toggle(0, "Always run",
        "Hold the run button to walk.",
        &s_always_run, 0);
    (void)pe_menu_register_choice(0, "Run speed",
        "How fast Aya runs in the field.",
        &s_run_choice, s_speed_names, PE_SPEED_CHOICES);
    (void)pe_menu_register_choice(0, "Walk speed",
        "How fast Aya walks in the field.",
        &s_walk_choice, s_speed_names, PE_SPEED_CHOICES);
    (void)pe_menu_register_choice(0, "Combat speed",
        "How fast Aya moves during battle.",
        &s_combat_choice, s_speed_names, PE_SPEED_CHOICES);
    (void)pe_menu_register_choice(0, "Battle start speed",
        "Aya's walk to her spot as a fight opens.",
        &s_entry_choice, s_entry_names, PE_ENTRY_CHOICES);
    (void)pe_menu_register_tick(pe_move_tick);
    pe_god_mode_menu_register();
    pe_cheats_menu_register();
    pe_weapon_power_menu_register();
    pe_atb_items_menu_register();
    pe_wayne_refund_menu_register();
    pe_loot_menu_register();
    pe_loot_catalog_menu_register();
    pe_loot_shop_menu_register();
    pe_native_menu_register();
    pe_wayne_menu_register();
    pe_catalog_menu_register();
    pe_stats_menu_register();
    pe_item_editor_register();
    pe_debug_menu_register();
    pe_loot_logger_menu_register();
    pe_loot_hooks_register();
    pe_custom_register();
}
