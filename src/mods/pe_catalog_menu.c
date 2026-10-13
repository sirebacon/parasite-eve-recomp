/*
 * Loot overhaul, phase 5: the Mod Catalog as its own entry in Aya's main menu
 * (bridge findings pe-loot-overhaul-design-2026-10-11.md decisions 27, 31).
 *
 * The main menu is a column of icons, one per action bit of
 * g_MenuItemContextFlag (0x8009CEF0, & 0x1EF outside battle); row r is the
 * r-th set bit. Its title is Str_LookupTable10(action + 0x28) and its icon
 * sprite action + 0x56. A row past the last set bit maps to action 9: the
 * menu draws it with sprite 0x5F and title 0x31, and confirming it does
 * nothing (Menu_StepItemSelectScreen's default case). So:
 *   - Draw_SetPrimCallback (0x800647D0) on the main list (draw ==
 *     Menu_DrawItemList) gets one more row; its row cap (visible_rows_mirror,
 *     8 from grid descriptor 0) grows to fit, and the call itself grows the
 *     icon panel by the list's 16 px a row;
 *   - Draw_PrintRawText (0x8005F27C) shows "Mod Catalog" for title 0x31 while
 *     that row is under the cursor;
 *   - confirm on it in Menu_StepItemSelectScreen (0x80043DA4) opens the
 *     Catalog in a field text box over the menu (as Wayne's Workshop,
 *     pe_wayne_menu.c): your equipped gun and armor -> their mod slots -> the
 *     unlocked effects with prices (dimmed when they can't go in) and Remove.
 *     While it is up the menu's input queue is paused
 *     (g_MenuInputPollingPaused); on close the menu waits for every button to
 *     be released (g_MenuInputActive = 0) so the closing press isn't seen.
 * The menu masks directions out of the field inputs, so the box is driven
 * from the pad through the game's own button map (D_800A76F0).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_loot.h"
#include "pe_gametext.h"

#define PE_FN_SET_ROWS        0x800647D0u   /* Draw_SetPrimCallback */
#define PE_FN_ITEM_SELECT     0x80043DA4u   /* Menu_StepItemSelectScreen */
#define PE_FN_PRINT_RAW_TEXT  0x8005F27Cu   /* Draw_PrintRawText */
#define PE_FN_DRAW_TEXTBOXES  0x80037870u   /* Menu_DrawTextboxEntries */
#define PE_DRAW_ITEM_LIST     0x8004F838u   /* Menu_DrawItemList: the main list's draw */
#define PE_CONTEXT_FLAGS      0x8009CEF0u   /* g_MenuItemContextFlag */
#define PE_BATTLE_EQUIP_MODE  0x8009D028u   /* g_MenuBattleEquipMode */
#define PE_CURRENT_NODE       0x8009D15Cu   /* g_MenuWidgetCurrentNode */
#define PE_INPUT_ACTIVE       0x8009D0E8u   /* g_MenuInputActive */
#define PE_INPUT_PAUSED       0x8009D0ECu   /* g_MenuInputPollingPaused */
#define PE_INPUT_HELD         0x8009D0F0u   /* g_MenuInputHeldStatusMask */
#define PE_PAD_RAW            0x800BE9A2u   /* pad 1 buttons, active low */
#define PE_PAD_MAP            0x800A76F0u   /* u32[32]: pad bits of each input bit */
#define PE_PAD_PRESSED        0x8009D1F4u   /* this frame's new field inputs */
#define PE_CHOICE_CURSOR      0x8009CEA0u   /* s8 */
#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_TITLE_ACTION9      0x31u         /* Str_LookupTable10 index row 9 would show */

#define N_CHILD0      0x08u
#define N_DRAW        0x30u
#define N_GRID_W      0x34u
#define N_CURSOR_X    0x44u
#define N_CURSOR_Y    0x48u
#define N_Y_LIMIT     0x58u
#define N_ROW_CAP     0x6Cu                 /* visible_rows_mirror */

#define PE_TEXTBOX_BASE   0x800BCEA8u       /* g_TextboxEntries */
#define PE_TEXTBOX_STRIDE 0x38u
#define PE_TEXTBOX_COUNT  4u
#define TB_STATE   0x00u
#define TB_MESSAGE 0x04u
#define TB_STYLE   0x08u
#define TB_BACKGROUND 0x09u
#define TB_FLAGS   0x0Cu
#define TB_PAGE    0x10u
#define TB_X       0x12u
#define TB_Y       0x14u
#define TB_W       0x16u
#define TB_H       0x18u
#define PE_OUR_PAGE 0x7E7Eu                 /* page id no script uses */

#define IN_UP      0x0008u
#define IN_RIGHT   0x0010u
#define IN_DOWN    0x0020u
#define IN_LEFT    0x0040u
#define IN_CONFIRM 0x0100u
#define IN_CANCEL  0x0400u

#define PE_PAGE_SIZE 5
#define PE_BOX_TOP   48u

enum { CM_NONE, CM_GEAR, CM_SLOTS, CM_EFFECTS };
enum { WHICH_GUN = 0, WHICH_ARMOR = 1 };

static struct {
    int screen;
    uint32_t entry;                           /* the text box we drive */
    int which;                                /* gun / armor */
    int slot;
    int page;
    int options;                              /* choices on screen */
    int items;                                /* of which list rows */
    int has_next;                             /* a Next page option */
    uint32_t prev_pad;
    int hold;
    char note[48];                            /* title line after an action */
} s_cm;

static uint32_t s_buf;                        /* the box text, mod memory */
static uint32_t s_title;                      /* "Mod Catalog", mod memory */

int pe_catalog_menu_open(void) { return s_cm.screen != CM_NONE; }

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

/* ---- the main menu row --------------------------------------------------------- */
static int pe_vanilla_rows(void) {
    uint32_t mask = psx_mod_read_word(PE_CONTEXT_FLAGS) & 0x1EFu;
    int n = 0;
    for (; mask; mask >>= 1) n += (int)(mask & 1u);
    return n;
}

static int pe_row_wanted(void) {
    return pe_loaded() && pe_loot_enabled() && psx_mod_read_word(PE_BATTLE_EQUIP_MODE) == 0;
}

/* The main list, if it is the focused widget. */
static uint32_t pe_main_list(void) {
    const uint32_t node = psx_mod_read_word(PE_CURRENT_NODE);
    if (!pe_ram_pointer(node) || psx_mod_read_word(node + N_DRAW) != PE_DRAW_ITEM_LIST) return 0;
    return node;
}

/* Is our row on `list` and under its cursor? */
static int pe_on_our_row(uint32_t list) {
    const int ours = pe_vanilla_rows();
    if ((int)psx_mod_read_word(list + N_Y_LIMIT) != ours + 1) return 0;
    const int index = (int)psx_mod_read_word(list + N_GRID_W) * (int)psx_mod_read_word(list + N_CURSOR_Y)
                    + (int)psx_mod_read_word(list + N_CURSOR_X);
    return index == ours;
}

static void pe_on_set_rows(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t node = cpu->gpr[4];
    if (!pe_ram_pointer(node) || psx_mod_read_word(node + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (!pe_row_wanted()) return;
    const int rows = pe_vanilla_rows();
    if ((int)cpu->gpr[5] != rows) return;
    if ((int)psx_mod_read_word(node + N_ROW_CAP) < rows + 1)
        psx_mod_write_word(node + N_ROW_CAP, (uint32_t)(rows + 1));
    cpu->gpr[5] = (uint32_t)(rows + 1);
}

static void pe_on_print_raw_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!cpu->gpr[4] || cpu->gpr[4] != pe_text_entry(0x10u, PE_TITLE_ACTION9)) return;
    const uint32_t list = pe_main_list();
    if (!list || !pe_on_our_row(list)) return;
    if (!s_title) {
        s_title = psx_mod_alloc_guest_memory(16, 4);
        if (!s_title) return;
        PeText t;
        memset(&t, 0, sizeof t);
        pe_t_str(&t, "Mod Catalog");
        pe_t_byte(&t, G_END);
        for (unsigned i = 0; i < t.n; ++i) psx_mod_write_byte(s_title + i, t.b[i]);
    }
    cpu->gpr[4] = s_title;
}

/* ---- input: the pad through the game's button map ------------------------------- */
static uint32_t pe_pad_inputs(void) {
    const uint32_t pad = psx_mod_read_half(PE_PAD_RAW);
    uint32_t bits = ~pad & 0xFFFF9FFFu & 0xFFFFu;
    if (~pad & 0x2000u) bits |= 0x4000u;       /* as Boot_RenderFlagsAndPadInput */
    if (~pad & 0x4000u) bits |= 0x2000u;
    uint32_t inputs = 0;
    for (uint32_t i = 0; i < 32; ++i)
        if (bits & psx_mod_read_word(PE_PAD_MAP + i * 4u) & 0xFFFFu) inputs |= 1u << i;
    return inputs;
}

/* New presses this frame; up/down repeat while held. */
static uint32_t pe_pad_pressed(void) {
    const uint32_t now = pe_pad_inputs();
    uint32_t pressed = now & ~s_cm.prev_pad;
    if (now & (IN_UP | IN_DOWN)) {
        if (pressed & (IN_UP | IN_DOWN)) s_cm.hold = 0;
        else if (++s_cm.hold > 15 && (s_cm.hold & 3) == 0) pressed |= now & (IN_UP | IN_DOWN);
    }
    s_cm.prev_pad = now;
    return pressed;
}

/* ---- names ------------------------------------------------------------------------- */
static const char *const s_gun_names[21] = {
    "Empty", "Rate of Fire 2", "Rate of Fire 3", "Rate of Fire 5", "Rate of Fire 7",
    "Rate of Fire 10", "Random Fire", "Burst Spray", "Attack All", "Heat Bullets",
    "Frost Bullets", "Cyanide Bullets", "Acid Bullets", "Tranquilizer", "Counterattack",
    "Quickdraw", "Steal", "Steal on Attack", "Critical Up", "2 Commands", "3 Commands",
};
static const char *const s_armor_names[21] = {
    "Empty", "Resist Poison", "Resist Stiffness", "Resist Darkness", "Less Stolen",
    "Resist Confusion", "Auto Medicine", "Auto Cure", "Capacity 1", "Capacity 2",
    "Capacity 4", "Max HP Up", "Attack Up", "Slow Active Time", "PE Burden Down",
    "Attack Down", "", "Bargain", "Scavenger", "Lucky Find", "Salvager",
};

static const char *pe_fx_name(int which, int e) {
    if (e < 0 || e > 20) return "?";
    return which == WHICH_GUN ? s_gun_names[e] : s_armor_names[e];
}

static void pe_t_gear(PeText *t, int which) {
    const uint32_t rec = pe_cat_record(which);
    if (rec) pe_t_item(t, psx_mod_read_byte(rec + 4u));
    else pe_t_str(t, "none");
}

/* ---- screens ----------------------------------------------------------------------- */
/* Effects the effect screen lists: Remove (if the slot can be emptied), then
 * every unlocked effect in Catalog order. */
static int pe_fx_rows(int *values, int max) {
    int n = 0;
    if (pe_cat_remove(s_cm.which, s_cm.slot, 1) == PE_CAT_OK && n < max) values[n++] = -1;
    const int count = pe_cat_board_count(s_cm.which);
    for (int i = 0; i < count && n < max; ++i) {
        const int e = pe_cat_board_effect(s_cm.which, i);
        if (pe_cat_unlocked(s_cm.which, e)) values[n++] = e;
    }
    return n;
}

static int pe_row_count(void) {
    int values[32];
    switch (s_cm.screen) {
    case CM_GEAR: return 2;
    case CM_SLOTS: return pe_cat_slots(s_cm.which);
    case CM_EFFECTS: return pe_fx_rows(values, 32);
    default: return 0;
    }
}

/* Row i: its value (gear / slot / effect, -1 Remove) and whether it is dimmed. */
static int pe_row_value(int i, int *dim) {
    int values[32];
    *dim = 0;
    switch (s_cm.screen) {
    case CM_GEAR:
        *dim = !pe_cat_record(i);
        return i;
    case CM_SLOTS:
        *dim = pe_cat_is_bag_mod(s_cm.which, pe_cat_slot_effect(s_cm.which, i));
        return i;
    case CM_EFFECTS: {
        const int n = pe_fx_rows(values, 32);
        if (i < 0 || i >= n) return 0;
        if (values[i] > 0)
            *dim = pe_cat_install(s_cm.which, s_cm.slot, values[i], 1) != PE_CAT_OK;
        return values[i];
    }
    default:
        return 0;
    }
}

static void pe_row_label(PeText *t, int i, int value) {
    switch (s_cm.screen) {
    case CM_GEAR:
        pe_t_str(t, i == WHICH_GUN ? "Gun: " : "Armor: ");
        pe_t_gear(t, i);
        break;
    case CM_SLOTS:
        pe_t_num(t, (unsigned)i + 1u);
        pe_t_str(t, "  ");
        pe_t_str(t, pe_fx_name(s_cm.which, pe_cat_slot_effect(s_cm.which, i)));
        break;
    case CM_EFFECTS:
        if (value < 0) {
            pe_t_str(t, "Remove ");
            pe_t_str(t, pe_fx_name(s_cm.which, pe_cat_slot_effect(s_cm.which, s_cm.slot)));
        } else {
            pe_t_str(t, pe_fx_name(s_cm.which, value));
            pe_t_str(t, "  ");
            if (pe_cat_slot_effect(s_cm.which, s_cm.slot) == value) {
                pe_t_str(t, "here");
            } else {
                pe_t_num(t, (unsigned)pe_cat_price(s_cm.which, value));
                if (pe_cat_needs_tool(s_cm.which, value)) pe_t_str(t, " Tool");
            }
        }
        break;
    default:
        break;
    }
}

static void pe_title(PeText *t) {
    if (s_cm.note[0]) {
        pe_t_str(t, s_cm.note);
    } else {
        if (s_cm.screen == CM_GEAR) pe_t_str(t, "Mod Catalog");
        else pe_t_gear(t, s_cm.which);
        if (s_cm.screen == CM_EFFECTS) {
            pe_t_str(t, " slot ");
            pe_t_num(t, (unsigned)s_cm.slot + 1u);
        }
        pe_t_str(t, "   Junk ");
        pe_t_num(t, (unsigned)pe_loot_junk());
    }
    pe_t_byte(t, G_NEWLINE);
}

static int pe_pages(int count) { return count <= PE_PAGE_SIZE + 1 ? 1 : (count + PE_PAGE_SIZE - 1) / PE_PAGE_SIZE; }

/* Write the current screen into our box. */
static void pe_show(int screen, int cursor) {
    PeText t;
    memset(&t, 0, sizeof t);
    t.lines = 1;
    s_cm.screen = screen;
    const int count = pe_row_count();
    const int pages = pe_pages(count);
    if (s_cm.page >= pages) s_cm.page = 0;
    const int first = pages > 1 ? s_cm.page * PE_PAGE_SIZE : 0;
    int items = count - first;
    if (pages > 1 && items > PE_PAGE_SIZE) items = PE_PAGE_SIZE;
    if (items < 0) items = 0;
    s_cm.items = items;
    s_cm.has_next = pages > 1;
    s_cm.options = items + s_cm.has_next + 1;

    pe_title(&t);
    pe_t_choice(&t, s_cm.options);
    for (int k = 0; k < items; ++k) {
        int dim;
        const int value = pe_row_value(first + k, &dim);
        pe_t_option_start(&t, k == 0);
        if (dim) { pe_t_byte(&t, G_CODE); pe_t_byte(&t, G_RED); }
        pe_row_label(&t, first + k, value);
        if (dim) { pe_t_byte(&t, G_CODE); pe_t_byte(&t, G_NORMAL); }
    }
    if (s_cm.has_next) { pe_t_option_start(&t, items == 0); pe_t_str(&t, "Next page"); }
    pe_t_option_start(&t, items == 0 && !s_cm.has_next);
    pe_t_str(&t, screen == CM_GEAR ? "Close" : "Back");
    pe_t_byte(&t, G_END);

    if (!s_buf) s_buf = psx_mod_alloc_guest_memory(PE_TEXT_MAX, 4);
    if (!s_buf) return;
    for (unsigned i = 0; i < t.n; ++i) psx_mod_write_byte(s_buf + i, t.b[i]);
    const uint32_t e = s_cm.entry;
    unsigned width = t.widest * 9u + 24u;
    if (width < 200u) width = 200u;
    if (width > 304u) width = 304u;
    psx_mod_write_word(e + TB_MESSAGE, s_buf);
    psx_mod_write_half(e + TB_X, (uint16_t)((320u - width) / 2u));
    psx_mod_write_half(e + TB_Y, (uint16_t)PE_BOX_TOP);
    psx_mod_write_half(e + TB_W, (uint16_t)width);
    psx_mod_write_half(e + TB_H, (uint16_t)((unsigned)t.lines * 12u + 10u));
    if (cursor < 0) cursor = 0;
    if (cursor >= s_cm.options) cursor = s_cm.options - 1;
    psx_mod_write_byte(PE_CHOICE_CURSOR, (uint8_t)cursor);
}

static void pe_open(void) {
    uint32_t e = 0;
    for (uint32_t i = 0; i < PE_TEXTBOX_COUNT && !e; ++i) {
        const uint32_t a = PE_TEXTBOX_BASE + i * PE_TEXTBOX_STRIDE;
        if (psx_mod_read_byte(a + TB_STATE) == 0) e = a;
    }
    if (!e) return;
    memset(&s_cm, 0, sizeof s_cm);
    s_cm.entry = e;
    s_cm.prev_pad = pe_pad_inputs();          /* the opening press isn't ours */
    psx_mod_write_byte(e + TB_STYLE, 1);      /* framed box */
    psx_mod_write_byte(e + TB_BACKGROUND, 0);
    psx_mod_write_word(e + TB_FLAGS, 0);
    psx_mod_write_half(e + TB_PAGE, PE_OUR_PAGE);
    pe_show(CM_GEAR, 0);
    psx_mod_write_byte(e + TB_STATE, 2);      /* showing; 1 would re-read a script message */
    psx_mod_write_word(PE_INPUT_PAUSED, 1);
}

static void pe_close(void) {
    const uint32_t e = s_cm.entry;
    if (e && psx_mod_read_byte(e + TB_STATE) == 2 && psx_mod_read_word(e + TB_MESSAGE) == s_buf)
        psx_mod_write_byte(e + TB_STATE, 0);
    psx_mod_write_word(PE_INPUT_PAUSED, 0);
    psx_mod_write_word(PE_INPUT_HELD, 0);
    psx_mod_write_word(PE_INPUT_ACTIVE, 0);   /* the menu waits for all buttons up */
    memset(&s_cm, 0, sizeof s_cm);
}

static void pe_on_item_select(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (s_cm.screen != CM_NONE) { cpu->gpr[5] = 0; return; }
    if (!(cpu->gpr[5] & 0x10000u) || !pe_row_wanted()) return;
    const uint32_t root = cpu->gpr[4];
    if (!pe_ram_pointer(root)) return;
    const uint32_t list = psx_mod_read_word(root + N_CHILD0);
    if (!pe_ram_pointer(list) || psx_mod_read_word(list + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (!pe_on_our_row(list)) return;
    pe_open();                                /* the game finds no action there */
}

/* ---- actions ----------------------------------------------------------------------- */
static void pe_note(const char *text) { snprintf(s_cm.note, sizeof s_cm.note, "%s", text); }

static void pe_show_slots(int slot) {
    s_cm.slot = slot;
    const int pages = pe_pages(pe_cat_slots(s_cm.which));
    s_cm.page = pages > 1 ? slot / PE_PAGE_SIZE : 0;
    pe_show(CM_SLOTS, pages > 1 ? slot % PE_PAGE_SIZE : slot);
}

static void pe_why_not(int result, int e) {
    char msg[48];
    switch (result) {
    case PE_CAT_JUNK:
        snprintf(msg, sizeof msg, "Need %u Junk, you have %u", (unsigned)pe_cat_price(s_cm.which, e),
                 (unsigned)pe_loot_junk());
        pe_note(msg);
        break;
    case PE_CAT_TOOL: pe_note("Needs a Tool or a Tool Kit"); break;
    case PE_CAT_BAG: pe_note("Capacity mods stay put"); break;
    case PE_CAT_ALREADY: pe_note("Already in this slot"); break;
    case PE_CAT_LOCKED: pe_note("Not unlocked yet"); break;
    case PE_CAT_NO_ITEM: pe_note("Nothing equipped"); break;
    default: pe_note("Can't do that now"); break;
    }
}

static void pe_confirm(int cursor) {
    const int back = s_cm.options - 1;
    if (cursor == back) {
        s_cm.note[0] = '\0';
        if (s_cm.screen == CM_GEAR) pe_close();
        else if (s_cm.screen == CM_SLOTS) { s_cm.page = 0; pe_show(CM_GEAR, s_cm.which); }
        else pe_show_slots(s_cm.slot);
        return;
    }
    if (s_cm.has_next && cursor == s_cm.items) {
        s_cm.page = (s_cm.page + 1) % pe_pages(pe_row_count());
        s_cm.note[0] = '\0';
        pe_show(s_cm.screen, 0);
        return;
    }
    if (cursor < 0 || cursor >= s_cm.items) return;
    const int first = s_cm.has_next ? s_cm.page * PE_PAGE_SIZE : 0;
    int dim;
    const int value = pe_row_value(first + cursor, &dim);
    char msg[48];
    switch (s_cm.screen) {
    case CM_GEAR:
        if (!pe_cat_record(value)) { pe_note(value == WHICH_GUN ? "No gun equipped" : "No armor equipped"); pe_show(CM_GEAR, cursor); return; }
        s_cm.which = value;
        s_cm.note[0] = '\0';
        if (pe_cat_slots(value) <= 0) { pe_note("It has no mod slots"); pe_show(CM_GEAR, cursor); return; }
        pe_show_slots(0);
        return;
    case CM_SLOTS: {
        s_cm.note[0] = '\0';
        s_cm.slot = value;
        const int fx = pe_cat_slot_effect(s_cm.which, value);
        if (pe_cat_is_bag_mod(s_cm.which, fx)) { pe_note("Capacity mods stay put"); pe_show_slots(value); return; }
        int values[32];
        if (pe_fx_rows(values, 32) == 0) { pe_note("Nothing unlocked: dismantle gear"); pe_show_slots(value); return; }
        s_cm.page = 0;
        pe_show(CM_EFFECTS, 0);
        return;
    }
    case CM_EFFECTS: {
        const int had = pe_cat_slot_effect(s_cm.which, s_cm.slot);
        if (value < 0) {
            const int r = pe_cat_remove(s_cm.which, s_cm.slot, 0);
            if (r != PE_CAT_OK) { pe_why_not(r, had); pe_show(CM_EFFECTS, cursor); return; }
            snprintf(msg, sizeof msg, "Removed %s", pe_fx_name(s_cm.which, had));
        } else {
            const int r = pe_cat_install(s_cm.which, s_cm.slot, value, 0);
            if (r != PE_CAT_OK) { pe_why_not(r, value); pe_show(CM_EFFECTS, cursor); return; }
            snprintf(msg, sizeof msg, pe_cat_is_bag_mod(s_cm.which, value) ? "Installed %s, re-equip"
                     : "Installed %s", pe_fx_name(s_cm.which, value));
        }
        pe_note(msg);
        pe_show_slots(s_cm.slot);
        return;
    }
    default:
        return;
    }
}

static void pe_cancel(void) {
    s_cm.note[0] = '\0';
    if (s_cm.screen == CM_GEAR) pe_close();
    else if (s_cm.screen == CM_SLOTS) { s_cm.page = 0; pe_show(CM_GEAR, s_cm.which); }
    else pe_show_slots(s_cm.slot);
}

/* Every frame, before the text boxes are drawn. */
static void pe_on_draw_textboxes(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (s_cm.screen == CM_NONE) return;
    const uint32_t e = s_cm.entry;
    if (!pe_main_list() || psx_mod_read_byte(e + TB_STATE) != 2 ||
        psx_mod_read_word(e + TB_MESSAGE) != s_buf) {
        pe_close();                           /* the menu or the box went away */
        return;
    }
    psx_mod_write_word(PE_INPUT_PAUSED, 1);
    const uint32_t pressed = pe_pad_pressed();
    /* The box moves its cursor and closes on the field inputs: they're ours. */
    psx_mod_write_word(PE_PAD_PRESSED, psx_mod_read_word(PE_PAD_PRESSED) &
                       ~(uint32_t)(IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT | IN_CONFIRM | IN_CANCEL));
    int cursor = (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR);
    if (pressed & IN_CONFIRM) { pe_confirm(cursor); return; }
    if (pressed & IN_CANCEL) { pe_cancel(); return; }
    if ((pressed & (IN_LEFT | IN_RIGHT)) && s_cm.has_next) {
        const int pages = pe_pages(pe_row_count());
        s_cm.page = (s_cm.page + ((pressed & IN_RIGHT) ? 1 : pages - 1)) % pages;
        s_cm.note[0] = '\0';
        pe_show(s_cm.screen, 0);
        return;
    }
    if (pressed & (IN_UP | IN_DOWN)) {
        cursor += (pressed & IN_DOWN) ? 1 : -1;
        if (cursor < 0) cursor = s_cm.options - 1;
        if (cursor >= s_cm.options) cursor = 0;
        psx_mod_write_byte(PE_CHOICE_CURSOR, (uint8_t)cursor);
    }
}

/* Save states: s_cm rides along (block "pe.catalog.ui"); the text lives in
 * mod memory, which snapshots don't hold, so it is written afresh. */
static void pe_cm_state_loaded(int result, uint32_t saved_version, const void *saved,
                               uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_cm, 0, sizeof s_cm);
}

static void pe_cm_restored(void) {
    if (s_cm.screen == CM_NONE) return;
    const uint32_t e = s_cm.entry;
    if (e < PE_TEXTBOX_BASE || e >= PE_TEXTBOX_BASE + PE_TEXTBOX_COUNT * PE_TEXTBOX_STRIDE ||
        psx_mod_read_byte(e + TB_STATE) != 2 || !pe_main_list()) {
        memset(&s_cm, 0, sizeof s_cm);
        return;
    }
    s_cm.prev_pad = pe_pad_inputs();
    pe_show(s_cm.screen, (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR));
}

/* Called from pe_mod_menu.c after pe_wayne_menu_register. */
void pe_catalog_menu_register(void) {
    (void)psx_mod_register_function_entry_plugin("pe.catalog.rows", PE_FN_SET_ROWS, pe_on_set_rows);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.title", PE_FN_PRINT_RAW_TEXT,
                                                 pe_on_print_raw_text);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.open", PE_FN_ITEM_SELECT,
                                                 pe_on_item_select);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.box", PE_FN_DRAW_TEXTBOXES,
                                                 pe_on_draw_textboxes);
    (void)psx_mod_register_state("pe.catalog.ui", 1u, &s_cm, sizeof s_cm, pe_cm_state_loaded);
    (void)psx_mod_register_restore_callback(pe_cm_restored);
}
