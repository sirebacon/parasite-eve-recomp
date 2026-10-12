/* Shared Junk counter of the loot overhaul (pe_loot.c), for the Mod Catalog,
 * Rebuild and shop (pe_loot_catalog.c). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int pe_loot_enabled(void);              /* "Junk economy" switch */
uint32_t pe_loot_junk(void);
int pe_loot_spend_junk(uint32_t n);     /* 0 (and nothing spent) if short */
void pe_loot_give_junk(uint32_t n);

#ifdef __cplusplus
}
#endif
