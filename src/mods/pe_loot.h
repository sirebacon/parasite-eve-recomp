/* Shared Junk counter of the loot overhaul (pe_loot.c), for the Mod Catalog,
 * Rebuild and shop (pe_loot_catalog.c, pe_loot_shop.c). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int pe_loot_enabled(void);              /* "Junk economy" switch */
uint32_t pe_loot_junk(void);
int pe_loot_spend_junk(uint32_t n);     /* 0 (and nothing spent) if short */
void pe_loot_give_junk(uint32_t n);

/* Economy armor mods (custom items doc C1-C5) in the game's unused armor
 * effect numbers: 17-20 show the "dummy" mod texts, which pe_custom.c
 * relabels; the game's armor code ignores 18-20 and only sets an unread
 * flag for 17 (Inv_BuildArmorList). */
#define PE_FX_BARGAIN    17             /* prices -15% (C4) */
#define PE_FX_SCAVENGER  18             /* battle Junk +50% (C1) */
#define PE_FX_LUCKY_FIND 19             /* pity builds twice as fast (C2) */
#define PE_FX_SALVAGER   20             /* dismantle Junk +25% (C3) */
int pe_loot_armor_has(int effect);      /* on the equipped armor */
uint32_t pe_loot_price(uint32_t base);  /* after preset and Bargain; caps after */

/* pe_loot_catalog.c: armor effects in the Mod Catalog. */
int pe_catalog_armor_unlocked(int effect);
int pe_catalog_unlock_armor(int effect); /* 1 if it was new */

#ifdef __cplusplus
}
#endif
