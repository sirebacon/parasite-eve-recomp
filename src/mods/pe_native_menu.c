/*
 * Loot overhaul, phase 5: native menu actions in Aya's own item menu
 * (bridge findings pe-loot-overhaul-design-2026-10-11.md decision 25, custom
 * items doc C10; decision 44 for how they are offered).
 *
 * The game's item menu has a fixed action table, so instead of new rows the
 * existing Discard does the overhaul's work when it makes sense:
 *
 *   - Discard on a gun or armor dismantles it (the row reads "Dismantle"): the
 *     confirm dialog reads "<name>: N Junk, M mods recovered, rebuildable" (shorter for long names), and "yes" unlocks its effects in
 *     the Mod Catalog, puts it on the Rebuild list and pays the Junk
 *     (pe_catalog_dismantle_reward); the game's own code then removes it.
 *   - Combine (C10): a real fourth action row, "Combine", on Medicine 1-3
 *     in Aya's bag while she holds two of it (not in battle). The action list
 *     is a widget of three rows reading g_MenuItemActionStateTable
 *     (0x80092234, int [context][3]); consumables (context 0) are Use / Move
 *     / Discard. While the list is open for such a medicine, the list node
 *     (g_MenuWidgetCurrentNode) gets a fourth row (y_limit / visible_rows)
 *     and its panel 16 px more height; the fourth row reads the next
 *     context's first entry, action 3, whose label (Str_LookupTable4(3),
 *     drawn by Draw_PrintCenteredTextInWidth) is swapped for "Combine", and
 *     accepting it (Menu_StepEquipScreen with the cursor on row 3) combines
 *     the pair -- the selected medicine becomes the next one, the other goes
 *     -- and closes the list as a cancel would. Discard stays vanilla.
 *
 * Hooks: Util_AppendFFTerminatedBytes (0x80052C08) while the dialog label is
 * built (Menu_StepItemDetailPanel appends Str_LookupTable4(4) " will be
 * discarded" to the item name in g_MenuItemLabelBuffer, then sizes the dialog
 * to it), and Menu_OnItemDiscardConfirm (0x80045110, a1 = yes). The selected
 * item is g_MenuActiveItemList (0 Aya's bag, else the storage list) and
 * g_MenuActiveItemSlot. Both behaviours have a Workshop toggle, on by default.
 *
 * "Dismantle" (68 px) is wider than the action popup's text (56 px; even the
 * game's "Tune up" overhangs it), so while Discard dismantles the popup is
 * 16 px wider: simple descriptors 2/3 (width 60, centered) and grid
 * descriptors 2/3 (text width 56) -- static tables the popup is built from.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"

#define PE_FN_APPEND_FF_TEXT     0x80052C08u   /* Util_AppendFFTerminatedBytes(dst, src) */
#define PE_FN_DISCARD_CONFIRM    0x80045110u   /* Menu_OnItemDiscardConfirm(dialog, yes) */
#define PE_FN_CONTEXT_PICK       0x80050878u   /* Sfx_ContextPick(row): draws an action row */
#define PE_FN_CENTERED_TEXT      0x8005F354u   /* Draw_PrintCenteredTextInWidth(text, w) */
#define PE_FN_STEP_EQUIP_SCREEN  0x80044B0Cu   /* Menu_StepEquipScreen(node, flags) */

#define PE_MENU_CURRENT_NODE     0x8009D15Cu   /* g_MenuWidgetCurrentNode */
#define PE_MENU_ACTION_CONTEXT   0x8009CDA8u   /* g_MenuItemActionContext: 0 consumables */
#define PE_GAME_BLOCK_ADDR       0x8009D1A0u
#define NODE_PARENT       0x04u
#define NODE_CHILD0       0x08u
#define NODE_UPDATE       0x2Cu
#define NODE_GRID_WIDTH   0x34u
#define NODE_VISIBLE_ROWS 0x38u
#define NODE_CURSOR_X     0x44u
#define NODE_CURSOR_Y     0x48u
#define NODE_Y_LIMIT      0x58u
#define NODE_ROWS_MIRROR  0x6Cu
#define PE_INPUT_ACCEPT   0x10000u
#define PE_INPUT_CANCEL   0x40u
/* Shown order Use / Move / Combine / Discard: rows 2 and 3 are drawn swapped,
 * so the game's row 2 (Discard) appears last and row 3 (Combine) above it. */
#define PE_COMBINE_ROW    2               /* where Combine is shown */
#define PE_DISCARD_SHOWN  3               /* where Discard is shown */

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

#define PE_SIMPLE_DESCRIPTORS 0x80092478u  /* g_MenuWidgetSimpleDescriptors, 0x10 each */
#define PE_GRID_DESCRIPTORS   0x80092888u  /* g_MenuWidgetGridDescriptors, 0x20 each */
#define PE_POPUP_WIDTH      60u            /* simple +0x08 */
#define PE_POPUP_TEXT_WIDTH 56u            /* grid +0x14 */
#define PE_POPUP_WIDEN      16u

#define PE_ITEM_MEDICINE_1 0x06u
#define PE_ITEM_MEDICINE_3 0x08u

static int s_discard_dismantles = 1;     /* decision 25 via Discard (44) */
static int s_combine_row = 1;            /* C10: the Combine action row */
static uint32_t s_suffix;                /* our label suffix, mod memory */
static uint32_t s_combine_text;          /* "Combine", mod memory */
static uint32_t s_extended_node;         /* the action list we gave a 4th row */
static int s_restore_cursor;             /* put the cursor back on Discard's row */

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
    switch (c) {                              /* from the game's own dialogue */
    case '.': return 0x2F; case ',': return 0x4B; case ':': return 0x4A;
    case '!': return 0x2B; case '\'': return 0x2E;
    default: return 0x0F;
    }
}

/* What the selected item is: 1 = gun/armor (rec set), 0 = anything else. */
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
    return s;
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
    /* One line (the menu font has no line break), about 36 characters with the
     * name: the longest of these that fits. Mods = effects still on the item,
     * which go to the Mod Catalog. */
    unsigned name_len = 0;
    while (name_len < 40 && psx_mod_read_byte(PE_MENU_LABEL_BUFFER + name_len) != 0xFFu) ++name_len;
    int mods = 0;
    int slots = psx_mod_read_byte(sel.rec + 0x14u);
    if (slots > 10) slots = 10;
    for (int m = 0; m < slots; ++m) mods += (psx_mod_read_byte(sel.rec + 0x15u + (uint32_t)m) & 0x1F) != 0;
    const unsigned junk = (unsigned)pe_catalog_dismantle_preview(sel.rec);
    char buf[48];
    if (mods) {
        snprintf(buf, sizeof buf, ": %u Junk, %d mod%s recovered, rebuildable", junk, mods, mods == 1 ? "" : "s");
        if (name_len + strlen(buf) > 36u)
            snprintf(buf, sizeof buf, ": %u Junk, %d mod%s recovered", junk, mods, mods == 1 ? "" : "s");
    } else {
        snprintf(buf, sizeof buf, ": %u Junk, rebuildable", junk);
    }
    uint8_t out[48];
    unsigned n = pe_put_ascii(out, 0, sizeof out - 1, buf);
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
    if (sel.what == 1) {                      /* the confirm dialog already said what it pays */
        int unlocks = 0;
        (void)pe_catalog_dismantle_reward(sel.rec, &unlocks);
    }
}

/* ---- Combine row (C10) ------------------------------------------------------ */
/* The bag slot of another medicine like the selected one, or -1 (only for
 * Medicine 1-3 selected in Aya's bag, outside battle, economy on). */
static int pe_combine_pair(uint16_t *id_out) {
    if (!s_combine_row || !pe_loot_enabled() || !pe_loaded()) return -1;
    if (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2u) return -1;
    if (psx_mod_read_word(PE_MENU_ACTIVE_LIST) != 0) return -1;
    if (psx_mod_read_word(PE_MENU_ACTION_CONTEXT) != 0) return -1;
    const int slot = (int)psx_mod_read_word(PE_MENU_ACTIVE_SLOT);
    const uint32_t limit = pe_bag_limit();
    if (slot < 0 || (uint32_t)slot >= limit) return -1;
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u);
    if (v < PE_ITEM_MEDICINE_1 || v > PE_ITEM_MEDICINE_3) return -1;
    for (uint32_t i = 0; i < limit; ++i)
        if ((int)i != slot && psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == v) {
            if (id_out) *id_out = v;
            return (int)i;
        }
    return -1;
}

/* The open item-action list node, if the current node is one. */
static uint32_t pe_action_list_node(void) {
    const uint32_t node = psx_mod_read_word(PE_MENU_CURRENT_NODE);
    if (!pe_ram_pointer(node)) return 0;
    const uint32_t parent = psx_mod_read_word(node + NODE_PARENT);
    if (!pe_ram_pointer(parent) || psx_mod_read_word(parent + NODE_UPDATE) != PE_FN_STEP_EQUIP_SCREEN)
        return 0;
    return node;
}

static void pe_set_rows(uint32_t node, int rows) {
    const uint32_t parent = psx_mod_read_word(node + NODE_PARENT);
    const int now = (int)psx_mod_read_word(node + NODE_Y_LIMIT);
    if (now == rows) return;
    psx_mod_write_word(node + NODE_Y_LIMIT, (uint32_t)rows);
    psx_mod_write_word(node + NODE_VISIBLE_ROWS, (uint32_t)rows);
    psx_mod_write_word(node + NODE_ROWS_MIRROR, (uint32_t)rows);
    if (pe_ram_pointer(parent))                /* the panel: 16 px per row */
        psx_mod_write_word(parent + NODE_VISIBLE_ROWS,
                           psx_mod_read_word(parent + NODE_VISIBLE_ROWS) + (uint32_t)((rows - now) * 16));
}

/* The extended list is the one on screen: still the current node, still a
 * four-row consumables list (a new list can reuse the node's memory). */
static int pe_extended_active(void) {
    return s_extended_node && s_extended_node == psx_mod_read_word(PE_MENU_CURRENT_NODE) &&
           psx_mod_read_word(s_extended_node + NODE_Y_LIMIT) == 4u &&
           psx_mod_read_word(PE_MENU_ACTION_CONTEXT) == 0;
}

/* Sfx_ContextPick(row) entry: on the extended list rows 2 and 3 swap (Combine
 * above Discard); once per frame, at row 0, grow or shrink the list. */
static void pe_on_context_pick(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t row = cpu->gpr[4];
    if (row != 0) {
        if (pe_extended_active()) {
            if (row == 2) cpu->gpr[4] = 3;
            else if (row == 3) cpu->gpr[4] = 2;
        }
        return;
    }
    const uint32_t node = pe_action_list_node();
    if (!node) return;                       /* e.g. the Discard dialog is on top */
    const int want = pe_combine_pair(0) >= 0;
    const uint32_t rows = psx_mod_read_word(node + NODE_Y_LIMIT);
    /* Consumable lists have three rows unless we added one, so a four-row one
     * (e.g. brought back by a save state) is ours either way. */
    if (want && rows == 3u) {
        pe_set_rows(node, 4);
        s_extended_node = node;
    } else if (want && rows == 4u) {
        s_extended_node = node;
    } else if (!want && rows == 4u && psx_mod_read_word(PE_MENU_ACTION_CONTEXT) == 0) {
        pe_set_rows(node, 3);
        s_extended_node = 0;
    }
    /* After a Discard press the cursor was moved to Discard's real row (2);
     * back on the list (the dialog said no), it goes back down. */
    if (s_restore_cursor && node == s_extended_node) {
        if (psx_mod_read_word(node + NODE_CURSOR_Y) == 2u)
            psx_mod_write_word(node + NODE_CURSOR_Y, PE_DISCARD_SHOWN);
        s_restore_cursor = 0;
    }
}

/* A short label in mod memory, written once. */
static uint32_t pe_label(uint32_t *slot, const char *ascii) {
    if (*slot) return *slot;
    const uint32_t g = psx_mod_alloc_guest_memory(16, 4);
    if (!g) return 0;
    uint8_t out[16];
    unsigned n = pe_put_ascii(out, 0, sizeof out - 1, ascii);
    out[n++] = 0xFFu;
    for (unsigned i = 0; i < n; ++i) psx_mod_write_byte(g + i, out[i]);
    return *slot = g;
}

/* Draw_PrintCenteredTextInWidth(text, width) entry: action row labels.
 *   - "Combine" for the extended medicine list's fourth row (action 3);
 *   - "Dismantle" for Discard (action 2) on a gun or armor while Discard
 *     dismantles (decision 47); everything else keeps "Discard". */
static void pe_on_centered_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    static uint32_t s_dismantle_text;
    const uint32_t text = cpu->gpr[4];
    if (pe_extended_active() && text == pe_text_entry(0x04u, 3u)) {
        const uint32_t g = pe_label(&s_combine_text, "Combine");
        if (g) cpu->gpr[4] = g;
        return;
    }
    if (text == pe_text_entry(0x04u, 2u) && pe_action_list_node() && pe_selected().what == 1) {
        const uint32_t g = pe_label(&s_dismantle_text, "Dismantle");
        if (g) cpu->gpr[4] = g;
    }
}

/* Menu_StepEquipScreen(node, flags) entry: accept on the Combine row. */
static void pe_on_step_equip_screen(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!(cpu->gpr[5] & PE_INPUT_ACCEPT) || !pe_extended_active()) return;
    const uint32_t parent = cpu->gpr[4];
    if (!pe_ram_pointer(parent) || psx_mod_read_word(parent + NODE_CHILD0) != s_extended_node) return;
    const uint32_t list = s_extended_node;
    const int cell = (int)psx_mod_read_word(list + NODE_GRID_WIDTH) * (int)psx_mod_read_word(list + NODE_CURSOR_Y) +
                     (int)psx_mod_read_word(list + NODE_CURSOR_X);
    if (cell == PE_DISCARD_SHOWN) {          /* the game's Discard is its row 2 */
        psx_mod_write_word(list + NODE_CURSOR_Y, 2u);
        s_restore_cursor = 1;
        return;
    }
    if (cell != PE_COMBINE_ROW) return;
    uint16_t id = 0;
    const int other = pe_combine_pair(&id);
    if (other < 0) return;
    const int slot = (int)psx_mod_read_word(PE_MENU_ACTIVE_SLOT);
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, (uint16_t)(id + 1u));
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)other * 2u, 0);
    pe_log("combine", "from=0x%02X into=0x%02X", (unsigned)id, (unsigned)(id + 1u));
    char msg[48];
    snprintf(msg, sizeof msg, "Combined into Medicine %u", (unsigned)(id - PE_ITEM_MEDICINE_1 + 2u));
    psx_mod_osd_message(msg);
    s_extended_node = 0;
    cpu->gpr[5] = PE_INPUT_CANCEL;            /* the game closes the list */
}

/* Every frame: the action popup's width for the next time it opens. */
static void pe_popup_width_tick(void) {
    const uint32_t extra = s_discard_dismantles && pe_loot_enabled() ? PE_POPUP_WIDEN : 0u;
    for (uint32_t d = 2; d <= 3; ++d) {
        const uint32_t w = PE_SIMPLE_DESCRIPTORS + d * 0x10u + 0x08u;
        const uint32_t tw = PE_GRID_DESCRIPTORS + d * 0x20u + 0x14u;
        if (psx_mod_read_word(w) == PE_POPUP_WIDTH + (PE_POPUP_WIDEN - extra))
            psx_mod_write_word(w, PE_POPUP_WIDTH + extra);
        if (psx_mod_read_word(tw) == PE_POPUP_TEXT_WIDTH + (PE_POPUP_WIDEN - extra))
            psx_mod_write_word(tw, PE_POPUP_TEXT_WIDTH + extra);
    }
}

/* After a save state loads, nothing here is current any more. */
static void pe_native_restored(void) {
    s_extended_node = 0;
    s_restore_cursor = 0;
}

#define PE_WORKSHOP_PAGE "Workshop"

/* Called from pe_mod_menu.c after pe_loot_shop_menu_register. */
void pe_native_menu_register(void) {
    (void)pe_menu_register_toggle(PE_WORKSHOP_PAGE, "Discard dismantles",
        "Discarding gear in Aya's menu pays Junk.", &s_discard_dismantles, 0);
    (void)pe_menu_register_toggle(PE_WORKSHOP_PAGE, "Combine medicine",
        "Combine row: Medicine x2 -> next.", &s_combine_row, 0);
    (void)psx_mod_register_function_entry_plugin("pe.native.append-text",
        PE_FN_APPEND_FF_TEXT, pe_on_append_text);
    (void)psx_mod_register_function_entry_plugin("pe.native.discard",
        PE_FN_DISCARD_CONFIRM, pe_on_discard_confirm);
    (void)psx_mod_register_function_entry_plugin("pe.native.combine-rows",
        PE_FN_CONTEXT_PICK, pe_on_context_pick);
    (void)psx_mod_register_function_entry_plugin("pe.native.combine-label",
        PE_FN_CENTERED_TEXT, pe_on_centered_text);
    (void)psx_mod_register_function_entry_plugin("pe.native.combine-accept",
        PE_FN_STEP_EQUIP_SCREEN, pe_on_step_equip_screen);
    (void)psx_mod_register_restore_callback(pe_native_restored);
    (void)psx_mod_register_vblank_plugin("pe.native.popup-width", pe_popup_width_tick);
}
