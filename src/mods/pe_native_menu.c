/*
 * Loot overhaul, phase 5: native menu actions in Aya's own item menu
 * (bridge findings pe-loot-overhaul-design-2026-10-11.md decision 25, custom
 * items doc C10; decision 44 for how they are offered).
 *
 * The game's item menu has a fixed action table, so instead of new rows the
 * existing Discard does the overhaul's work when it makes sense:
 *
 *   - Discard on a gun or armor dismantles it: the confirm dialog reads
 *     "<name> will be dismantled for N Junk", and "yes" unlocks its effects in
 *     the Mod Catalog, puts it on the Rebuild list and pays the Junk
 *     (pe_catalog_dismantle_reward); the game's own code then removes it.
 *   - Discard on Medicine 1-3 while the bag holds two of it combines them
 *     (C10): "<name> x2 will become <next>", and "yes" turns the other one
 *     into the next medicine while the game removes the selected one.
 *
 * Hooks: Util_AppendFFTerminatedBytes (0x80052C08) while the dialog label is
 * built (Menu_StepItemDetailPanel appends Str_LookupTable4(4) " will be
 * discarded" to the item name in g_MenuItemLabelBuffer, then sizes the dialog
 * to it), and Menu_OnItemDiscardConfirm (0x80045110, a1 = yes). The selected
 * item is g_MenuActiveItemList (0 Aya's bag, else the storage list) and
 * g_MenuActiveItemSlot. Both behaviours have a Workshop toggle, on by default.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"

#define PE_FN_APPEND_FF_TEXT     0x80052C08u   /* Util_AppendFFTerminatedBytes(dst, src) */
#define PE_FN_DISCARD_CONFIRM    0x80045110u   /* Menu_OnItemDiscardConfirm(dialog, yes) */

#define PE_MENU_ACTIVE_LIST      0x8009CF10u   /* g_MenuActiveItemList */
#define PE_MENU_ACTIVE_SLOT      0x8009CF04u   /* g_MenuActiveItemSlot */
#define PE_MENU_LABEL_BUFFER     0x800A1980u   /* g_MenuItemLabelBuffer */
#define PE_LIST_OVERRIDE_ADDR    0x8009D04Cu   /* g_InvActiveListOverride (storage) */
#define PE_AYA_SLOTS_ADDR        0x800C0E48u
#define PE_SLOT_COUNT_ADDR       0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR      0x8009D018u
#define PE_EQUIP_RECORDS_ADDR    0x800C0EACu
#define PE_MAX_HP_ADDR           0x800C0E06u
#define PE_STR_ORIGIN            0x800A8028u
#define PE_STR_DIRECTORY_OFFSET  0x800A802Cu

#define PE_ITEM_MEDICINE_1 0x06u
#define PE_ITEM_MEDICINE_3 0x08u

static int s_discard_dismantles = 1;     /* decision 25 via Discard (44) */
static int s_discard_combines = 1;       /* C10 via Discard (44) */
static uint32_t s_suffix;                /* our label suffix, mod memory */

static int pe_ram_pointer(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

static uint32_t pe_text_entry(uint32_t table_off, unsigned index) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory)) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + table_off);
    if (!pe_ram_pointer(table) || index >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + index * 2u);
}

static uint8_t pe_glyph(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x10 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x30 + (c - 'a'));
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    return 0x0F;
}

/* What the selected item is: 1 = gun/armor (rec set), 2 = Medicine 1-3 with
 * another of it in the bag (other = that slot), 0 = neither. */
typedef struct { int what; uint32_t rec; uint16_t id; int other; } PeSelected;

static PeSelected pe_selected(void) {
    PeSelected s = { 0, 0, 0, -1 };
    const int list = (int)psx_mod_read_word(PE_MENU_ACTIVE_LIST);
    const int slot = (int)psx_mod_read_word(PE_MENU_ACTIVE_SLOT);
    uint32_t base = PE_AYA_SLOTS_ADDR;
    if (list) {
        base = psx_mod_read_word(PE_LIST_OVERRIDE_ADDR);
        if (!pe_ram_pointer(base)) return s;
    } else if (slot < 0 || (uint32_t)slot >= pe_bag_limit()) {
        return s;
    }
    if (slot < 0 || slot >= 100) return s;
    const uint16_t v = psx_mod_read_half(base + (uint32_t)slot * 2u);
    if (v >= 256 && v < 384) {
        const uint32_t rec = PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * 0x20u;
        const int kind = psx_mod_read_byte(rec + 0x06u);
        if (kind >= 1 && kind <= 9 && s_discard_dismantles && pe_loot_enabled()) {
            s.what = 1; s.rec = rec;
        }
        return s;
    }
    if (!list && s_discard_combines && v >= PE_ITEM_MEDICINE_1 && v <= PE_ITEM_MEDICINE_3) {
        const uint32_t limit = pe_bag_limit();
        for (uint32_t i = 0; i < limit; ++i)
            if ((int)i != slot && psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == v) {
                s.what = 2; s.id = v; s.other = (int)i;
                break;
            }
    }
    return s;
}

/* Copy of a game item name (text table +0x08) into out, glyph codes. */
static unsigned pe_put_item_name(uint8_t *out, unsigned n, unsigned max, uint16_t id) {
    const uint32_t name = pe_text_entry(0x08u, (unsigned)id - 1u);
    if (!name) return n;
    for (unsigned i = 0; n < max; ++i) {
        const uint8_t c = psx_mod_read_byte(name + i);
        if (c == 0xFFu) break;
        out[n++] = c;
    }
    return n;
}

static unsigned pe_put_ascii(uint8_t *out, unsigned n, unsigned max, const char *text) {
    for (; *text && n < max; ++text) out[n++] = pe_glyph(*text);
    return n;
}

/* Util_AppendFFTerminatedBytes(dst, src) entry: the discard dialog's suffix. */
static void pe_on_append_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[4] != PE_MENU_LABEL_BUFFER || !pe_loaded()) return;
    if (cpu->gpr[5] != pe_text_entry(0x04u, 4u)) return;     /* " will be discarded" */
    const PeSelected sel = pe_selected();
    if (!sel.what) return;
    if (!s_suffix) s_suffix = psx_mod_alloc_guest_memory(48, 4);
    if (!s_suffix) return;
    uint8_t out[48];
    unsigned n = 0;
    char buf[40];
    if (sel.what == 1) {
        snprintf(buf, sizeof buf, " will be dismantled for %u Junk",
                 (unsigned)pe_catalog_dismantle_preview(sel.rec));
        n = pe_put_ascii(out, n, sizeof out - 1, buf);
    } else {
        n = pe_put_ascii(out, n, sizeof out - 1, " x2 will become ");
        n = pe_put_item_name(out, n, sizeof out - 1, (uint16_t)(sel.id + 1u));
    }
    out[n++] = 0xFFu;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_byte(s_suffix + i, out[i]);
    cpu->gpr[5] = s_suffix;
}

/* Menu_OnItemDiscardConfirm(dialog, yes) entry: the game removes the
 * selected item right after; do the overhaul's part first. */
static void pe_on_discard_confirm(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!cpu->gpr[5] || !pe_loaded()) return;
    const PeSelected sel = pe_selected();
    char msg[80];
    if (sel.what == 1) {
        int unlocks = 0;
        const uint32_t paid = pe_catalog_dismantle_reward(sel.rec, &unlocks);
        snprintf(msg, sizeof msg, "Dismantled: +%u Junk, %d new effect(s) (total %u)",
                 (unsigned)paid, unlocks, (unsigned)pe_loot_junk());
        psx_mod_osd_message(msg);
    } else if (sel.what == 2) {
        psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)sel.other * 2u, (uint16_t)(sel.id + 1u));
        snprintf(msg, sizeof msg, "Combined into Medicine %u", (unsigned)(sel.id - PE_ITEM_MEDICINE_1 + 2u));
        psx_mod_osd_message(msg);
    }
}

#define PE_WORKSHOP_PAGE "Workshop"

/* Called from pe_mod_menu.c after pe_loot_shop_menu_register. */
void pe_native_menu_register(void) {
    (void)pe_menu_register_toggle(PE_WORKSHOP_PAGE, "Discard dismantles",
        "Discarding gear in Aya's menu pays Junk.", &s_discard_dismantles, 0);
    (void)pe_menu_register_toggle(PE_WORKSHOP_PAGE, "Discard combines",
        "Discard Medicine x2: next medicine.", &s_discard_combines, 0);
    (void)psx_mod_register_function_entry_plugin("pe.native.append-text",
        PE_FN_APPEND_FF_TEXT, pe_on_append_text);
    (void)psx_mod_register_function_entry_plugin("pe.native.discard",
        PE_FN_DISCARD_CONFIRM, pe_on_discard_confirm);
}
