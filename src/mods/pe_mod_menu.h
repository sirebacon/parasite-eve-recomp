/*
 * Registry for the in-game mod menu.
 *
 * The menu itself is the runtime's pause menu (F1): it freezes the game at a VBlank boundary, draws over FMVs, and
 * keeps its input away from the game. This file is only the PE-side shim that
 * hands plugin rows to it (psx_mod_menu_add_*, runtime/include/mod_plugins.h)
 * and runs the per-frame ticks that make a toggle actually do something.
 *
 *   label       Row text.
 *   description  One line shown when the row is selected.
 *   flag         Read by the menu to show ON/OFF and written when it is
 *                toggled. Owned by the registering plugin; must be static.
 *   per_frame    Optional. Called from a VBlank plugin every frame the game is
 *                running, whether or not the menu is open. It must decide for
 *                itself what to do from *flag (this is how a toggle drives
 *                behaviour even when the plugin's own launcher feature is off).
 *   action       Called once, with the game paused, when the row is chosen.
 *                The psx_mod read and write functions are safe inside it.
 *
 * Both return 0 on success, -1 if the registry is full or an argument is bad.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PeMenuPerFrame)(void);
typedef void (*PeMenuAction)(void);

int pe_menu_register_toggle(const char *label, const char *description,
                            int *flag, PeMenuPerFrame per_frame);

int pe_menu_register_action(const char *label, const char *description,
                            PeMenuAction action);

#ifdef __cplusplus
}
#endif
