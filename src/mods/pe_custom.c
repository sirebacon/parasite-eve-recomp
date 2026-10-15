/*
 * Loot overhaul, phase 6: the custom items and mods in the game's own menus
 * (bridge findings pe-custom-items-design-2026-10-11.md C1-C7, C12).
 *
 *   - Names. The game's text tables are read in place (directory at
 *     0x800A8028 + word 0x800A802C; table +0x08 item names, +0x0C item
 *     descriptions, +0x10 mod descriptions: guns = entry effect - 1, armor =
 *     entry effect + 19). A Draw_PrintRawText entry hook swaps a pointer to
 *     one of these strings for our own text in mod memory:
 *       ITEM24 (0x18) -> Smoke Grenade, ITEM25 (0x19) -> Panacea (was the Lucky Charm until
 *       2026-10-15), with
 *       descriptions; the "dummy" mod texts of armor effects 17-20 -> the
 *       economy mods' PE-style descriptions (C12).
 *     Text is the game's glyph code: 'A'-'Z' 0x10-0x29, 'a'-'z' 0x30-0x49,
 *     '0'-'9' 0x00-0x09, space 0x0F, end 0xFF (read from the live tables).
 *   - Use. Item flags (+0x05 of the base record: bit 0 field use, bit 1
 *     battle use) are set every half second: the Smoke Grenade is battle
 *     only, and greyed out in a battle it can't escape (the game's own way to
 *     say an item can't be used); Panacea is used in battle and from the menu.
 *   - Panacea (Items & Parasite Energy doc): Combine Medicine 4 + Full Cure (pe_native_menu.c).
 *     Battle_ApplyDamage(0x19) cures every ailment here (as Full Cure: poison, confusion,
 *     darkness, stiffness, unless immune, and state 0x1000) and is then handed to the game as
 *     Medicine 4 (a0 = 9): +400 HP, the medicine effect, the HP clamp. Its icon is Medicine 4's.
 *   - Smoke Grenade (C6). A battle item resolves through Battle_ApplyDamage
 *     (0x80023E14, a0 = item id), right after Battle_ApplyPlayerHit sets the
 *     player's idle action mode -- the same thing a successful escape roll
 *     does before it sets the battle phase to 4 (escape). So on the grenade,
 *     the phase (0x8009D28C) is set to 4: a sure escape. Enemies flagged
 *     "can't escape" (statusFlags2 & 0x40000, as Battle_RollEscapeChance
 *     checks) stop it; if one is used there anyway, it goes back in the bag.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"

#define PE_FN_DRAW_PRINT_RAW_TEXT 0x8005F27Cu
#define PE_FN_BATTLE_APPLY_DAMAGE 0x80023E14u

#define PE_MAX_HP_ADDR          0x800C0E06u
#define PE_GAME_BLOCK_ADDR      0x8009D1A0u
#define PE_BATTLE_PHASE_ADDR    0x8009D28Cu   /* D_8009D28C: 4 = escape */
#define PE_ENEMY_SLOT_BASE      0x800A5D58u   /* 7 x { u8 active; pad; EnemyCombatant } */
#define PE_ENEMY_SLOT_STRIDE    0xDCu
#define PE_ENEMY_SLOTS          7
#define EN_HP_ALIVE             (4u + 0x10u)
#define EN_STATUS_FLAGS2        (4u + 0xCCu)
#define PE_AYA_SLOTS_ADDR       0x800C0E48u
#define PE_SLOT_COUNT_ADDR      0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR     0x8009D018u

#define PE_STR_ORIGIN           0x800A8028u
#define PE_STR_DIRECTORY_OFFSET 0x800A802Cu
#define PE_ITEM_TABLE_BASE_ADDR 0x800A8034u
#define PE_ITEM_TABLE_END_ADDR  0x800A8038u

#define PE_ITEM_SMOKE   0x18u
#define PE_ITEM_PANACEA 0x19u                 /* ITEM25 */
#define PE_ITEM_MEDICINE_4 0x09u
#define PE_PLAYER_ENTITY   0x8009D254u
#define PE_FIELD_MOVE_FLAGS 0x8009D2E8u        /* & 0x10: confused movement */
int g_pe_panacea = 1;                         /* F1 > Items & Parasite Energy */

/* ---- text ------------------------------------------------------------------- */
typedef struct {
    uint32_t table;                           /* directory offset of the table */
    unsigned entry;
    const char *text;
    uint32_t guest;                           /* our copy, in mod memory */
    int economy_only;                         /* only while the Junk economy is on */
} PeRelabel;

static PeRelabel s_relabels[] = {
    { 0x08, PE_ITEM_SMOKE - 1, "Smoke Grenade", 0 },
    { 0x08, PE_ITEM_PANACEA - 1, "Panacea", 0 },
    { 0x0C, PE_ITEM_SMOKE - 1, "Sure Escape from Battle", 0 },
    { 0x0C, PE_ITEM_PANACEA - 1, "Restores 400 HP and Cures All Status", 0 },
    { 0x0C, 0x0B - 1, "Worth 75 Junk  a Quarter of a Wayne Gun", 0, 1 },   /* pe_loot.c bundles */
    { 0x0C, 0x0C - 1, "Worth 150 Junk  Half of a Wayne Gun", 0, 1 },
    { 0x10, PE_FX_BARGAIN + 19, "Lowers shop prices", 0 },
    { 0x10, PE_FX_SCAVENGER + 19, "Increases Junk found", 0 },
    { 0x10, PE_FX_LUCKY_FIND + 19, "Finds rare items more often", 0 },
    { 0x10, PE_FX_SALVAGER + 19, "More Junk from dismantling", 0 },
};
#define PE_RELABELS ((int)(sizeof s_relabels / sizeof s_relabels[0]))

static uint32_t s_source[PE_RELABELS];       /* the game's string addresses */

static int pe_ram_pointer(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static uint8_t pe_glyph(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x10 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x30 + (c - 'a'));
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    return 0x0F;                              /* space (and anything else) */
}

static uint32_t pe_text_entry(uint32_t table_off, unsigned index) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory)) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + table_off);
    if (!pe_ram_pointer(table) || index >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + index * 2u);
}

/* Look the source strings up again (cheap; tables can be reloaded). */
static void pe_refresh_sources(void) {
    for (int i = 0; i < PE_RELABELS; ++i)
        s_source[i] = pe_text_entry(s_relabels[i].table, s_relabels[i].entry);
}

static uint32_t pe_guest_text(PeRelabel *r) {
    if (r->guest) return r->guest;
    const size_t n = strlen(r->text);
    const uint32_t g = psx_mod_alloc_guest_memory((uint32_t)n + 1u, 4);
    if (!g) return 0;
    for (size_t i = 0; i < n; ++i) psx_mod_write_byte(g + (uint32_t)i, pe_glyph(r->text[i]));
    psx_mod_write_byte(g + (uint32_t)n, 0xFFu);
    return r->guest = g;
}

/* Registered after every other Draw_PrintRawText plugin, so the rare-drop
 * color still compares the game's own pointer. */
static void pe_on_print_raw_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t text = cpu->gpr[4];
    if (!pe_ram_pointer(text)) return;
    for (int i = 0; i < PE_RELABELS; ++i) {
        if (!s_source[i] || s_source[i] != text) continue;
        if (s_relabels[i].economy_only && !pe_loot_enabled()) return;
        const uint32_t g = pe_guest_text(&s_relabels[i]);
        if (g) cpu->gpr[4] = g;
        return;
    }
}

/* ---- item flags ------------------------------------------------------------- */
static uint32_t pe_base_record(uint8_t id) {
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE_ADDR);
    const uint32_t end = psx_mod_read_word(PE_ITEM_TABLE_END_ADDR);
    const uint32_t index = (uint32_t)id - 1u;
    if (end <= base || index >= ((end - base) >> 5)) return 0;
    return PE_STR_ORIGIN + base + index * 0x20u;
}

static void pe_set_item_flags(uint8_t id, uint8_t use) {
    const uint32_t rec = pe_base_record(id);
    if (!rec) return;
    const uint8_t f = (uint8_t)psx_mod_read_byte(rec + 0x05u);
    if ((f & 3u) != use) psx_mod_write_byte(rec + 0x05u, (uint8_t)((f & ~3u) | use));
}

static int pe_escape_blocked(void);

static void pe_custom_tick(void) {
    static unsigned frames;
    if (++frames % 30u) return;
    pe_refresh_sources();
    if (!pe_loaded()) return;
    const int battle = (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2u) != 0;
    pe_set_item_flags(PE_ITEM_SMOKE, battle && pe_escape_blocked() ? 0x0u : 0x2u);   /* battle only */
    pe_set_item_flags(PE_ITEM_PANACEA, g_pe_panacea ? 0x3u : 0x0u);   /* field and battle */
    {                                         /* Medicine 4's icon */
        const uint32_t p = pe_base_record(PE_ITEM_PANACEA), m = pe_base_record(PE_ITEM_MEDICINE_4);
        if (p && m && psx_mod_read_byte(p) != psx_mod_read_byte(m)) psx_mod_write_byte(p, psx_mod_read_byte(m));
    }
}

/* ---- Smoke Grenade ---------------------------------------------------------- */
static int pe_escape_blocked(void) {
    for (int s = 0; s < PE_ENEMY_SLOTS; ++s) {
        const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)s * PE_ENEMY_SLOT_STRIDE;
        if (!psx_mod_read_byte(slot) || (int32_t)psx_mod_read_word(slot + EN_HP_ALIVE) <= 0) continue;
        if (psx_mod_read_word(slot + EN_STATUS_FLAGS2) & 0x40000u) return 1;
    }
    return 0;
}

/* Panacea: every ailment Full Cure cures, then Medicine 4's heal. */
static void pe_panacea(struct CPUState *cpu) {
    const uint32_t ent = psx_mod_read_word(PE_PLAYER_ENTITY);
    const uint32_t core = pe_ram_pointer(ent) ? psx_mod_read_word(ent) : 0;
    if (pe_ram_pointer(core)) {
        uint32_t s = psx_mod_read_word(core + 0x4Cu);
        static const uint32_t masks[4] = { 0x03u, 0x0Cu, 0x30u, 0xC0u };   /* poison, confusion, darkness, stiffness */
        for (int i = 0; i < 4; ++i)
            if ((s & masks[i]) != masks[i]) s &= ~masks[i];                 /* all bits set = immune: kept */
        s &= ~0x1000u;
        psx_mod_write_word(core + 0x4Cu, s);
        psx_mod_write_byte(PE_FIELD_MOVE_FLAGS, (uint8_t)(psx_mod_read_byte(PE_FIELD_MOVE_FLAGS) & ~0x10u));
    }
    cpu->gpr[4] = PE_ITEM_MEDICINE_4;
}

static void pe_on_apply_damage(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[4] == PE_ITEM_PANACEA) { pe_panacea(cpu); return; }
    if (cpu->gpr[4] != PE_ITEM_SMOKE || !(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2u)) return;
    if (pe_escape_blocked()) {
        uint32_t limit = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
        if (limit > 50u) limit = 50u;
        for (uint32_t i = 0; i < limit; ++i)
            if (!psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u)) {
                psx_mod_write_half(PE_AYA_SLOTS_ADDR + i * 2u, PE_ITEM_SMOKE);
                break;
            }
        return;
    }
    psx_mod_write_word(PE_BATTLE_PHASE_ADDR, 4u);
}

/* Called last from pe_mod_menu.c (after pe_loot_hooks_register). */
void pe_custom_register(void) {
    (void)pe_menu_register_tick(pe_custom_tick);
    (void)psx_mod_register_function_entry_plugin("pe.custom.text",
        PE_FN_DRAW_PRINT_RAW_TEXT, pe_on_print_raw_text);
    (void)psx_mod_register_function_entry_plugin("pe.custom.smoke",
        PE_FN_BATTLE_APPLY_DAMAGE, pe_on_apply_damage);
}
