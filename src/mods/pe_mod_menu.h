/*
 * Registry for the in-game mod menu.
 *
 * The menu itself is the runtime's pause menu (F1): it freezes the game at a
 * VBlank boundary, draws over FMVs, and keeps its input away from the game.
 * Every PE row lives under our own entry, beside other people's mods:
 *
 *     Mods > PE Enhancements > (rows registered with sub = 0)
 *     Mods > PE Enhancements > Item Editor > (rows with sub = "Item Editor")
 *
 * This file is only the PE-side shim that hands rows to the runtime
 * (psx_mod_menu_add_*, runtime/include/mod_plugins.h) and runs the per-frame
 * ticks that make a toggle actually do something.
 *
 *   sub          Sub-page below "PE Enhancements", or 0 for the page itself.
 *                May itself contain '/' for deeper pages.
 *   label        Row text.
 *   description  One line shown when the row is selected (keep it under ~38
 *                characters, or the menu footer cuts it off).
 *   flag         Read by the menu to show ON/OFF and written when it is
 *                toggled. Owned by the registering plugin; must be static.
 *   per_frame    Optional. Called from a VBlank plugin every frame the game is
 *                running, whether or not the menu is open. It must decide for
 *                itself what to do from *flag (this is how a toggle drives
 *                behaviour even when the plugin's own launcher feature is off).
 *   action       Called once, with the game paused, when the row is chosen.
 *                The psx_mod read and write functions are safe inside it.
 *
 * All return 0 on success, -1 if the registry is full or an argument is bad.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PE_MENU_PAGE "PE Enhancements"

typedef void (*PeMenuPerFrame)(void);
typedef void (*PeMenuAction)(void);

int pe_menu_register_toggle(const char *sub, const char *label,
                            const char *description, int *flag,
                            PeMenuPerFrame per_frame);

int pe_menu_register_action(const char *sub, const char *label,
                            const char *description, PeMenuAction action);

/* Pick one of `count` names; *index is the 0-based selection. `names` must be
 * static: the menu keeps the pointer. */
int pe_menu_register_choice(const char *sub, const char *label,
                            const char *description, int *index,
                            const char *const *names, unsigned count);

/* A number row (left/right steps by `step`, clamped to [minimum, maximum]). */
int pe_menu_register_int(const char *sub, const char *label,
                         const char *description, int *value,
                         int minimum, int maximum, int step);

/* Live rows: get() is called whenever the row is shown and set(value) when
 * the player changes it, so the row can read/write game memory directly and
 * changes apply immediately (the game is paused while the menu is open).
 * set() returns non-zero if it accepted the value. */
typedef int (*PeMenuGet)(void);
typedef int (*PeMenuSet)(int value);
int pe_menu_register_live_int(const char *sub, const char *label,
                              const char *description, int minimum,
                              int maximum, int step, PeMenuGet get,
                              PeMenuSet set);
int pe_menu_register_live_choice(const char *sub, const char *label,
                                 const char *description,
                                 const char *const *names, unsigned count,
                                 PeMenuGet get, PeMenuSet set);

/* Run fn from the menu's VBlank plugin every frame the game runs (menu
 * closed). Used to mirror game state into rows and apply edits after F1. */
int pe_menu_register_tick(PeMenuPerFrame fn);

/* A choice whose list depends on other rows (re-read after any row changes;
 * *index resets to 0 when the list changes). count_fn() must not exceed
 * max_count; name_fn must return static strings. */
typedef unsigned (*PeMenuChoiceCount)(void);
typedef const char *(*PeMenuChoiceName)(unsigned index);
int pe_menu_register_dynamic_choice(const char *sub, const char *label,
                                    const char *description, int *index,
                                    unsigned max_count,
                                    PeMenuChoiceCount count_fn,
                                    PeMenuChoiceName name_fn);

#ifdef __cplusplus
}
#endif
