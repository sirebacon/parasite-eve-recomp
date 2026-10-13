/*
 * Wayne junk refund: when Wayne's 300-Junk craft ("Leave it to Wayne") hands
 * out a Super Junk or Duper Junk instead of a gun, return the 300 Junk so the
 * player can try again. The player keeps the item: it restores 1/4 / 1/2 of
 * max Parasite Energy (decomp Battle_ApplyDamage), the guides' "worthless"
 * notwithstanding (user's decision, 2026-10-11).
 *
 * How Wayne crafts (live tests 2026-10-11, bridge findings
 * pe-loot-overhaul-design-2026-10-11.md section 6.5): his room script's
 * "Discard junk" adds 1 to his counter (script variable 0x800A78B0) per Junk
 * it takes; when a hand-in brings the counter to 300 or more on a full-menu
 * day, he crafts, the item lands in the bag, and the script then subtracts 300
 * (301 -> gun -> 1). So:
 *   1. a rising counter means a hand-in is under way: remember how many
 *      Super/Duper Junk Aya has (bag + Wayne's storage) and arm;
 *   2. a drop of 300 or more while armed is the craft: if a Super/Duper Junk
 *      appeared since step 1, add the 300 back. The counter is
 *      then >= 300 again and the next "Discard junk" of at least one Junk
 *      crafts again (a counter at 300 alone doesn't trigger him).
 * Leaving the room or loading a state disarms, so jumps in the counter from
 * loads never look like a craft.
 */
#include <stdint.h>
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

#define PE_WAYNE_COUNTER   0x800A78B0u  /* u16 script variable, g_EntityWorkBuffer + 0xC0 */
#define PE_ROOM_ADDR       0x8009D280u
#define PE_MAX_HP_ADDR     0x800C0E06u  /* 0 until a game is loaded */
#define PE_AYA_SLOTS_ADDR  0x800C0E48u
#define PE_AYA_SLOT_COUNT  50u
#define PE_STORAGE_ADDR    0x800C1EB8u  /* Wayne's storage */
#define PE_STORAGE_COUNT   100u
#define PE_SUPER_JUNK      0x0Bu
#define PE_DUPER_JUNK      0x0Cu
#define PE_CRAFT_COST      300u

static int s_enabled = 1;
static int s_valid, s_armed;
static unsigned s_prev_counter, s_prev_room, s_junk_before;

static int pe_is_bad_roll(unsigned id) {
    return id == PE_SUPER_JUNK || id == PE_DUPER_JUNK;
}

static unsigned pe_count_bad_rolls(void) {
    unsigned n = 0;
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        n += pe_is_bad_roll(psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u));
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i)
        n += pe_is_bad_roll(psx_mod_read_half(PE_STORAGE_ADDR + i * 2u));
    return n;
}

static void pe_wayne_refund_tick(void) {
    if (!s_enabled || psx_mod_read_half(PE_MAX_HP_ADDR) == 0) { s_valid = s_armed = 0; return; }
    unsigned counter = psx_mod_read_half(PE_WAYNE_COUNTER);
    const unsigned room = psx_mod_read_half(PE_ROOM_ADDR);
    if (!s_valid || room != s_prev_room) {
        s_valid = 1;
        s_armed = 0;
    } else if (counter > s_prev_counter) {              /* a hand-in step */
        if (!s_armed) s_junk_before = pe_count_bad_rolls();
        s_armed = 1;
    } else if (s_armed && s_prev_counter >= counter + PE_CRAFT_COST) {   /* the craft */
        s_armed = 0;
        if (pe_count_bad_rolls() > s_junk_before) {
            counter += PE_CRAFT_COST;
            psx_mod_write_half(PE_WAYNE_COUNTER, (uint16_t)counter);   /* his row shows it */
            pe_log("wayne_refund", "back=%u", PE_CRAFT_COST);
        }
    }
    s_prev_counter = counter;
    s_prev_room = room;
}

/* A loaded state's counter is unrelated to the one before it. */
static void pe_wayne_refund_restored(void) {
    s_valid = s_armed = 0;
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_wayne_refund_menu_register(void) {
    (void)pe_menu_register_toggle(0, "Wayne junk refund",
        "Junk roll refunds Wayne's 300 Junk.", &s_enabled, pe_wayne_refund_tick);
    (void)psx_mod_register_restore_callback(pe_wayne_refund_restored);
}
