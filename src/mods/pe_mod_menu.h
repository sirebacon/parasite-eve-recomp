/*
 * Registry for the in-game mod menu (pe_mod_menu.c).
 *
 * Any plugin in src/mods/ can add a toggle by calling pe_menu_register_toggle()
 * from its PSX_MOD_CONSTRUCTOR. The registry storage is zero-initialised static
 * data, so registration order across constructors does not matter (display
 * order follows registration order, which is unspecified across files).
 *
 *   label      Short uppercase text, at most ~22 characters; the menu font only
 *              has A-Z, 0-9, space and  > - : ( ) % . +
 *   flag       Read by the menu to show ON/OFF, and written when the player
 *              toggles it. Owned by the registering plugin.
 *   on_change  Optional. Called once, from the menu's VBlank, after a toggle.
 *   per_frame  Optional. Called from the menu's VBlank every frame while the
 *              game is running, whether or not the menu is open. It must decide
 *              for itself what to do from *flag (this is how a toggle drives
 *              behaviour even when the plugin's own launcher feature is off).
 *
 * Returns 0 on success, -1 if the registry is full.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PeMenuChanged)(int enabled);
typedef void (*PeMenuPerFrame)(void);

int pe_menu_register_toggle(const char *label, int *flag,
                            PeMenuChanged on_change, PeMenuPerFrame per_frame);

#ifdef __cplusplus
}
#endif
