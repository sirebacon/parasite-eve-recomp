/*
 * Personal inventory floor (raises Aya's carried-item cap from its natural
 * per-level progression).
 *
 * BACKGROUND: g_AyaInventorySlotCount (RAM byte 0x800C0E0C) is NOT a static
 * constant anywhere in the shipped SLUS-00662 executable -- every byte of the
 * 99-entry per-level table that feeds it (found at a runtime-resolved address
 * built from a boot-time-computed base, not a fixed one) is zero in the
 * static disc image and only gets its real values (10 at level 1, growing to
 * 46 at level 99; confirmed by live capture 2026-09-13) from code that runs
 * at boot. That rules out patching the table as data (there's nothing to
 * patch) and rules out patching a single "set slot count" call site (the
 * value gets re-applied on every level-up from the same generated table, so
 * a one-shot fix regresses at the next level-up).
 *
 * FIX: rather than reverse-engineer the table generator, poll the live byte
 * every VBlank and raise it to the floor whenever the game (New Game init,
 * a level-up, or a savestate load) has just set it lower. A vblank plugin
 * re-registers itself with the runtime's ongoing frame loop, not a one-time
 * boot action, so this is savestate-safe by construction -- unlike the
 * activation-plugin mistake made earlier this session (see
 * pe.enhancement.faster-movement's retirement notes).
 *
 * Deliberately leaves the byte alone once the natural curve exceeds the
 * floor (~level 75+, see the 2026-09-13 capture), so late-game progression
 * beyond the floor is untouched.
 */
#include "mod_plugins.h"

#define PE_INV_SLOT_COUNT_ADDR 0x800C0E0Cu
#define PE_AYA_SLOTS_ADDR      0x800C0E48u  /* g_AyaInventoryItems */
#define PE_ACTIVE_LIST_PTR     0x8009D048u  /* g_InvItemPtr */
#define PE_ACTIVE_LIST_LIMIT   0x8009D050u  /* g_InvSlotLimit */
#define PE_BONUS_SLOTS_ADDR    0x8009D018u  /* g_InvBonusSlotCount */

/* The game caches the bag's length in g_InvSlotLimit whenever it selects Aya's
 * bag (Inv_GetAyaSlotLimit = slot count + armor bonus slots, max 50), and its
 * own capacity setter refreshes that cache. Raising the slot count from here
 * must do the same, or the game keeps the old length until it next re-selects
 * the bag: items in the new slots are then invisible, and a weapon equipped
 * from one builds an empty battle action (Aya's turn gauge never fills). */
static void pe_inventory_floor_sync_limit(void) {
    if (psx_mod_read_word(PE_ACTIVE_LIST_PTR) != PE_AYA_SLOTS_ADDR) return;
    uint32_t limit = psx_mod_read_byte(PE_INV_SLOT_COUNT_ADDR) +
                     psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    if (limit > 50u) limit = 50u;
    if (psx_mod_read_word(PE_ACTIVE_LIST_LIMIT) < limit)
        psx_mod_write_word(PE_ACTIVE_LIST_LIMIT, limit);
}

static int pe_inventory_floor_slots(void) {
    char text[16] = "";
    if (!psx_mod_option_value("pe.enhancement.personal-inventory-floor",
                              "personal-inventory-floor", "floor", text,
                              sizeof text) || !text[0]) {
        return 40;
    }
    int value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return 40;
        value = value * 10 + (*p - '0');
    }
    if (value <= 0 || value > 50) return 40;
    return value;
}

/* The floor while this package runs (the play log's settings row reads it).
 * Off by default since 2026-10-13: a 40-slot bag skewed a balance playthrough. */
int g_pe_inv_floor_active;

static void pe_inventory_floor_vblank(void) {
    if (!psx_mod_game_started()) return;
    g_pe_inv_floor_active = pe_inventory_floor_slots();

    const uint8_t current = psx_mod_read_byte(PE_INV_SLOT_COUNT_ADDR);
    /* 0 means no save/character state has initialized this byte yet
     * (pre-New-Game boot) -- leave it alone rather than forcing a floor
     * onto memory that isn't live character state at all. */
    if (current == 0) return;

    const int floor_slots = pe_inventory_floor_slots();
    if ((int)current < floor_slots) {
        psx_mod_write_byte(PE_INV_SLOT_COUNT_ADDR, (uint8_t)floor_slots);
    }
    pe_inventory_floor_sync_limit();
}

PSX_MOD_CONSTRUCTOR(pe_register_inventory_floor_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.inventory-floor",
                                         pe_inventory_floor_vblank);
}
