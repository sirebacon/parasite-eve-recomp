/* Shared Junk counter of the loot overhaul (pe_loot.c), for the Mod Catalog,
 * Rebuild and shop (pe_loot_catalog.c, pe_loot_shop.c). */
#pragma once
#include <stddef.h>
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
/* Dismantling the gun/armor in equipment record `rec` (0 if it isn't one). The
 * reward unlocks, adds a Rebuild entry and pays Junk; the caller frees it. */
uint32_t pe_catalog_dismantle_preview(uint32_t rec);
uint32_t pe_catalog_dismantle_reward(uint32_t rec, int *unlocks_out);

/* Requisition goods (pe_loot_shop.c) and the Rebuild list (pe_loot_catalog.c),
 * by index, for Wayne's in-game Workshop and the F1 pages. The *_index
 * actions write what happened to msg and return 1 on success. */
int pe_shop_count(void);
uint16_t pe_shop_item(int i);
uint32_t pe_shop_price_index(int i);
const char *pe_shop_blocked_index(int i);   /* why it can't be bought now, or 0 */
int pe_shop_buy_index(int i, char *msg, size_t msg_size);
int pe_rebuild_count(void);
uint8_t pe_rebuild_item(int i);
uint32_t pe_rebuild_price_index(int i);
int pe_rebuild_index(int i, char *msg, size_t msg_size);

#ifdef __cplusplus
}
#endif
