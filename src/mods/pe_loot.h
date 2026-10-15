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
int pe_loot_wayne_waiting(void);        /* he holds 300+ but can't build before Day 4 */
int pe_loot_handin_blocked(void);       /* a hand-in needs one free bag slot */

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
const char *pe_shop_label(int i);           /* ASCII name for a row with no item (an economy mod), or 0 */
int pe_shop_buy_index(int i, char *msg, size_t msg_size);
int pe_rebuild_count(void);
uint8_t pe_rebuild_item(int i);
uint32_t pe_rebuild_price_index(int i);
unsigned pe_rebuild_bought_slots(int i);   /* slots bought with Mod Permits */
int pe_rebuild_index(int i, char *msg, size_t msg_size);

/* The Mod Catalog (pe_loot_catalog.c) for the in-game menu (pe_catalog_menu.c).
 * which: 0 = the equipped gun, 1 = the equipped armor. */
enum {
    PE_CAT_OK, PE_CAT_NO_GAME, PE_CAT_OFF, PE_CAT_NO_ITEM, PE_CAT_NO_SLOTS, PE_CAT_LOCKED,
    PE_CAT_ALREADY, PE_CAT_BAG, PE_CAT_JUNK, PE_CAT_TOOL, PE_CAT_EMPTY,
};
uint32_t pe_cat_record(int which);          /* equipment record, 0 if none */
int pe_cat_slots(int which);
int pe_cat_slot_effect(int which, int slot); /* 0 = empty */
int pe_cat_board_count(int which);
int pe_cat_board_effect(int which, int i);
int pe_cat_unlocked(int which, int e);
uint32_t pe_cat_price(int which, int e);
int pe_cat_needs_tool(int which, int e);
int pe_cat_is_bag_mod(int which, int e);
int pe_cat_install(int which, int slot, int e, int dry_run);  /* PE_CAT_* */
int pe_cat_remove(int which, int slot, int dry_run);
/* pe_key_items.c: the key item pocket */
int pe_keys_enabled(void);
int pe_keys_count(void);
uint16_t pe_keys_item(int i);
int pe_keys_take(uint16_t item);   /* found-list key with the bag full: straight into the pocket */
int pe_cat_replaces(int which, int slot, int e, int *out, int max);  /* effects an install takes out */
/* pe_bag_sort.c: Sort in Use Item, the bag kept sorted */
int pe_bag_sort_hide_row(void);
void pe_bag_sort_screen_seen(void);
/* pe_field_wayne.c: Warner offers Wayne's services */
uint32_t pe_field_wayne_text(void);
int pe_field_wayne_box_ours(void);

#ifdef __cplusplus
}
#endif
