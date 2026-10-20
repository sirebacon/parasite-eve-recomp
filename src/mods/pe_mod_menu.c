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
 *
 * Every change made through the menu goes to the play log (pe_log.h): rows
 * bound to a plugin int (toggles, numbers, choices) are compared every frame
 * and a change logs "tweak <page>/<label> old->new"; actions, grid accepts and
 * the setters of live rows go through small trampolines that log "action" /
 * "tweak" before calling the real one. The Stats page mirrors the game into
 * its ints, so it logs its own edits (pe_stats_menu.c); dynamic choices and
 * grid cursors are only selections and aren't logged.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

/* ---- registry ------------------------------------------------------------ */
#define PE_MENU_MAX_TICKS 16

static PeMenuPerFrame s_ticks[PE_MENU_MAX_TICKS];
static int            s_tick_count;

/* ---- play log: what was changed from the menu --------------------------- */
#define PE_WATCH_MAX 128
#define PE_WRAP_MAX  64

typedef struct {
    const char *sub, *label;
    int *value;
    int last, def;                            /* last seen, value at registration */
    const char *const *names;
    unsigned count;
} PeWatch;
static PeWatch s_watch[PE_WATCH_MAX];
static int s_watch_count, s_watch_ready;

static const char *pe_sub_name(const char *sub) { return sub && *sub ? sub : "PE"; }

static void pe_watch(const char *sub, const char *label, int *value,
                     const char *const *names, unsigned count) {
    if (!value || s_watch_count >= PE_WATCH_MAX) return;
    if (sub && strcmp(sub, "Stats") == 0) return;      /* mirrored; logs itself */
    s_watch[s_watch_count++] = (PeWatch){ sub, label, value, *value, *value, names, count };
}

static void pe_value_text(char *out, size_t size, int v, const char *const *names, unsigned count) {
    if (names && v >= 0 && (unsigned)v < count) snprintf(out, size, "%s", names[v]);
    else snprintf(out, size, "%d", v);
}

static void pe_watch_tick(void) {
    for (int i = 0; i < s_watch_count; ++i) {
        PeWatch *w = &s_watch[i];
        const int v = *w->value;
        if (s_watch_ready && v != w->last) {
            char a[32], b[32];
            pe_value_text(a, sizeof a, w->last, w->names, w->count);
            pe_value_text(b, sizeof b, v, w->names, w->count);
            pe_log("tweak", "%s/%s %s->%s", pe_sub_name(w->sub), w->label, a, b);
        }
        w->last = v;
    }
    s_watch_ready = 1;
}

/* Every row not at its default, so a log shows what was already on (God mode,
 * No random battles, speeds...) when play started or a save loaded. */
extern int g_pe_bp_floor_active;   /* pe_bonus_points_floor.c: launcher packages, */
extern int g_pe_inv_floor_active;  /* pe_inventory_floor.c: not F1 rows */

void pe_menu_log_settings(void) {
    char out[300];
    size_t n = 0;
    out[0] = 0;
    if (g_pe_bp_floor_active)
        n += (size_t)snprintf(out + n, sizeof out - n, "package/Bonus Points Floor=%d", g_pe_bp_floor_active);
    if (g_pe_inv_floor_active)
        n += (size_t)snprintf(out + n, sizeof out - n, "%spackage/Personal Inventory Floor=%d",
                              n ? " " : "", g_pe_inv_floor_active);
    for (int i = 0; i < s_watch_count && n < sizeof out; ++i) {
        const PeWatch *w = &s_watch[i];
        const int v = *w->value;
        if (v == w->def) continue;
        char text[32];
        pe_value_text(text, sizeof text, v, w->names, w->count);
        n += (size_t)snprintf(out + n, sizeof out - n, "%s%s/%s=%s", n ? " " : "",
                              pe_sub_name(w->sub), w->label, text);
    }
    pe_log("settings", "%s", n ? out : "all defaults");
}

typedef struct { const char *sub, *label; PeMenuAction fn; int accept; } PeWrapAction;
static PeWrapAction s_wrap_action[PE_WRAP_MAX];
static int s_wrap_action_count;

static void pe_action_fire(int i) {
    const PeWrapAction *w = &s_wrap_action[i];
    pe_log("action", "%s/%s%s", pe_sub_name(w->sub), w->label, w->accept ? " (accept)" : "");
    if (w->fn) w->fn();
}

typedef struct {
    const char *sub, *label;
    PeMenuGet get;
    PeMenuSet set;
    const char *const *names;
    unsigned count;
} PeWrapSetter;
static PeWrapSetter s_wrap_setter[PE_WRAP_MAX];
static int s_wrap_setter_count;

static int pe_setter_fire(int i, int value) {
    const PeWrapSetter *w = &s_wrap_setter[i];
    const int old = w->get ? w->get() : 0;
    const int ok = w->set ? w->set(value) : 0;
    if (ok && value != old) {
        char a[32], b[32];
        pe_value_text(a, sizeof a, old, w->names, w->count);
        pe_value_text(b, sizeof b, value, w->names, w->count);
        pe_log("tweak", "%s/%s %s->%s", pe_sub_name(w->sub), w->label, a, b);
    }
    return ok;
}

static void pe_act_0(void) { pe_action_fire(0); }
static void pe_act_1(void) { pe_action_fire(1); }
static void pe_act_2(void) { pe_action_fire(2); }
static void pe_act_3(void) { pe_action_fire(3); }
static void pe_act_4(void) { pe_action_fire(4); }
static void pe_act_5(void) { pe_action_fire(5); }
static void pe_act_6(void) { pe_action_fire(6); }
static void pe_act_7(void) { pe_action_fire(7); }
static void pe_act_8(void) { pe_action_fire(8); }
static void pe_act_9(void) { pe_action_fire(9); }
static void pe_act_10(void) { pe_action_fire(10); }
static void pe_act_11(void) { pe_action_fire(11); }
static void pe_act_12(void) { pe_action_fire(12); }
static void pe_act_13(void) { pe_action_fire(13); }
static void pe_act_14(void) { pe_action_fire(14); }
static void pe_act_15(void) { pe_action_fire(15); }
static void pe_act_16(void) { pe_action_fire(16); }
static void pe_act_17(void) { pe_action_fire(17); }
static void pe_act_18(void) { pe_action_fire(18); }
static void pe_act_19(void) { pe_action_fire(19); }
static void pe_act_20(void) { pe_action_fire(20); }
static void pe_act_21(void) { pe_action_fire(21); }
static void pe_act_22(void) { pe_action_fire(22); }
static void pe_act_23(void) { pe_action_fire(23); }
static void pe_act_24(void) { pe_action_fire(24); }
static void pe_act_25(void) { pe_action_fire(25); }
static void pe_act_26(void) { pe_action_fire(26); }
static void pe_act_27(void) { pe_action_fire(27); }
static void pe_act_28(void) { pe_action_fire(28); }
static void pe_act_29(void) { pe_action_fire(29); }
static void pe_act_30(void) { pe_action_fire(30); }
static void pe_act_31(void) { pe_action_fire(31); }
static void pe_act_32(void) { pe_action_fire(32); }
static void pe_act_33(void) { pe_action_fire(33); }
static void pe_act_34(void) { pe_action_fire(34); }
static void pe_act_35(void) { pe_action_fire(35); }
static void pe_act_36(void) { pe_action_fire(36); }
static void pe_act_37(void) { pe_action_fire(37); }
static void pe_act_38(void) { pe_action_fire(38); }
static void pe_act_39(void) { pe_action_fire(39); }
static void pe_act_40(void) { pe_action_fire(40); }
static void pe_act_41(void) { pe_action_fire(41); }
static void pe_act_42(void) { pe_action_fire(42); }
static void pe_act_43(void) { pe_action_fire(43); }
static void pe_act_44(void) { pe_action_fire(44); }
static void pe_act_45(void) { pe_action_fire(45); }
static void pe_act_46(void) { pe_action_fire(46); }
static void pe_act_47(void) { pe_action_fire(47); }
static void pe_act_48(void) { pe_action_fire(48); }
static void pe_act_49(void) { pe_action_fire(49); }
static void pe_act_50(void) { pe_action_fire(50); }
static void pe_act_51(void) { pe_action_fire(51); }
static void pe_act_52(void) { pe_action_fire(52); }
static void pe_act_53(void) { pe_action_fire(53); }
static void pe_act_54(void) { pe_action_fire(54); }
static void pe_act_55(void) { pe_action_fire(55); }
static void pe_act_56(void) { pe_action_fire(56); }
static void pe_act_57(void) { pe_action_fire(57); }
static void pe_act_58(void) { pe_action_fire(58); }
static void pe_act_59(void) { pe_action_fire(59); }
static void pe_act_60(void) { pe_action_fire(60); }
static void pe_act_61(void) { pe_action_fire(61); }
static void pe_act_62(void) { pe_action_fire(62); }
static void pe_act_63(void) { pe_action_fire(63); }

static const PeMenuAction s_act_tramp[PE_WRAP_MAX] = {
    pe_act_0,
    pe_act_1,
    pe_act_2,
    pe_act_3,
    pe_act_4,
    pe_act_5,
    pe_act_6,
    pe_act_7,
    pe_act_8,
    pe_act_9,
    pe_act_10,
    pe_act_11,
    pe_act_12,
    pe_act_13,
    pe_act_14,
    pe_act_15,
    pe_act_16,
    pe_act_17,
    pe_act_18,
    pe_act_19,
    pe_act_20,
    pe_act_21,
    pe_act_22,
    pe_act_23,
    pe_act_24,
    pe_act_25,
    pe_act_26,
    pe_act_27,
    pe_act_28,
    pe_act_29,
    pe_act_30,
    pe_act_31,
    pe_act_32,
    pe_act_33,
    pe_act_34,
    pe_act_35,
    pe_act_36,
    pe_act_37,
    pe_act_38,
    pe_act_39,
    pe_act_40,
    pe_act_41,
    pe_act_42,
    pe_act_43,
    pe_act_44,
    pe_act_45,
    pe_act_46,
    pe_act_47,
    pe_act_48,
    pe_act_49,
    pe_act_50,
    pe_act_51,
    pe_act_52,
    pe_act_53,
    pe_act_54,
    pe_act_55,
    pe_act_56,
    pe_act_57,
    pe_act_58,
    pe_act_59,
    pe_act_60,
    pe_act_61,
    pe_act_62,
    pe_act_63,
};
static int pe_set_0(int v) { return pe_setter_fire(0, v); }
static int pe_set_1(int v) { return pe_setter_fire(1, v); }
static int pe_set_2(int v) { return pe_setter_fire(2, v); }
static int pe_set_3(int v) { return pe_setter_fire(3, v); }
static int pe_set_4(int v) { return pe_setter_fire(4, v); }
static int pe_set_5(int v) { return pe_setter_fire(5, v); }
static int pe_set_6(int v) { return pe_setter_fire(6, v); }
static int pe_set_7(int v) { return pe_setter_fire(7, v); }
static int pe_set_8(int v) { return pe_setter_fire(8, v); }
static int pe_set_9(int v) { return pe_setter_fire(9, v); }
static int pe_set_10(int v) { return pe_setter_fire(10, v); }
static int pe_set_11(int v) { return pe_setter_fire(11, v); }
static int pe_set_12(int v) { return pe_setter_fire(12, v); }
static int pe_set_13(int v) { return pe_setter_fire(13, v); }
static int pe_set_14(int v) { return pe_setter_fire(14, v); }
static int pe_set_15(int v) { return pe_setter_fire(15, v); }
static int pe_set_16(int v) { return pe_setter_fire(16, v); }
static int pe_set_17(int v) { return pe_setter_fire(17, v); }
static int pe_set_18(int v) { return pe_setter_fire(18, v); }
static int pe_set_19(int v) { return pe_setter_fire(19, v); }
static int pe_set_20(int v) { return pe_setter_fire(20, v); }
static int pe_set_21(int v) { return pe_setter_fire(21, v); }
static int pe_set_22(int v) { return pe_setter_fire(22, v); }
static int pe_set_23(int v) { return pe_setter_fire(23, v); }
static int pe_set_24(int v) { return pe_setter_fire(24, v); }
static int pe_set_25(int v) { return pe_setter_fire(25, v); }
static int pe_set_26(int v) { return pe_setter_fire(26, v); }
static int pe_set_27(int v) { return pe_setter_fire(27, v); }
static int pe_set_28(int v) { return pe_setter_fire(28, v); }
static int pe_set_29(int v) { return pe_setter_fire(29, v); }
static int pe_set_30(int v) { return pe_setter_fire(30, v); }
static int pe_set_31(int v) { return pe_setter_fire(31, v); }
static int pe_set_32(int v) { return pe_setter_fire(32, v); }
static int pe_set_33(int v) { return pe_setter_fire(33, v); }
static int pe_set_34(int v) { return pe_setter_fire(34, v); }
static int pe_set_35(int v) { return pe_setter_fire(35, v); }
static int pe_set_36(int v) { return pe_setter_fire(36, v); }
static int pe_set_37(int v) { return pe_setter_fire(37, v); }
static int pe_set_38(int v) { return pe_setter_fire(38, v); }
static int pe_set_39(int v) { return pe_setter_fire(39, v); }
static int pe_set_40(int v) { return pe_setter_fire(40, v); }
static int pe_set_41(int v) { return pe_setter_fire(41, v); }
static int pe_set_42(int v) { return pe_setter_fire(42, v); }
static int pe_set_43(int v) { return pe_setter_fire(43, v); }
static int pe_set_44(int v) { return pe_setter_fire(44, v); }
static int pe_set_45(int v) { return pe_setter_fire(45, v); }
static int pe_set_46(int v) { return pe_setter_fire(46, v); }
static int pe_set_47(int v) { return pe_setter_fire(47, v); }
static int pe_set_48(int v) { return pe_setter_fire(48, v); }
static int pe_set_49(int v) { return pe_setter_fire(49, v); }
static int pe_set_50(int v) { return pe_setter_fire(50, v); }
static int pe_set_51(int v) { return pe_setter_fire(51, v); }
static int pe_set_52(int v) { return pe_setter_fire(52, v); }
static int pe_set_53(int v) { return pe_setter_fire(53, v); }
static int pe_set_54(int v) { return pe_setter_fire(54, v); }
static int pe_set_55(int v) { return pe_setter_fire(55, v); }
static int pe_set_56(int v) { return pe_setter_fire(56, v); }
static int pe_set_57(int v) { return pe_setter_fire(57, v); }
static int pe_set_58(int v) { return pe_setter_fire(58, v); }
static int pe_set_59(int v) { return pe_setter_fire(59, v); }
static int pe_set_60(int v) { return pe_setter_fire(60, v); }
static int pe_set_61(int v) { return pe_setter_fire(61, v); }
static int pe_set_62(int v) { return pe_setter_fire(62, v); }
static int pe_set_63(int v) { return pe_setter_fire(63, v); }

static const PeMenuSet s_set_tramp[PE_WRAP_MAX] = {
    pe_set_0,
    pe_set_1,
    pe_set_2,
    pe_set_3,
    pe_set_4,
    pe_set_5,
    pe_set_6,
    pe_set_7,
    pe_set_8,
    pe_set_9,
    pe_set_10,
    pe_set_11,
    pe_set_12,
    pe_set_13,
    pe_set_14,
    pe_set_15,
    pe_set_16,
    pe_set_17,
    pe_set_18,
    pe_set_19,
    pe_set_20,
    pe_set_21,
    pe_set_22,
    pe_set_23,
    pe_set_24,
    pe_set_25,
    pe_set_26,
    pe_set_27,
    pe_set_28,
    pe_set_29,
    pe_set_30,
    pe_set_31,
    pe_set_32,
    pe_set_33,
    pe_set_34,
    pe_set_35,
    pe_set_36,
    pe_set_37,
    pe_set_38,
    pe_set_39,
    pe_set_40,
    pe_set_41,
    pe_set_42,
    pe_set_43,
    pe_set_44,
    pe_set_45,
    pe_set_46,
    pe_set_47,
    pe_set_48,
    pe_set_49,
    pe_set_50,
    pe_set_51,
    pe_set_52,
    pe_set_53,
    pe_set_54,
    pe_set_55,
    pe_set_56,
    pe_set_57,
    pe_set_58,
    pe_set_59,
    pe_set_60,
    pe_set_61,
    pe_set_62,
    pe_set_63,
};

static PeMenuAction pe_wrap_action(const char *sub, const char *label, PeMenuAction fn, int accept) {
    if (!fn || s_wrap_action_count >= PE_WRAP_MAX) return fn;
    s_wrap_action[s_wrap_action_count] = (PeWrapAction){ sub, label, fn, accept };
    return s_act_tramp[s_wrap_action_count++];
}

static PeMenuSet pe_wrap_setter(const char *sub, const char *label, PeMenuGet get, PeMenuSet set,
                                const char *const *names, unsigned count) {
    if (!set || s_wrap_setter_count >= PE_WRAP_MAX) return set;
    s_wrap_setter[s_wrap_setter_count] = (PeWrapSetter){ sub, label, get, set, names, count };
    return s_set_tramp[s_wrap_setter_count++];
}

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
    pe_watch(sub, label, flag, 0, 0);
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
    if (!psx_mod_menu_add_int(pe_page(sub), label, description, value,
                              minimum, maximum, step)) return -1;
    pe_watch(sub, label, value, 0, 0);
    return 0;
}

int pe_menu_register_action(const char *sub, const char *label,
                            const char *description, PeMenuAction action) {
    return psx_mod_menu_add_action(pe_page(sub), label, description,
                                   pe_wrap_action(sub, label, action, 0)) ? 0 : -1;
}

int pe_menu_register_choice(const char *sub, const char *label,
                            const char *description, int *index,
                            const char *const *names, unsigned count) {
    if (!psx_mod_menu_add_choice(pe_page(sub), label, description, index,
                                 names, count)) return -1;
    pe_watch(sub, label, index, names, count);
    return 0;
}

int pe_menu_register_live_int(const char *sub, const char *label,
                              const char *description, int minimum,
                              int maximum, int step, PeMenuGet get,
                              PeMenuSet set) {
    return psx_mod_menu_add_live_int(pe_page(sub), label, description, minimum,
                                     maximum, step, get,
                                     pe_wrap_setter(sub, label, get, set, 0, 0)) ? 0 : -1;
}

int pe_menu_register_live_choice(const char *sub, const char *label,
                                 const char *description,
                                 const char *const *names, unsigned count,
                                 PeMenuGet get, PeMenuSet set) {
    return psx_mod_menu_add_live_choice(pe_page(sub), label, description,
                                        names, count, get,
                                        pe_wrap_setter(sub, label, get, set, names, count)) ? 0 : -1;
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
                                 (PSXModMenuCell)cell_fn,
                                 pe_wrap_action(sub, label, on_accept, 1), on_move)
        ? 0 : -1;
}

static void pe_mod_menu_vblank(void) {
    if (!psx_mod_game_started()) return;
    pe_watch_tick();
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
 * it walks. Menus and battle get the original mapping back.
 *
 * ANALOG: in analog mode the game drops the mapping's run bit and sets it only
 * when one stick axis is past full tilt (< 0x14 or >= 0xE7, its pad read
 * Field_HandleStateTransition 0x8003EB04); directions start at < 0x5A /
 * >= 0xA1. A round stick gate never gets an axis that far on a diagonal, so
 * sweeping through a turn dropped Aya to a walk (user report 2026-10-13). At
 * that function's entry, in free field movement, the stick bytes it is about to
 * read are reshaped (the pad buffer is refilled every frame), both axes scaled
 * together so the angle the game steers by (ratan2 of the stick, decomp
 * Scene_UpdatePlayerEntity) is unchanged: to run, out until the larger axis
 * reaches its edge; to walk, in until neither axis is at full tilt. Run =
 * Always run and not holding the run button, or, with Always run off, the
 * stick at least ~80% out in any direction (round, not per axis). Cross
 * always walks: the game's own rule is that pressing Cross drops the run bit
 * (so examining stops her), but in analog mode it rebuilds the run bit from
 * the stick after that rule, so a full-tilt stick kept her running through
 * the press and phones / objects didn't answer (user report 2026-10-13).
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
#define PE_FN_FIELD_PAD       0x8003EB04u   /* Field_HandleStateTransition: builds the field input */
#define PE_STICK_CENTER       0x80
#define PE_STICK_FULL_R2      (100 * 100)   /* ~80% of a full push, any direction */
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
 * (+0x1C) is raised to match -- but only while her heading holds steady. While
 * the game turns her (the opening turn toward the spot, the turn-in at the
 * end, a turn back after overshooting) she walks at the game's own pace and
 * her animation goes back to normal: pushing along a turning heading made her
 * orbit the spot at 3x, and stopping for good at the first turn left only the
 * animation sped up (user reports 2026-10-13). */
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
static int s_entry_choice = 0;   /* vanilla: the push still looks off at 3x (2026-10-13), revisit */

static void pe_entry_walk_tick(uint32_t player) {
    static int s_walking, s_turning = 1;
    static int32_t s_prev_mx, s_prev_mz;     /* the last heading that differed */
    static uint32_t s_our_step;
    const uint32_t factor = s_entry_factor[(unsigned)s_entry_choice < PE_ENTRY_CHOICES ? (unsigned)s_entry_choice : 0];
    const int walk = (psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 0x2u) &&
                     psx_mod_read_word(PE_BATTLE_PHASE_ADDR) == 7 &&
                     psx_mod_read_byte(player + PE_ENT_ACTION_MODE) == PE_ENTRY_ACTION_MODE;
    const uint32_t step_addr = player + PE_ENT_ANIM_STEP;
    const int32_t mx = walk ? (int32_t)psx_mod_read_word(player + PE_ENT_MOTION_X) : 0;
    const int32_t mz = walk ? (int32_t)psx_mod_read_word(player + PE_ENT_MOTION_Z) : 0;
    if (!walk || factor == 0x10000u) {
        s_walking = 0;
    } else if (!s_walking) {
        s_walking = 1;
        s_turning = 1;                       /* wait for a steady heading */
        s_prev_mx = mx; s_prev_mz = mz;
    } else if (mx != s_prev_mx || mz != s_prev_mz) {
        /* The heading changed this logic frame: steady if within ~4 degrees
         * (cos^2 >= 0.995) and not reversed. */
        const int64_t ax = mx >> 8, az = mz >> 8, bx = s_prev_mx >> 8, bz = s_prev_mz >> 8;
        const int64_t dot = ax * bx + az * bz;
        s_turning = dot <= 0 || dot * dot * 1000 < (ax * ax + az * az) * (bx * bx + bz * bz) * 995;
        s_prev_mx = mx; s_prev_mz = mz;
    }
    const int push = s_walking && !s_turning && (mx || mz);

    const uint32_t step = psx_mod_read_word(step_addr);
    if (!push) {                             /* her own pace and animation (also once the walk ends) */
        if (s_our_step && step == s_our_step) psx_mod_write_word(step_addr, PE_ANIM_STEP_STOCK);
        s_our_step = 0;
        return;
    }
    const int64_t extra = (int64_t)factor - 0x10000;          /* 16.16 */
    const int32_t dx = (int32_t)(((int64_t)mx * extra) >> 17);  /* half per 60 Hz tick */
    const int32_t dz = (int32_t)(((int64_t)mz * extra) >> 17);
    psx_mod_write_word(player + PE_ENT_POS_X, (uint32_t)((int32_t)psx_mod_read_word(player + PE_ENT_POS_X) + dx));
    psx_mod_write_word(player + PE_ENT_POS_Z, (uint32_t)((int32_t)psx_mod_read_word(player + PE_ENT_POS_Z) + dz));
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

/* The raw pad as the game's internal button bits (Circle/Cross swapped, as in
 * the game's own pad read) and the left stick. The buffer is all zero before
 * the game sets it up, which would read as every button held, so a valid
 * header is required. */
typedef struct { uint32_t bits; int analog, x, y, stick; } PePad;

static PePad pe_read_pad(void) {
    PePad p = { 0, 0, PE_STICK_CENTER, PE_STICK_CENTER, 0 };
    const uint32_t pad = psx_mod_read_word(PE_PAD_ADDR);
    const uint32_t id = (pad >> 8) & 0xFFu;
    if ((pad & 0xFFu) != 0 || id == 0 || id == 0xFFu) return p;
    const uint32_t inv = ~(pad >> 16) & 0xFFFFu;
    p.bits = inv & 0x9FFFu;
    if (inv & 0x2000u) p.bits |= 0x4000u;
    if (inv & 0x4000u) p.bits |= 0x2000u;
    if ((pad & 0xF000u) == 0x7000u) {
        p.analog = 1;
        p.x = psx_mod_read_byte(PE_PAD_STICK_X);
        p.y = psx_mod_read_byte(PE_PAD_STICK_Y);
        p.stick = p.x < 0x5A || p.x >= 0xA1 || p.y < 0x5A || p.y >= 0xA1;
    }
    return p;
}

static int pe_in_free_field(void) {
    /* No battle (block 2), no menu (block 4, state 0x9000, set by
     * Entity_FrameUpdate when the item menu opens) and none of the states the
     * game itself requires clear before opening that menu (state 0x3400, block
     * 0x2000); 0x200/0x400 also gate the pad read for menus. */
    return !(psx_mod_read_word(PE_BATTLE_FLAGS_ADDR) & 0x2006u) &&
           !(psx_mod_read_word(PE_GAME_STATE_FLAGS) & 0xB600u);
}

/* Should a stick push run? (Always run: unless the run button is held.)
 * Never while Cross (internal 0x2000, examine / confirm) is held. */
#define PE_PAD_CROSS 0x2000u
static int pe_stick_runs(const PePad *p) {
    if (p->bits & PE_PAD_CROSS) return 0;
    if (s_always_run) return !(p->bits & s_run_mask);
    const int dx = p->x - PE_STICK_CENTER, dy = p->y - PE_STICK_CENTER;
    return dx * dx + dy * dy >= PE_STICK_FULL_R2;
}

/* Field_HandleStateTransition entry: reshape the stick it is about to read. */
static void pe_on_field_pad(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!psx_mod_game_started() || !pe_in_free_field()) return;
    const PePad p = pe_read_pad();
    if (!p.analog || !p.stick) return;
    /* Scale both axes by the same factor: the larger reaches `reach` (127:
     * past full tilt both ways; 100: inside it, still past the direction
     * thresholds of 0x5A / 0xA1), and the angle stays the same. */
    const int dx = p.x - PE_STICK_CENTER, dy = p.y - PE_STICK_CENTER;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int big = ax > ay ? ax : ay;
    const int run = pe_stick_runs(&p);
    const int full = p.x < 0x14 || p.x >= 0xE7 || p.y < 0x14 || p.y >= 0xE7;
    if (big <= 0 || run == full) return;            /* already what we want */
    const int reach = run ? 127 : 100;
    int x = PE_STICK_CENTER + (dx * reach + (dx < 0 ? -big / 2 : big / 2)) / big;
    int y = PE_STICK_CENTER + (dy * reach + (dy < 0 ? -big / 2 : big / 2)) / big;
    if (x < 0) x = 0; else if (x > 0xFF) x = 0xFF;
    if (y < 0) y = 0; else if (y > 0xFF) y = 0xFF;
    if (x != p.x) psx_mod_write_byte(PE_PAD_STICK_X, (uint8_t)x);
    if (y != p.y) psx_mod_write_byte(PE_PAD_STICK_Y, (uint8_t)y);
}

static void pe_move_tick(void) {
    const uint32_t old = psx_mod_read_word(PE_OLD_MOVE_ADDR);
    if (old == 0x3C040008u || old == 0x3C040014u)
        psx_mod_write_code_word(PE_OLD_MOVE_ADDR, PE_OLD_MOVE_RETAIL);

    const PePad p = pe_read_pad();
    const uint32_t bits = p.bits;
    const int dpad = (bits & PE_RUN_MAP_DPAD) != 0;
    const int stick = p.stick;
    const int in_field = pe_in_free_field();

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
    else      running = pe_stick_runs(&p);   /* what pe_on_field_pad makes the game do */

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
void pe_key_items_register(void);         /* pe_key_items.c */
void pe_bag_sort_register(void);          /* pe_bag_sort.c */
void pe_field_wayne_register(void);       /* pe_field_wayne.c */
void pe_mod_effects_register(void);       /* pe_mod_effects.c */
void pe_pe_balance_register(void);        /* pe_pe_balance.c */
void pe_loot_hooks_register(void);      /* pe_loot.c, after the loot logger */
void pe_stats_menu_register(void);      /* pe_stats_menu.c */
void pe_item_editor_register(void);     /* pe_item_editor.c */
void pe_debug_menu_register(void);      /* pe_debug_menu.c */
void pe_loot_logger_menu_register(void); /* pe_loot_logger.c (Debug page) */
void pe_custom_register(void);           /* pe_custom.c, after the loot hooks */
void pe_collection_register(void);       /* pe_collection.c: Bestiary + Wardrobe */

PSX_MOD_CONSTRUCTOR(pe_register_mod_menu_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.mod-menu", pe_mod_menu_vblank);
    (void)psx_mod_register_function_entry_plugin("pe.analog-run", PE_FN_FIELD_PAD, pe_on_field_pad);
    (void)pe_menu_register_toggle(0, "Always run",
        "Hold the run button to walk (d-pad and stick).",
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
    pe_key_items_register();
    pe_bag_sort_register();
    pe_field_wayne_register();
    pe_mod_effects_register();
    pe_pe_balance_register();
    pe_collection_register();
    pe_stats_menu_register();
    pe_item_editor_register();
    pe_debug_menu_register();
    pe_loot_logger_menu_register();
    pe_loot_hooks_register();
    pe_custom_register();
}
