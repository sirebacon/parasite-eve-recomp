/*
 * God Mode: keeps Aya's live in-battle HP topped up to her current max HP,
 * every VBlank, so damage never sticks.
 *
 * BACKGROUND: unlike the personal-inventory floor, this does NOT target the
 * save-struct mirror at 0x800C0E08 (AyaSaveState.current_hp) -- that field is
 * itself a derived cache re-synced FROM the live battle record, not the other
 * way around (see include/pe1/aya.h's own note: "synced from battle slot
 * +0x0C"). Writing the mirror would just get overwritten by the next sync and
 * do nothing to survive an attack.
 *
 * The actual value damage lands on is the live Combatant record:
 *   g_ActiveActor (0x8009D278) -> Combatant*
 *   Combatant + 0x0C = curHP  (u16, clamped to maxHP by Battle_ApplyDamage)
 *   Combatant + 0x1C = maxHP  (u16)
 * (include/pe1/battle.h, PE1_STATIC_ASSERT-confirmed offsets.)
 *
 * FIX: every VBlank, if g_ActiveActor resolves to a plausible KSEG0 pointer,
 * read maxHP and write it into curHP. A vblank hook re-runs continuously
 * rather than firing once, so this is savestate-safe by construction and
 * self-correcting the instant a battle (re)starts -- same idiom as
 * pe_inventory_floor.c, applied through one pointer indirection instead of a
 * fixed absolute address.
 *
 * NOT LIVE-VERIFIED: this session's synthetic-input testing can drive menu
 * navigation but not field movement (confirmed exhaustively elsewhere in this
 * project), so no automated test has actually reached a real battle to
 * confirm Aya survives a hit. The pointer-validity guard and offsets are
 * sourced directly from the decomp's own PE1_STATIC_ASSERT-checked struct
 * layout, not guessed, but treat this as needing a real playtest before
 * trusting it in a fight.
 */
#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_ACTIVE_ACTOR_PTR_ADDR 0x8009D278u
#define PE_COMBATANT_CUR_HP_OFFSET 0x0Cu
#define PE_COMBATANT_MAX_HP_OFFSET 0x1Cu

static int pe_is_plausible_kseg0_pointer(uint32_t addr) {
    /* PS1 KSEG0 RAM window; g_ActiveActor is null (or stale garbage) outside
     * battle, so this guard is load-bearing, not decorative -- dereferencing
     * an out-of-range value here would read/write outside guest RAM. */
    return addr >= 0x80000000u && addr < 0x80200000u;
}

/* Shared by this package's own VBlank plugin and by the in-game mod menu's
 * "GOD MODE" toggle, so the behaviour is the same however it is switched on. */
static void pe_god_mode_apply(void) {
    if (!psx_mod_game_started()) return;

    const uint32_t combatant = psx_mod_read_word(PE_ACTIVE_ACTOR_PTR_ADDR);
    if (!pe_is_plausible_kseg0_pointer(combatant)) return;

    const uint16_t max_hp = psx_mod_read_half(combatant + PE_COMBATANT_MAX_HP_OFFSET);
    /* 0 means no real combatant is resident at this address (pre-battle-init
     * garbage that happened to look like a pointer) -- leave it alone rather
     * than writing a fabricated "full HP" of zero. */
    if (max_hp == 0) return;

    const uint16_t cur_hp = psx_mod_read_half(combatant + PE_COMBATANT_CUR_HP_OFFSET);
    if (cur_hp != max_hp) {
        psx_mod_write_half(combatant + PE_COMBATANT_CUR_HP_OFFSET, max_hp);
    }
}

static void pe_god_mode_vblank(void) {
    pe_god_mode_apply();
}

/* In-game menu toggle. Off by default; the menu's VBlank calls the tick every
 * frame, so this works even when the launcher feature above is disabled. */
static int pe_menu_god_mode;

static void pe_god_mode_menu_tick(void) {
    if (pe_menu_god_mode) pe_god_mode_apply();
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_god_mode_menu_register(void) {
    (void)pe_menu_register_toggle(0, "God mode",
        "Keeps Aya's HP full during battle.",
        &pe_menu_god_mode, pe_god_mode_menu_tick);
}

PSX_MOD_CONSTRUCTOR(pe_register_god_mode_plugin) {
    (void)psx_mod_register_vblank_plugin("pe.god-mode", pe_god_mode_vblank);
}
