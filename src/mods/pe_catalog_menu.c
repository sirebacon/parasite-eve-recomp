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
 *   - with Distribute Bonus Points (action 8, the last row) present, our row
 *     goes above it: Draw_AllocSprite (0x8005EB64) swaps their icons (0x5E,
 *     0x5F) in the main list, the titles swap, and confirm on the BP row is
 *     handed to the game as the row above (the cursor is put back after);
 *   - confirm on it in Menu_StepItemSelectScreen (0x80043DA4) opens the
 *     Catalog.
 *
 * The Catalog is built from the game's own menu widgets, laid out like its
 * Tune Up screen: a slot panel (your gun's and armor's mod slots under their
 * names), an effect panel (Remove, the unlocked effects with prices, the
 * locked ones as "Unknown") and an info panel (Junk, Tools, what the
 * highlighted effect costs or why it can't go in); the title bar shows the
 * highlighted effect's description from the game's own text. Installing asks
 * first, in the game's own OK / Cancel dialog (the one Discard and Tune Up
 * use): a mode-1 node 0x29 drawn by Menu_DrawItemLabel (0x80044E14, the first
 * line from g_MenuItemLabelBuffer) and stepped by Menu_ConfirmDialogHandler
 * (0x80044E98), with a mode-2 child 0x29 (Menu_DrawActionOptionList, text base
 * 5: OK / Cancel), built as MenuWidget_CreateSimpleNode / CreateNode would from
 * descriptors 0x29. The other lines (price, what it takes out, the game's own
 * "Is this OK?") are drawn under the first, glyph by glyph as
 * Draw_AllocTexturedQuad draws, when Draw_PrintCenteredTextInWidth prints the
 * label; the panel grows 14 px a line. The handler reports through
 * g_MenuConfirmCallback, which we point at MenuWidget_GetCurrentNode: its entry
 * hook, called from the handler, reads a1 (1 = OK) and installs. Remove is
 * free and the mod stays unlocked, so it doesn't ask. Each panel is a
 * mode-1 node with a mode-2 list child, written into the widget pool as
 * MenuWidget_CreateSimpleNode / MenuWidget_CreateNode would. The lists step
 * with Menu_StepScrollCursor (cursor, scrolling, the move sound) and draw with
 * Menu_DrawActionOptionList (0x8004F950), whose rows
 * (Menu_DrawActionOptionRow, 0x800509A8) print one table string through
 * Draw_PrintCenteredTextInWidth (0x8005F354): there the string becomes our
 * row's text in the menu font, left-aligned by giving it its own width, and
 * rows that can't be picked are dimmed (D_8009D10C) as the game dims. Cross,
 * Circle, Left and Right reach the panel's update (Menu_StepItemSelectScreen
 * again, given no buttons so it does nothing) and are acted on next frame.
 * Like the game's own screens it hides Aya's status panels (0x12, 0x18, 0x2D:
 * hidden while cursor_y is set, MenuWidget_UpdateAndDraw) and the HP overlay.
 *
 * KEY ITEMS (2026-10-14): a second extra row opens a Key Items screen in the
 * same panels: the key item pocket (pe_key_items.c), one item a row with its
 * own inventory icon (base record +0 iconId - 1, as Menu_DrawSlotRow draws
 * it) and name, and its game description in the title bar. View only. Every
 * row past the menu's last action draws action 9 (Menu_DrawItemSlot stops at
 * index 9), so the order is: ..., Mod Catalog, Key Items, Distribute Bonus
 * Points (the BP row is handed to the game as its own row); the row's icon is
 * the Dressing Room Key's.
 *
 * Slots show the game's own mod badges (sprites 0x22 + effect for guns,
 * 0x36 + effect for armor, 0x46 empty; 32x12), written into the frame's
 * packet arena as Draw_AllocSprite does. Our economy armor effects (17-20)
 * have no badge art, so they get a text badge in the same 32x12 spot: a
 * framed box (two flat rectangles) and a short label ("Barg", "Scav",
 * "Luck", "Salv") in the menu font, its glyphs built as
 * Draw_AllocTexturedQuad builds them, one pixel tighter. Sounds come from the game: the focused
 * list keeps a dummy swap mark one column to the right of its cursor, so
 * Menu_StepScrollCursor treats Cross as a swap and calls the list's
 * itemAction -- MenuWidget_GetCurrentNode (nonzero: the confirm sound) when
 * the row can be picked, Battle_IsActiveWrapped (0 outside battle: the error
 * sound) when not -- and treats Circle as dropping the mark (the cancel
 * sound, the cursor left in that column, which is how we see it). The call
 * is the Cross press we act on.
 *
 * Vanilla screens draw an armor mod's badge as 0x36 + effect too
 * (Menu_DrawActionCodeItem), which for 17-20 is Aya's portrait and other
 * sprites: those draw the blank sprite 0x79 and our text badge instead.
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
#define PE_FN_DRAW_TEXTBOXES  0x80037870u   /* Menu_DrawTextboxEntries: once a frame */
#define PE_FN_ALLOC_SPRITE    0x8005EB64u   /* Draw_AllocSprite */
#define PE_FN_OPTION_LIST     0x8004F950u   /* Menu_DrawActionOptionList(node) */
#define PE_FN_OPTION_ROW      0x800509A8u   /* Menu_DrawActionOptionRow(row) */
#define PE_FN_CENTERED_TEXT   0x8005F354u   /* Draw_PrintCenteredTextInWidth(text, w) */
#define PE_FN_HELP_TEXT       0x8004C608u   /* Menu_DrawContextHelpText: the title bar */
#define PE_FN_STEP_SCROLL     0x80063E0Cu   /* Menu_StepScrollCursor: a list's update */
#define PE_FN_STEP_SCROLL_END 0x800647D0u   /* (Draw_SetPrimCallback follows it) */
#define PE_FN_SAY_YES         0x80062CC4u   /* MenuWidget_GetCurrentNode: nonzero */
#define PE_FN_SAY_NO          0x80052534u   /* Battle_IsActiveWrapped: 0 in the field */
#define PE_FN_CODE_ITEM       0x80050ADCu   /* Menu_DrawActionCodeItem(row): a mod badge */
#define PE_ACTION_ITEM_DATA   0x8009CF20u   /* g_MenuActionItemData */
#define PE_ITEM_IS_GUN        0x8009CF18u   /* g_InvItemUsableFlag */
#define PE_SPRITE_BLANK       0x79u         /* zero-sized */
#define PE_SPRITE_GUN_MODS    0x22u
#define PE_SPRITE_ARMOR_MODS  0x36u
#define PE_SPRITE_NO_MOD      0x46u
#define PE_PACKET_CURSOR      0x8009D100u   /* g_DrawPacketCursor */
#define PE_PACKET_ARENA       0x8009D104u   /* g_DrawPacketArenaBase (0x4000 bytes) */
#define PE_ORDERING_ENTRY     0x8009D11Cu   /* g_DrawOrderingTableEntry */
#define PE_TEXT_X             0x8009D124u   /* g_TextCursorX */
#define PE_TEXT_Y             0x8009D128u
#define PE_GLYPH_DESC_SLOT    0x800A8030u   /* resident directory +0x08 */
#define PE_MENU_FONT_CLUT     0x089Cu       /* Draw_AllocTexturedQuad's font */
#define PE_MENU_FONT_TPAGE    0x0005u       /* GetTPage(0, 0, 0x140, 0) */
#define PE_BADGE_FRAME        0x00707070u   /* text badge colours (BBGGRR) */
#define PE_BADGE_FILL         0x00402814u
#define PE_SPRITE_BP          0x5Eu         /* action 8's icon */
#define PE_SPRITE_ROW9        0x5Fu         /* what our row draws */
#define PE_ACTIVE_WIDGET      0x8009CEF4u   /* g_MenuActiveWidget: list being drawn */
#define PE_WIDGET_LIST_HEAD   0x8009D154u   /* g_MenuWidgetActiveListHead */
#define PE_WIDGET_FREE_HEAD   0x8009D158u   /* g_MenuWidgetFreeListHead */
#define PE_CURRENT_NODE       0x8009D15Cu   /* g_MenuWidgetCurrentNode */
#define PE_STATUS_OVERLAY     0x8009CEF8u   /* g_MenuBattleStatusOverlayActive */
#define PE_DRAW_ITEM_LIST     0x8004F838u   /* Menu_DrawItemList: the main list's draw */
#define PE_CONTEXT_FLAGS      0x8009CEF0u   /* g_MenuItemContextFlag */
#define PE_BATTLE_EQUIP_MODE  0x8009D028u   /* g_MenuBattleEquipMode */
#define PE_TEXT_DIMMED        0x8009D10Cu   /* menu text: dimmed colour on */
#define PE_TEXT_COLOR         0x8009D110u   /* menu text colour */
#define PE_TEXT_COLOR_DIM     0x8009D114u   /* its dimmed colour */
#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_AYA_SLOTS_ADDR     0x800C0E48u
#define PE_STORAGE_ADDR       0x800C1EB8u
#define PE_GLYPH_METRICS_SLOT 0x800A8050u   /* resident directory +0x28 */
#define PE_TITLE_ACTION9      0x31u         /* Str_LookupTable10 index row 9 would show */
#define PE_TITLE_BP           0x30u         /* "Distribute Bonus Points" */
#define PE_HEADER_COLOR       0x00308CA8u   /* yellow, as the menus' labels */
#define PE_ITEM_TABLE_BASE    0x800A8034u   /* Item_LookupBaseData: base records, 0x20 each */
#define PE_KEY_ROW_ITEM       0xC8u         /* Dressing Room Key: the Key Items row's icon */
#define PE_KEY_ICON_INDENT    "     "        /* room for an item icon before the name */
enum { MODE_CATALOG, MODE_KEYS };
#define PE_FN_DRAW_ITEM_LABEL 0x80044E14u   /* Menu_DrawItemLabel: a dialog's text */
#define PE_FN_CONFIRM_DIALOG  0x80044E98u   /* Menu_ConfirmDialogHandler */
#define PE_FN_CONFIRM_DIALOG_END 0x80044F8Cu
#define PE_ACTION_TEXT_BASE   0x8009CF14u   /* g_MenuActionTextBase: 5 = OK / Cancel */
#define PE_CONFIRM_CALLBACK   0x8009CFA8u   /* g_MenuConfirmCallback */
#define PE_ITEM_LABEL_BUF     0x800A1980u   /* g_MenuItemLabelBuffer (2 x 0x40) */
#define PE_ITEM_PRIM_HANDLES  0x8009CFA0u   /* g_MenuItemPrimListHandles */
#define PE_SIMPLE_DESC        0x80092478u   /* g_MenuWidgetSimpleDescriptors, 0x10 each */
#define PE_GRID_DESC          0x80092888u   /* g_MenuWidgetGridDescriptors, 0x20 each */
#define PE_DIALOG             0x29u         /* the yes/no dialog's descriptors */
#define PE_DLG_LINES          5

/* MenuWidgetNode */
#define N_NEXT        0x00u
#define N_PARENT      0x04u
#define N_CHILD0      0x08u
#define N_X           0x18u
#define N_Y           0x1Cu
#define N_MODE        0x20u
#define N_SELECTED    0x24u
#define N_UPDATE      0x2Cu
#define N_DRAW        0x30u
#define N_GRID_W      0x34u
#define N_ROWS        0x38u                 /* visible rows; a panel's height */
#define N_CELL_W      0x3Cu                 /* draw_state: a list's column width */
#define N_ROW_H       0x40u                 /* "disabled": a list's row height */
#define N_CURSOR_X    0x44u
#define N_CURSOR_Y    0x48u                 /* a panel: 1 = hidden */
#define N_TARGET_X    0x4Cu
#define N_TARGET_Y    0x50u
#define N_X_LIMIT     0x54u
#define N_Y_LIMIT     0x58u
#define N_SCROLL_Y    0x5Cu
#define N_SCROLL_ADJ  0x60u
#define N_LAYOUT      0x64u
#define N_ROW_CAP     0x6Cu                 /* visible_rows_mirror */
#define N_AUX         0x70u
#define N_ITEM_ACTION 0x84u
#define N_SIZE        0x90u
#define PE_OUR_PANEL  0x3Fu                 /* selected_base of our panels */

/* Input bits the menu queue hands to widgets. */
#define IN_CONFIRM 0x10000u
#define IN_CANCEL  0x40u
#define IN_LEFT    0x8000u
#define IN_RIGHT   0x2000u

#define PE_HIDDEN_MAX 4
#define PE_ROWS_MAX   40
#define PE_ROW_H      16

enum { P_SLOTS, P_EFFECTS, P_INFO, P_COUNT };
enum { WHICH_GUN = 0, WHICH_ARMOR = 1 };
enum { R_NONE, R_HEADER, R_SLOT, R_NOSLOTS, R_REMOVE, R_EFFECT, R_LOCKED, R_TEXT };

typedef struct { uint8_t kind, which; int8_t slot; uint8_t effect; } Row;

typedef struct { int x, y, w, h, rows; } PeLayout;
static const PeLayout s_layout_keys_list = { 32, 32, 280, 132, 8 };
static const PeLayout s_layout[P_COUNT] = {
    { 32, 32, 116, 132, 8 },                /* slots: where Aya's status panel was */
    { 152, 32, 160, 132, 8 },               /* effects */
    { 32, 168, 280, 36, 2 },                /* info */
};

static struct {
    int open;
    int mode;                                 /* MODE_CATALOG / MODE_KEYS */
    uint32_t panel[P_COUNT];
    uint32_t list[P_COUNT];
    uint32_t main_list;                       /* to go back to */
    int focus;                                /* P_SLOTS / P_EFFECTS */
    int prev_slot_row;                        /* for skipping the name rows */
    uint32_t pending;                         /* buttons for next frame */
    int note_effects_row;                     /* the note belongs to this row */
    int confirm;                              /* the install dialog is up */
    int confirm_row;                          /* ... for the effect on this row */
    int confirm_effect;
    uint32_t dlg, dlg_list;                   /* its nodes */
    char note[48];
    uint32_t hidden[PE_HIDDEN_MAX];           /* status panels we hid */
    uint32_t hidden_selected[PE_HIDDEN_MAX];
    uint32_t overlay;                         /* HP overlay flag before */
} s_cm;

static uint32_t s_bp_restore_list;            /* BP confirm: put its cursor back */
static uint32_t s_bp_restore_row;
static uint32_t s_title;                      /* "Mod Catalog", mod memory */
static uint32_t s_rowbuf;                     /* the row being printed, mod memory */
static uint32_t s_descbuf;                    /* the title bar text, mod memory */
static uint32_t s_drawing;                    /* our list being drawn */
static int s_drawing_row = -1;                /* its row about to print */
static int s_help_pending;
static int s_blank_badge;                     /* the next sprite is an econ badge */
static int s_color_saved;
static uint32_t s_saved_color, s_saved_color_dim;
static PeText s_dlg_text[PE_DLG_LINES];       /* the dialog's lines after the first */
static int s_dlg_lines;
static uint32_t s_is_this_ok;                 /* the game's "Is this OK?" (table 4) */

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

/* Rows we add after the game's: Mod Catalog, and Key Items while the pocket is on. */
static int pe_extra_rows(void) { return pe_keys_enabled() ? 2 : 1; }

static int pe_has_our_row(uint32_t list) {
    return (int)psx_mod_read_word(list + N_Y_LIMIT) == pe_vanilla_rows() + pe_extra_rows();
}

/* Our row sits above Distribute Bonus Points when that is on the menu. */
static int pe_swapped(void) { return (psx_mod_read_word(PE_CONTEXT_FLAGS) >> 8) & 1u; }

static int pe_catalog_row(void) { return pe_vanilla_rows() - (pe_swapped() ? 1 : 0); }

/* What a main menu row is, with our rows in. With BP on the menu (its last
 * action): ..., [base-1] Mod Catalog, [base] Key Items, [last] BP. */
enum { ROLE_VANILLA, ROLE_CATALOG, ROLE_KEYS, ROLE_BP };
static int pe_row_role(int row) {
    const int base = pe_vanilla_rows(), extra = pe_extra_rows();
    if (row == pe_catalog_row()) return ROLE_CATALOG;
    if (pe_swapped()) {
        if (extra == 2 && row == base) return ROLE_KEYS;
        if (row == base + extra - 1) return ROLE_BP;
    } else if (extra == 2 && row == base + 1) {
        return ROLE_KEYS;
    }
    return ROLE_VANILLA;
}

/* An item's inventory icon (Menu_DrawSlotRow: Draw_AllocSprite(iconId - 1)). */
static uint32_t pe_item_icon_sprite(uint16_t id) {
    const uint32_t base = psx_mod_read_word(PE_ITEM_TABLE_BASE);
    if (!id || !base) return PE_SPRITE_ROW9;
    const uint32_t icon = psx_mod_read_byte(PE_STR_ORIGIN + base + (uint32_t)(id - 1u) * 0x20u);
    return icon ? icon - 1u : PE_SPRITE_ROW9;
}

static int pe_cursor_row(uint32_t list) {
    return (int)psx_mod_read_word(list + N_GRID_W) * (int)psx_mod_read_word(list + N_CURSOR_Y)
         + (int)psx_mod_read_word(list + N_CURSOR_X);
}

/* Is our row on `list` and under its cursor? */
static int pe_on_our_row(uint32_t list) {
    return pe_has_our_row(list) && pe_cursor_row(list) == pe_catalog_row();
}

/* Row icons: with the swap, the BP row shows action 9's icon spot and ours
 * the BP spot -- trade the two sprites while the main list draws. */
static void pe_on_alloc_sprite(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (s_blank_badge) {
        s_blank_badge = 0;
        if (cpu->gpr[4] > PE_SPRITE_NO_MOD) { cpu->gpr[4] = PE_SPRITE_BLANK; return; }
    }
    static int s_extra_seen;                  /* action-9 rows drawn since a vanilla row */
    const uint32_t sprite = cpu->gpr[4];
    if (sprite < 0x56u || sprite > PE_SPRITE_ROW9) return;
    const uint32_t list = psx_mod_read_word(PE_ACTIVE_WIDGET);
    if (!pe_ram_pointer(list) || psx_mod_read_word(list + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (!pe_has_our_row(list)) return;
    if (sprite != PE_SPRITE_ROW9) {           /* a vanilla row's icon */
        s_extra_seen = 0;
        if (sprite == PE_SPRITE_BP && pe_swapped()) cpu->gpr[4] = PE_SPRITE_ROW9;   /* BP spot: Catalog */
        return;
    }
    const int n = ++s_extra_seen;             /* 1st / 2nd row past the game's last */
    const int keys = pe_extra_rows() == 2;
    if (pe_swapped()) {
        if (n == 1) cpu->gpr[4] = keys ? pe_item_icon_sprite(PE_KEY_ROW_ITEM) : PE_SPRITE_BP;
        else if (n == 2) cpu->gpr[4] = PE_SPRITE_BP;
    } else if (n == 2 && keys) {
        cpu->gpr[4] = pe_item_icon_sprite(PE_KEY_ROW_ITEM);
    }
}

static void pe_on_set_rows(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t node = cpu->gpr[4];
    if (!pe_ram_pointer(node) || psx_mod_read_word(node + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (pe_loaded() && psx_mod_read_word(PE_BATTLE_EQUIP_MODE) == 0 && pe_bag_sort_hide_row())
        cpu->gpr[5] -= 1;                     /* Sort lives in Use Item now (pe_bag_sort.c) */
    if (!pe_row_wanted()) return;
    const int rows = pe_vanilla_rows();
    if ((int)cpu->gpr[5] != rows) return;
    const int extra = pe_extra_rows();
    if ((int)psx_mod_read_word(node + N_ROW_CAP) < rows + extra)
        psx_mod_write_word(node + N_ROW_CAP, (uint32_t)(rows + extra));
    cpu->gpr[5] = (uint32_t)(rows + extra);
}

/* ---- text ----------------------------------------------------------------------- */
static uint32_t pe_mem(uint32_t *slot, uint32_t size) {
    if (!*slot) *slot = psx_mod_alloc_guest_memory(size, 4);
    return *slot;
}

static void pe_put(uint32_t dst, const PeText *t) {
    for (unsigned i = 0; i < t->n; ++i) psx_mod_write_byte(dst + i, t->b[i]);
    psx_mod_write_byte(dst + t->n, G_END);
}

/* Width of glyph text in the menu font, as Draw_MeasureTextWidth. */
static int pe_text_width(const uint8_t *b, unsigned n) {
    const uint32_t metrics = PE_STR_ORIGIN + psx_mod_read_word(PE_GLYPH_METRICS_SLOT);
    int w = 0, page = 0;
    for (unsigned i = 0; i < n; ++i) {
        int g = b[i];
        if (g >= 0xFA) { page = g - 0xFA; continue; }
        if (page) { g += page << 8; page = 0; }
        const int spacing = (g < 10 || g == 15) ? 1 : 0;
        if (g >= 0x100) g -= 0x13;
        w += ((psx_mod_read_byte(metrics + (uint32_t)g) >> 4) & 15) + spacing + 1;
    }
    return w;
}

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

/* The game's own description of an effect (pe_custom.c relabels 17-20). */
static uint32_t pe_fx_description(int which, int e) {
    if (e <= 0) return 0;
    return pe_text_entry(0x10u, (unsigned)(which == WHICH_GUN ? e - 1 : e + 19));
}

/* Our text badges for the economy armor effects. */
static const char *pe_text_badge(int which, int e) {
    if (which != WHICH_ARMOR) return 0;
    switch (e) {
    case 17: return "Barg";
    case 18: return "Scav";
    case 19: return "Luck";
    case 20: return "Salv";
    default: return 0;
    }
}

/* The game's badge for an effect, or 0 (our economy armor effects). */
static uint32_t pe_badge(int which, int e) {
    if (e <= 0) return PE_SPRITE_NO_MOD;
    if (which == WHICH_GUN) return e <= 20 ? PE_SPRITE_GUN_MODS + (uint32_t)e : 0;
    return e <= 15 ? PE_SPRITE_ARMOR_MODS + (uint32_t)e : 0;
}
/* ---- rows ----------------------------------------------------------------------- */
static int pe_slot_rows(Row *out, int max) {
    int n = 0;
    for (int which = WHICH_GUN; which <= WHICH_ARMOR; ++which) {
        if (n < max) out[n++] = (Row){ R_HEADER, (uint8_t)which, -1, 0 };
        const int slots = pe_cat_record(which) ? pe_cat_slots(which) : 0;
        if (pe_cat_record(which) && slots <= 0 && n < max) out[n++] = (Row){ R_NOSLOTS, (uint8_t)which, -1, 0 };
        for (int s = 0; s < slots && n < max; ++s) out[n++] = (Row){ R_SLOT, (uint8_t)which, (int8_t)s, 0 };
    }
    return n;
}

static Row pe_slot_row(int i) {
    Row rows[PE_ROWS_MAX];
    const int n = pe_slot_rows(rows, PE_ROWS_MAX);
    return i >= 0 && i < n ? rows[i] : (Row){ R_NONE, 0, -1, 0 };
}

/* The slot under the slot panel's cursor. */
static Row pe_chosen_slot(void) {
    if (!s_cm.open) return (Row){ R_NONE, 0, -1, 0 };
    return pe_slot_row((int)psx_mod_read_word(s_cm.list[P_SLOTS] + N_CURSOR_Y));
}

static int pe_effect_rows(Row *out, int max) {
    const Row s = pe_chosen_slot();
    int n = 0;
    if (s.kind != R_SLOT) return 0;
    if (pe_cat_remove(s.which, s.slot, 1) == PE_CAT_OK && n < max)
        out[n++] = (Row){ R_REMOVE, s.which, s.slot, 0 };
    const int count = pe_cat_board_count(s.which);
    for (int pass = 0; pass < 2; ++pass)        /* unlocked first, then the unknown */
        for (int i = 0; i < count && n < max; ++i) {
            const int e = pe_cat_board_effect(s.which, i);
            if (pe_cat_unlocked(s.which, e) == !pass)
                out[n++] = (Row){ (uint8_t)(pass ? R_LOCKED : R_EFFECT), s.which, s.slot, (uint8_t)e };
        }
    return n;
}

static Row pe_effect_row(int i) {
    Row rows[PE_ROWS_MAX];
    const int n = pe_effect_rows(rows, PE_ROWS_MAX);
    return i >= 0 && i < n ? rows[i] : (Row){ R_NONE, 0, -1, 0 };
}

static int pe_row_count(int panel) {
    Row rows[PE_ROWS_MAX];
    if (s_cm.mode == MODE_KEYS) return panel == P_SLOTS ? (pe_keys_count() ? pe_keys_count() : 1) : 2;
    if (panel == P_SLOTS) return pe_slot_rows(rows, PE_ROWS_MAX);
    if (panel == P_EFFECTS) return pe_effect_rows(rows, PE_ROWS_MAX);
    return 2;
}

static void pe_count_tools(int *tools, int *kit) {
    *tools = 0;
    *kit = 0;
    for (uint32_t i = 0; i < 150u; ++i) {
        const uint32_t a = i < 50u ? PE_AYA_SLOTS_ADDR + i * 2u : PE_STORAGE_ADDR + (i - 50u) * 2u;
        const uint16_t v = psx_mod_read_half(a);
        if (v == 0x16) ++*tools;
        if (v == 0x35 || v == 0x36) *kit = 1;
    }
}

/* Why the effect row can't be picked now ("" if it can). */
static const char *pe_effect_blocker(Row r, char *buf, size_t size) {
    if (r.kind == R_LOCKED) return "Not found yet";
    if (r.kind != R_EFFECT) return "";
    switch (pe_cat_install(r.which, r.slot, r.effect, 1)) {
    case PE_CAT_OK: return "";
    case PE_CAT_JUNK:
        snprintf(buf, size, "Need %u Junk", (unsigned)pe_cat_price(r.which, r.effect));
        return buf;
    case PE_CAT_TOOL: return "Needs a Tool";
    case PE_CAT_ALREADY: return "Already in this slot";
    case PE_CAT_BAG: return "Capacity mods stay put";
    default: return "Can't install now";
    }
}

/* Row text: left part, right part (prices), dimmed, header colour. */
static void pe_row_text(int panel, int i, PeText *left, PeText *right, int *dim, int *header) {
    char buf[48];
    *dim = 0;
    *header = 0;
    if (s_cm.mode == MODE_KEYS) {
        if (panel == P_SLOTS) {
            const uint16_t id = pe_keys_item(i);
            if (id) { pe_t_str(left, PE_KEY_ICON_INDENT); pe_t_item(left, id); }
            else { pe_t_str(left, "No key items"); *dim = 1; }
        } else if (i == 0) {
            pe_t_str(left, "Key items ");
            pe_t_num(left, (unsigned)pe_keys_count());
        } else {
            pe_t_str(left, "Kept out of your bag");
        }
        return;
    }
    if (panel == P_SLOTS) {
        const Row r = pe_slot_row(i);
        switch (r.kind) {
        case R_HEADER: {
            const uint32_t rec = pe_cat_record(r.which);
            if (rec) { pe_t_item(left, psx_mod_read_byte(rec + 4u)); *header = 1; }
            else { pe_t_str(left, r.which == WHICH_GUN ? "No gun" : "No armor"); *dim = 1; }
            break;
        }
        case R_NOSLOTS: pe_t_str(left, " No mod slots"); *dim = 1; break;
        case R_SLOT: {                        /* " 1" and its badge, drawn after */
            const int e = pe_cat_slot_effect(r.which, r.slot);
            pe_t_str(left, " ");
            pe_t_num(left, (unsigned)r.slot + 1u);
            *dim = pe_cat_is_bag_mod(r.which, e);
            break;
        }
        default: break;
        }
    } else if (panel == P_EFFECTS) {
        const Row r = pe_effect_row(i);
        switch (r.kind) {
        case R_REMOVE: pe_t_str(left, "Remove"); break;
        case R_EFFECT:
            pe_t_str(left, pe_fx_name(r.which, r.effect));
            if (pe_cat_slot_effect(r.which, r.slot) == r.effect) {
                pe_t_str(right, "In");
            } else {
                pe_t_num(right, (unsigned)pe_cat_price(r.which, r.effect));
                if (pe_cat_needs_tool(r.which, r.effect)) pe_t_str(right, " T");
            }
            *dim = pe_effect_blocker(r, buf, sizeof buf)[0] != '\0';
            break;
        case R_LOCKED: pe_t_str(left, "Unknown"); *dim = 1; break;
        default: break;
        }
    } else if (i == 0) {
        int tools, kit;
        pe_count_tools(&tools, &kit);
        pe_t_str(left, "Junk ");
        pe_t_num(left, (unsigned)pe_loot_junk());
        if (kit) pe_t_str(right, "Tool Kit");
        else { pe_t_str(right, "Tools "); pe_t_num(right, (unsigned)tools); }
    } else {
        const int row = (int)psx_mod_read_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y);
        if (s_cm.note[0] && (s_cm.focus != P_EFFECTS || s_cm.note_effects_row == row)) {
            pe_t_str(left, s_cm.note);
        } else if (s_cm.focus == P_EFFECTS) {
            const Row r = pe_effect_row(row);
            const char *why = pe_effect_blocker(r, buf, sizeof buf);
            if (why[0]) pe_t_str(left, why);
            else if (r.kind == R_REMOVE) pe_t_str(left, "Free, it stays unlocked");
            else if (r.kind == R_EFFECT) {
                pe_t_str(left, "Costs ");
                pe_t_num(left, (unsigned)pe_cat_price(r.which, r.effect));
                pe_t_str(left, pe_cat_needs_tool(r.which, r.effect) ? " Junk and a Tool" : " Junk");
            }
        } else {
            const Row s = pe_chosen_slot();
            if (s.kind == R_SLOT && pe_cat_is_bag_mod(s.which, pe_cat_slot_effect(s.which, s.slot)))
                pe_t_str(left, "Capacity mods stay put");
            else if (s.kind == R_SLOT) pe_t_str(left, "Choose an effect for this slot");
        }
    }
}

/* ---- sprites ------------------------------------------------------------------------ */
/* Sprite `index` at (x, y), as Draw_AllocSprite builds it: a textured
 * rectangle and its texture page, linked into the current ordering entry. */
static void pe_emit_sprite(uint32_t index, uint32_t x, uint32_t y, int dim) {
    const uint32_t desc = PE_STR_ORIGIN + psx_mod_read_word(PE_GLYPH_DESC_SLOT) + index * 8u;
    const uint32_t cursor = psx_mod_read_word(PE_PACKET_CURSOR);
    const uint32_t arena = psx_mod_read_word(PE_PACKET_ARENA);
    const uint32_t ot = psx_mod_read_word(PE_ORDERING_ENTRY);
    if (!pe_ram_pointer(cursor) || !pe_ram_pointer(ot) || cursor + 28u >= arena + 0x4000u) return;
    const uint32_t color = psx_mod_read_word(dim ? PE_TEXT_COLOR_DIM : PE_TEXT_COLOR);
    psx_mod_write_word(cursor + 4u, (color & 0x00FFFFFFu) | 0x64000000u);
    psx_mod_write_half(cursor + 8u, (uint16_t)x);
    psx_mod_write_half(cursor + 10u, (uint16_t)y);
    psx_mod_write_byte(cursor + 12u, psx_mod_read_byte(desc + 0u));
    psx_mod_write_byte(cursor + 13u, psx_mod_read_byte(desc + 1u));
    psx_mod_write_half(cursor + 14u, psx_mod_read_half(desc + 2u));
    psx_mod_write_half(cursor + 16u, psx_mod_read_byte(desc + 4u));
    psx_mod_write_half(cursor + 18u, psx_mod_read_byte(desc + 5u));
    uint32_t link = psx_mod_read_word(ot);
    psx_mod_write_word(cursor, 0x04000000u | (link & 0x00FFFFFFu));
    link = (link & 0xFF000000u) | (cursor & 0x00FFFFFFu);
    const uint32_t page = cursor + 20u;     /* drawn first: linked last */
    psx_mod_write_word(page, 0x01000000u | (link & 0x00FFFFFFu));
    psx_mod_write_word(page + 4u, 0xE1000000u | ((psx_mod_read_byte(desc + 6u) & 3u) << 7) | 7u);
    psx_mod_write_word(ot, (link & 0xFF000000u) | (page & 0x00FFFFFFu));
    psx_mod_write_word(PE_PACKET_CURSOR, cursor + 28u);
}


/* Link a packet of `words` words (tag included) into the current ordering
 * entry; returns its address, or 0 when the arena is full. */
static uint32_t pe_packet(uint32_t words) {
    const uint32_t cursor = psx_mod_read_word(PE_PACKET_CURSOR);
    const uint32_t arena = psx_mod_read_word(PE_PACKET_ARENA);
    const uint32_t ot = psx_mod_read_word(PE_ORDERING_ENTRY);
    if (!pe_ram_pointer(cursor) || !pe_ram_pointer(ot) || cursor + words * 4u >= arena + 0x4000u) return 0;
    const uint32_t link = psx_mod_read_word(ot);
    psx_mod_write_word(cursor, ((words - 1u) << 24) | (link & 0x00FFFFFFu));
    psx_mod_write_word(ot, (link & 0xFF000000u) | (cursor & 0x00FFFFFFu));
    psx_mod_write_word(PE_PACKET_CURSOR, cursor + words * 4u);
    return cursor;
}

static void pe_emit_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    const uint32_t p = pe_packet(4);
    if (!p) return;
    psx_mod_write_word(p + 4u, (color & 0x00FFFFFFu) | 0x60000000u);  /* TILE */
    psx_mod_write_half(p + 8u, (uint16_t)x);
    psx_mod_write_half(p + 10u, (uint16_t)y);
    psx_mod_write_half(p + 12u, (uint16_t)w);
    psx_mod_write_half(p + 14u, (uint16_t)h);
}

/* One menu-font glyph at (x, y); returns its advance, one pixel tighter. */
static int pe_emit_glyph(uint8_t glyph, uint32_t x, uint32_t y, uint32_t color) {
    const uint32_t metrics = PE_STR_ORIGIN + psx_mod_read_word(PE_GLYPH_METRICS_SLOT);
    const int m = psx_mod_read_byte(metrics + glyph);
    const int width = (m >> 4) & 15, left = m & 15;
    const int spacing = (glyph < 10 || glyph == 15) ? 2 : 1;
    if (glyph != 0x0F) {
        const uint32_t p = pe_packet(10);
        if (!p) return width + spacing - 1;
        const uint32_t u = (uint32_t)((glyph % 21) * 12 + left), v = (uint32_t)((glyph / 21) * 12);
        const uint32_t x0 = x + (uint32_t)(spacing >> 1), y0 = y + 1u;
        psx_mod_write_word(p + 4u, (color & 0x00FFFFFFu) | 0x2C000000u);  /* POLY_FT4 */
        const uint32_t xs[4] = { x0, x0 + (uint32_t)width, x0, x0 + (uint32_t)width };
        const uint32_t ys[4] = { y0, y0, y0 + 12u, y0 + 12u };
        const uint32_t us[4] = { u, u + (uint32_t)width, u, u + (uint32_t)width };
        const uint32_t vs[4] = { v, v, v + 12u, v + 12u };
        for (uint32_t k = 0; k < 4; ++k) {
            psx_mod_write_half(p + 8u + k * 8u, (uint16_t)xs[k]);
            psx_mod_write_half(p + 10u + k * 8u, (uint16_t)ys[k]);
            psx_mod_write_byte(p + 12u + k * 8u, (uint8_t)us[k]);
            psx_mod_write_byte(p + 13u + k * 8u, (uint8_t)vs[k]);
        }
        psx_mod_write_half(p + 14u, PE_MENU_FONT_CLUT);
        psx_mod_write_half(p + 22u, PE_MENU_FONT_TPAGE);
    }
    return width + spacing - 1;
}

/* A text badge in a 32x12 badge spot at (x, y). Packets draw in the reverse
 * of linking, so the label goes in first and the frame last. */
static void pe_emit_text_badge(const char *label, uint32_t x, uint32_t y, int dim) {
    uint8_t g[8];
    unsigned n = 0;
    int width = 0;
    for (; label[n] && n < sizeof g; ++n) {
        g[n] = pe_glyph(label[n]);
        const int m = psx_mod_read_byte(PE_STR_ORIGIN + psx_mod_read_word(PE_GLYPH_METRICS_SLOT) + g[n]);
        width += ((m >> 4) & 15) + ((g[n] < 10 || g[n] == 15) ? 2 : 1) - 1;
    }
    const uint32_t text = psx_mod_read_word(dim ? PE_TEXT_COLOR_DIM : PE_TEXT_COLOR);
    const uint32_t shade = dim ? 1u : 0u;
    uint32_t cx = x + (uint32_t)((32 - width) / 2);
    for (unsigned i = 0; i < n; ++i) cx += (uint32_t)pe_emit_glyph(g[i], cx, y - 1u, text);
    pe_emit_rect(x + 1u, y + 1u, 30u, 10u, (PE_BADGE_FILL >> shade) & 0x7F7F7Fu);
    pe_emit_rect(x, y, 32u, 12u, (PE_BADGE_FRAME >> shade) & 0x7F7F7Fu);
}

/* ---- the widget pool -------------------------------------------------------------- */
static int pe_free_nodes(void) {
    int n = 0;
    for (uint32_t p = psx_mod_read_word(PE_WIDGET_FREE_HEAD); pe_ram_pointer(p) && n < 64;
         p = psx_mod_read_word(p + N_NEXT))
        ++n;
    return n;
}

static uint32_t pe_node_alloc(void) {
    const uint32_t n = psx_mod_read_word(PE_WIDGET_FREE_HEAD);
    if (!pe_ram_pointer(n)) return 0;
    psx_mod_write_word(PE_WIDGET_FREE_HEAD, psx_mod_read_word(n + N_NEXT));
    for (uint32_t i = 0; i < N_SIZE; i += 4) psx_mod_write_word(n + i, 0);
    psx_mod_write_word(n + N_NEXT, psx_mod_read_word(PE_WIDGET_LIST_HEAD));
    psx_mod_write_word(PE_WIDGET_LIST_HEAD, n);
    return n;
}

/* Back to the free list, as MenuWidget_DestroyNodeRecursive (its own fields
 * stay, so a caller still walking it is safe). */
static void pe_node_free(uint32_t node) {
    uint32_t prev = 0, cur = psx_mod_read_word(PE_WIDGET_LIST_HEAD);
    for (int guard = 0; pe_ram_pointer(cur) && cur != node && guard < 64; ++guard) {
        prev = cur;
        cur = psx_mod_read_word(cur + N_NEXT);
    }
    if (cur != node || !pe_ram_pointer(cur)) return;
    const uint32_t next = psx_mod_read_word(cur + N_NEXT);
    if (prev) psx_mod_write_word(prev + N_NEXT, next);
    else psx_mod_write_word(PE_WIDGET_LIST_HEAD, next);
    psx_mod_write_word(cur + N_NEXT, psx_mod_read_word(PE_WIDGET_FREE_HEAD));
    psx_mod_write_word(PE_WIDGET_FREE_HEAD, cur);
}

static const PeLayout *pe_lay(int p) {
    return s_cm.mode == MODE_KEYS && p == P_SLOTS ? &s_layout_keys_list : &s_layout[p];
}

static int pe_build_panel(int p) {
    const PeLayout *L = pe_lay(p);
    const uint32_t panel = pe_node_alloc();
    if (!panel) return 0;
    psx_mod_write_word(panel + N_MODE, 1);
    psx_mod_write_word(panel + N_SELECTED, PE_OUR_PANEL);
    psx_mod_write_word(panel + N_X, (uint32_t)L->x);
    psx_mod_write_word(panel + N_Y, (uint32_t)L->y);
    psx_mod_write_word(panel + N_GRID_W, (uint32_t)L->w);
    psx_mod_write_word(panel + N_ROWS, (uint32_t)L->h);
    if (p != P_INFO) psx_mod_write_word(panel + N_UPDATE, PE_FN_ITEM_SELECT);
    s_cm.panel[p] = panel;

    const uint32_t list = pe_node_alloc();
    if (!list) return 0;
    psx_mod_write_word(list + N_PARENT, panel);
    psx_mod_write_word(panel + N_CHILD0, list);
    psx_mod_write_word(list + N_MODE, 2);
    psx_mod_write_word(list + N_SELECTED, 0);  /* the title bar reads it */
    psx_mod_write_word(list + N_AUX, PE_OUR_PANEL);
    psx_mod_write_word(list + N_X, 2);
    psx_mod_write_word(list + N_Y, 2);
    psx_mod_write_word(list + N_UPDATE, PE_FN_STEP_SCROLL);
    psx_mod_write_word(list + N_DRAW, PE_FN_OPTION_LIST);
    psx_mod_write_word(list + N_GRID_W, 1);
    psx_mod_write_word(list + N_X_LIMIT, 1);
    psx_mod_write_word(list + N_ROWS, (uint32_t)L->rows);
    psx_mod_write_word(list + N_ROW_CAP, (uint32_t)L->rows);
    psx_mod_write_word(list + N_Y_LIMIT, 1);
    psx_mod_write_word(list + N_CELL_W, (uint32_t)(L->w - 4));
    psx_mod_write_word(list + N_ROW_H, PE_ROW_H);
    psx_mod_write_word(list + N_CURSOR_X, p == P_SLOTS ? 0u : 0xFFFFFFFFu);
    psx_mod_write_word(list + N_TARGET_X, 0xFFFFFFFFu);
    psx_mod_write_word(list + N_TARGET_Y, 0xFFFFFFFFu);
    psx_mod_write_word(list + N_LAYOUT, 0x13u);  /* wrap; Left/Right go to the panel */
    psx_mod_write_word(list + N_ITEM_ACTION, PE_FN_SAY_NO);
    s_cm.list[p] = list;
    return 1;
}

/* Rows changed: the list's length, cursor and scroll follow. */
static void pe_fit_list(int p) {
    const uint32_t list = s_cm.list[p];
    if (!list) return;                        /* the Key Items screen has no effects list */
    int count = pe_row_count(p);
    if (count < 1) count = 1;
    psx_mod_write_word(list + N_Y_LIMIT, (uint32_t)count);
    int cursor = (int)psx_mod_read_word(list + N_CURSOR_Y);
    if (cursor >= count) cursor = count - 1;
    if (cursor < 0) cursor = 0;
    psx_mod_write_word(list + N_CURSOR_Y, (uint32_t)cursor);
    const int rows = pe_lay(p)->rows;
    int scroll = (int)psx_mod_read_word(list + N_SCROLL_Y);
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + rows) scroll = cursor - rows + 1;
    if (scroll > count - rows) scroll = count - rows;
    if (scroll < 0) scroll = 0;
    psx_mod_write_word(list + N_SCROLL_Y, (uint32_t)scroll);
}

static void pe_focus(int p) {
    s_cm.focus = p;
    s_cm.confirm = 0;
    s_cm.note[0] = '\0';
    if (p == P_EFFECTS) {
        psx_mod_write_word(s_cm.list[P_EFFECTS] + N_CURSOR_X, 0);
        psx_mod_write_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y, 0);
        psx_mod_write_word(s_cm.list[P_EFFECTS] + N_SCROLL_Y, 0);
    } else if (s_cm.list[P_EFFECTS]) {
        psx_mod_write_word(s_cm.list[P_EFFECTS] + N_CURSOR_X, 0xFFFFFFFFu);
    }
    psx_mod_write_word(PE_CURRENT_NODE, s_cm.list[p]);
}

/* ---- Aya's panels ------------------------------------------------------------------ */
static void pe_hide_panels(void) {
    int n = 0;
    uint32_t node = psx_mod_read_word(PE_WIDGET_LIST_HEAD);
    for (int guard = 0; pe_ram_pointer(node) && guard < 64 && n < PE_HIDDEN_MAX; ++guard) {
        const uint32_t sel = psx_mod_read_word(node + N_SELECTED);
        if (psx_mod_read_word(node + N_MODE) == 1 && (sel == 0x12 || sel == 0x18 || sel == 0x2D) &&
            psx_mod_read_word(node + N_CURSOR_Y) == 0) {
            psx_mod_write_word(node + N_CURSOR_Y, 1);
            s_cm.hidden[n] = node;
            s_cm.hidden_selected[n] = sel;
            ++n;
        }
        node = psx_mod_read_word(node + N_NEXT);
    }
    s_cm.overlay = psx_mod_read_word(PE_STATUS_OVERLAY);
    psx_mod_write_word(PE_STATUS_OVERLAY, 0);
}

static void pe_show_panels(void) {
    for (int i = 0; i < PE_HIDDEN_MAX; ++i) {
        const uint32_t node = s_cm.hidden[i];
        if (!pe_ram_pointer(node)) continue;
        if (psx_mod_read_word(node + N_MODE) == 1 &&
            psx_mod_read_word(node + N_SELECTED) == s_cm.hidden_selected[i])
            psx_mod_write_word(node + N_CURSOR_Y, 0);
    }
    psx_mod_write_word(PE_STATUS_OVERLAY, s_cm.overlay);
}

/* ---- open / close ------------------------------------------------------------------ */
static void pe_close(void) {
    if (s_cm.confirm) {
        pe_node_free(s_cm.dlg_list);
        pe_node_free(s_cm.dlg);
    }
    for (int p = 0; p < P_COUNT; ++p) {
        if (s_cm.list[p]) pe_node_free(s_cm.list[p]);
        if (s_cm.panel[p]) pe_node_free(s_cm.panel[p]);
    }
    pe_show_panels();
    if (pe_ram_pointer(s_cm.main_list) && psx_mod_read_word(s_cm.main_list + N_DRAW) == PE_DRAW_ITEM_LIST)
        psx_mod_write_word(PE_CURRENT_NODE, s_cm.main_list);
    memset(&s_cm, 0, sizeof s_cm);
}

static void pe_open(uint32_t main_list, int mode) {
    if (pe_free_nodes() < 2 * P_COUNT + 1) return;
    memset(&s_cm, 0, sizeof s_cm);
    s_cm.mode = mode;
    s_cm.main_list = main_list;
    for (int p = 0; p < P_COUNT; ++p) {
        if (mode == MODE_KEYS && p == P_EFFECTS) continue;     /* list + info only */
        if (!pe_build_panel(p)) { s_cm.open = 1; pe_close(); return; }
    }
    s_cm.open = 1;
    s_cm.note_effects_row = -1;
    /* Catalog: start on the first slot (row 0 is the gun's name). */
    psx_mod_write_word(s_cm.list[P_SLOTS] + N_CURSOR_Y,
                       mode == MODE_CATALOG && pe_slot_row(1).kind == R_SLOT ? 1u : 0u);
    s_cm.prev_slot_row = (int)psx_mod_read_word(s_cm.list[P_SLOTS] + N_CURSOR_Y);
    for (int p = 0; p < P_COUNT; ++p) pe_fit_list(p);
    pe_hide_panels();
    pe_focus(P_SLOTS);
}

static void pe_on_item_select(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t node = cpu->gpr[4];
    if (s_cm.open) {
        if (node == s_cm.panel[P_SLOTS] || node == s_cm.panel[P_EFFECTS]) {
            s_cm.pending |= cpu->gpr[5];
            cpu->gpr[5] = 0;                  /* the game does nothing with it */
        }
        return;
    }
    if (!(cpu->gpr[5] & IN_CONFIRM) || !pe_row_wanted() || !pe_ram_pointer(node)) return;
    const uint32_t list = psx_mod_read_word(node + N_CHILD0);
    if (!pe_ram_pointer(list) || psx_mod_read_word(list + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (!pe_has_our_row(list)) return;
    const int row = pe_cursor_row(list);
    const int role = pe_row_role(row);
    if (role == ROLE_CATALOG || role == ROLE_KEYS) {
        cpu->gpr[5] = 0;                      /* not the game's BP screen, nor nothing */
        pe_open(list, role == ROLE_KEYS ? MODE_KEYS : MODE_CATALOG);
    } else if (role == ROLE_BP) {
        /* The BP row, moved down: the game knows it as its last action row. */
        s_bp_restore_list = list;
        s_bp_restore_row = psx_mod_read_word(list + N_CURSOR_Y);
        psx_mod_write_word(list + N_CURSOR_Y,
                           s_bp_restore_row - (uint32_t)(row - (pe_vanilla_rows() - 1)));
    }
}

/* ---- actions ----------------------------------------------------------------------- */
static void pe_note(const char *text) {
    snprintf(s_cm.note, sizeof s_cm.note, "%s", text);
    s_cm.note_effects_row = (int)psx_mod_read_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y);
}

/* The game's own "Is this OK?" line, found once in text table 4. */
static uint32_t pe_find_is_this_ok(void) {
    static const char want[] = "Is this OK";
    if (s_is_this_ok) return s_is_this_ok;
    for (unsigned id = 0; id < 256; ++id) {
        const uint32_t s = pe_text_entry(0x04u, id);
        if (!s) break;
        unsigned k = 0;
        while (want[k] && psx_mod_read_byte(s + k) == pe_glyph(want[k])) ++k;
        if (!want[k]) return s_is_this_ok = s;
    }
    return 0;
}

static void pe_open_dialog(Row r, int row) {
    if (pe_free_nodes() < 2) { pe_note("Can't ask now"); return; }
    PeText first;
    memset(&first, 0, sizeof first);
    memset(s_dlg_text, 0, sizeof s_dlg_text);
    s_dlg_lines = 0;
    pe_t_str(&first, pe_fx_name(r.which, r.effect));
    pe_t_str(&first, " will be installed");
    PeText *l = &s_dlg_text[s_dlg_lines++];
    pe_t_str(l, "for ");
    pe_t_num(l, (unsigned)pe_cat_price(r.which, r.effect));
    pe_t_str(l, pe_cat_needs_tool(r.which, r.effect) ? " Junk and a Tool" : " Junk");
    int out[4];
    const int k = pe_cat_replaces(r.which, r.slot, r.effect, out, 2);
    if (k == 1) {
        l = &s_dlg_text[s_dlg_lines++];
        pe_t_str(l, pe_fx_name(r.which, out[0]));
        pe_t_str(l, " will be taken out");
    } else if (k == 2) {
        l = &s_dlg_text[s_dlg_lines++];
        pe_t_str(l, pe_fx_name(r.which, out[0]));
        pe_t_str(l, " and ");
        pe_t_str(l, pe_fx_name(r.which, out[1]));
        pe_t_str(&s_dlg_text[s_dlg_lines++], "will be taken out");
    }
    const uint32_t ok = pe_find_is_this_ok();
    if (ok) {
        l = &s_dlg_text[s_dlg_lines++];
        for (unsigned i = 0; i < 40 && psx_mod_read_byte(ok + i) != G_END; ++i) pe_t_byte(l, psx_mod_read_byte(ok + i));
    }
    /* First line: the game prints it from the label buffer. */
    for (unsigned i = 0; i < first.n && i < 0x3Eu; ++i) psx_mod_write_byte(PE_ITEM_LABEL_BUF + i, first.b[i]);
    psx_mod_write_byte(PE_ITEM_LABEL_BUF + (first.n < 0x3Eu ? first.n : 0x3Eu), G_END);
    psx_mod_write_word(PE_ITEM_PRIM_HANDLES, 0);
    int width = pe_text_width(first.b, first.n);
    for (int i = 0; i < s_dlg_lines; ++i) {
        const int w = pe_text_width(s_dlg_text[i].b, s_dlg_text[i].n);
        if (w > width) width = w;
    }
    if (width < 120) width = 120;
    const int height = 50 + 14 * s_dlg_lines;

    /* MenuWidget_CreateSimpleNode(0x29, effects list, 0, 1) */
    const uint32_t sd = PE_SIMPLE_DESC + PE_DIALOG * 0x10u, gd = PE_GRID_DESC + PE_DIALOG * 0x20u;
    const uint32_t dlg = pe_node_alloc();
    const uint32_t list = pe_node_alloc();
    if (!dlg || !list) { if (list) pe_node_free(list); if (dlg) pe_node_free(dlg); return; }
    psx_mod_write_word(dlg + N_PARENT, s_cm.list[P_EFFECTS]);
    psx_mod_write_word(dlg + N_MODE, 1);
    psx_mod_write_word(dlg + N_SELECTED, PE_DIALOG);
    psx_mod_write_word(dlg + N_GRID_W, (uint32_t)(width + 20));
    psx_mod_write_word(dlg + N_ROWS, (uint32_t)height);
    psx_mod_write_word(dlg + N_X, (uint32_t)((300 - width) >> 1));
    {
        const int dx = (int)psx_mod_read_word(sd + 0u), dy = (int)psx_mod_read_word(sd + 4u);
        psx_mod_write_word(dlg + N_Y, (uint32_t)(dx ? dy : (dy ? 0x50 : 0x78) - (height >> 1)));
    }
    psx_mod_write_word(dlg + N_CURSOR_X, 1);  /* the others dim, as for any dialog */
    psx_mod_write_word(dlg + N_DRAW, PE_FN_DRAW_ITEM_LABEL);
    psx_mod_write_word(dlg + N_UPDATE, PE_FN_CONFIRM_DIALOG);
    /* MenuWidget_CreateNode(0x29, dialog, dialog) */
    psx_mod_write_word(list + N_PARENT, dlg);
    psx_mod_write_word(dlg + N_CHILD0, list);
    psx_mod_write_word(list + N_MODE, 2);
    psx_mod_write_word(list + N_SELECTED, PE_DIALOG);
    psx_mod_write_word(list + N_AUX, PE_DIALOG);
    psx_mod_write_word(list + N_UPDATE, PE_FN_STEP_SCROLL);
    psx_mod_write_word(list + N_DRAW, PE_FN_OPTION_LIST);
    psx_mod_write_word(list + N_GRID_W, psx_mod_read_word(gd + 0x08u));
    psx_mod_write_word(list + N_X_LIMIT, psx_mod_read_word(gd + 0x08u));
    psx_mod_write_word(list + N_ROWS, psx_mod_read_word(gd + 0x0Cu));
    psx_mod_write_word(list + N_ROW_CAP, psx_mod_read_word(gd + 0x0Cu));
    psx_mod_write_word(list + N_Y_LIMIT, psx_mod_read_word(gd + 0x10u));
    psx_mod_write_word(list + N_CELL_W, psx_mod_read_word(gd + 0x14u));
    psx_mod_write_word(list + N_ROW_H, psx_mod_read_word(gd + 0x18u));
    psx_mod_write_word(list + N_LAYOUT, psx_mod_read_word(gd + 0x1Cu));
    psx_mod_write_word(list + N_TARGET_X, 0xFFFFFFFFu);
    psx_mod_write_word(list + N_TARGET_Y, 0xFFFFFFFFu);
    psx_mod_write_word(list + N_X, (uint32_t)((width + 20 - 0x80) >> 1));
    psx_mod_write_word(list + N_Y, (uint32_t)(height - 0x14));
    psx_mod_write_word(PE_ACTION_TEXT_BASE, 5);
    psx_mod_write_word(PE_CONFIRM_CALLBACK, PE_FN_SAY_YES);
    psx_mod_write_word(PE_CURRENT_NODE, list);
    s_cm.dlg = dlg;
    s_cm.dlg_list = list;
    s_cm.confirm = 1;
    s_cm.confirm_row = row;
    s_cm.confirm_effect = r.effect;
    s_cm.note[0] = '\0';
}

static void pe_pick_effect(void) {
    char msg[48], why[48];
    const int row = (int)psx_mod_read_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y);
    const Row r = pe_effect_row(row);
    if (r.kind == R_REMOVE) {
        const int had = pe_cat_slot_effect(r.which, r.slot);
        if (pe_cat_remove(r.which, r.slot, 0) != PE_CAT_OK) { pe_note("Can't remove that"); return; }
        snprintf(msg, sizeof msg, "Removed %s", pe_fx_name(r.which, had));
        pe_fit_list(P_EFFECTS);
        pe_note(msg);
        return;
    }
    if (r.kind == R_LOCKED) { pe_note("Dismantle gear that has it"); return; }
    if (r.kind != R_EFFECT) return;
    const char *blocked = pe_effect_blocker(r, why, sizeof why);
    if (!blocked[0]) { pe_open_dialog(r, row); return; }   /* OK installs (pe_install_row) */
    pe_note(blocked);
}

/* The dialog said OK: install the effect on `row`, if it still can go in. */
static void pe_install_row(int row) {
    char msg[48], why[48];
    const Row r = pe_effect_row(row);
    if (r.kind != R_EFFECT || r.effect != s_cm.confirm_effect) return;
    const char *blocked = pe_effect_blocker(r, why, sizeof why);
    if (blocked[0] || pe_cat_install(r.which, r.slot, r.effect, 0) != PE_CAT_OK) {
        pe_note(blocked[0] ? blocked : "Can't install now");
        return;
    }
    snprintf(msg, sizeof msg, pe_cat_is_bag_mod(r.which, r.effect) ? "Installed %s, re-equip"
             : "Installed %s", pe_fx_name(r.which, r.effect));
    /* A Remove row may have appeared above: stay on the effect. */
    Row rows[PE_ROWS_MAX];
    const int n = pe_effect_rows(rows, PE_ROWS_MAX);
    for (int i = 0; i < n; ++i)
        if (rows[i].kind == R_EFFECT && rows[i].effect == r.effect)
            psx_mod_write_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y, (uint32_t)i);
    pe_fit_list(P_EFFECTS);
    pe_note(msg);
}

static void pe_act(uint32_t buttons) {
    if (s_cm.mode == MODE_KEYS) {             /* view only */
        if (buttons & IN_CANCEL) pe_close();
        return;
    }
    if (s_cm.focus == P_SLOTS) {
        if (buttons & IN_CANCEL) { pe_close(); return; }
        if (buttons & (IN_CONFIRM | IN_RIGHT)) {
            const Row s = pe_chosen_slot();
            if (s.kind != R_SLOT) return;
            if (pe_cat_is_bag_mod(s.which, pe_cat_slot_effect(s.which, s.slot))) {
                pe_note("Capacity mods stay put");
                return;
            }
            pe_focus(P_EFFECTS);
            pe_fit_list(P_EFFECTS);
        }
    } else {
        if (buttons & (IN_CANCEL | IN_LEFT)) { pe_focus(P_SLOTS); return; }
        if (buttons & IN_CONFIRM) pe_pick_effect();
    }
}

/* Keep the slot cursor off the item-name rows, moving on in its direction. */
static void pe_skip_names(void) {
    const uint32_t list = s_cm.list[P_SLOTS];
    const int count = pe_row_count(P_SLOTS);
    int row = (int)psx_mod_read_word(list + N_CURSOR_Y);
    const int down = row > s_cm.prev_slot_row || (s_cm.prev_slot_row == count - 1 && row == 0);
    for (int guard = 0; guard < count; ++guard) {
        const int kind = pe_slot_row(row).kind;
        if (kind == R_SLOT || kind == R_NOSLOTS) break;
        row = down ? (row + 1) % count : (row + count - 1) % count;
    }
    if (row != s_cm.prev_slot_row) s_cm.note[0] = '\0';
    psx_mod_write_word(list + N_CURSOR_Y, (uint32_t)row);
    s_cm.prev_slot_row = row;
    pe_fit_list(P_SLOTS);
}

/* ---- Sort from Use Item ---------------------------------------------------------- */
/* Square on the Use Item list opens the game's own Sort screen (main menu
 * action 6, whose row pe_bag_sort.c takes off the menu). Use Item and Sort
 * open from the same main menu state (CLOSE_SELECT_VIEWS), so swapping one
 * for the other is: free Use Item's nodes (its header 0x1B, panel 1 and list,
 * as its own Cancel does) and build what Menu_CreateInvSwapView(main list, 0)
 * builds -- a mode-1 node 0x3A stepped by Menu_EquipOptionsInputHandler and a
 * mode-2 list 0x3A drawn by Menu_DrawEquipOptionsList -- with view mode 0.
 * From there it is all the game: a sort comes back to Use Item
 * (Menu_OpenInventoryOrSwapView(1)), Cancel to the main menu. */
#define PE_FIELD_PAD          0x8009D26Cu   /* g_FieldPadBits: bit 31 Square */
#define PE_FN_INV_INPUT       0x80044444u   /* Menu_InventoryInputHandler */
#define PE_DRAW_EQUIP_SLOTS   0x8004F8D0u   /* Menu_DrawSelectableEquipSlotList */
#define PE_FN_SORT_INPUT      0x800471E4u   /* Menu_EquipOptionsInputHandler */
#define PE_DRAW_SORT_LIST     0x800471BCu   /* Menu_DrawEquipOptionsList */
#define PE_SUBMENU_OPEN       0x8009CF00u   /* g_MenuActionSubmenuOpen */
#define PE_INV_SELECTED       0x8009CF8Cu   /* g_InvSelectedItemIndex */
#define PE_MENU_EQUIP_MODE    0x8009CF0Cu   /* g_MenuEquipMode: Wayne's */
#define PE_INV_VIEW_MODE      0x8009CFB8u   /* g_MenuInventoryViewMode */
#define PE_SORT_SCREEN        0x3Au
#define PE_USE_ITEM_HEADER    0x1Bu

/* MenuWidget_DestroyNodeRecursive, minus the column layout it saves. */
static void pe_node_destroy(uint32_t node, int depth) {
    if (!pe_ram_pointer(node) || depth > 4) return;
    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t c = psx_mod_read_word(node + N_CHILD0 + i * 4u);
        if (c) pe_node_destroy(c, depth + 1);
    }
    pe_node_free(node);
    for (uint32_t n = psx_mod_read_word(PE_WIDGET_LIST_HEAD), guard = 0; pe_ram_pointer(n) && guard < 64;
         n = psx_mod_read_word(n + N_NEXT), ++guard) {
        if (psx_mod_read_word(n + N_PARENT) == node) psx_mod_write_word(n + N_PARENT, 0);
        for (uint32_t i = 0; i < 4u; ++i)
            if (psx_mod_read_word(n + N_CHILD0 + i * 4u) == node) psx_mod_write_word(n + N_CHILD0 + i * 4u, 0);
    }
}

static uint32_t pe_find_panel(uint32_t selected) {
    for (uint32_t n = psx_mod_read_word(PE_WIDGET_LIST_HEAD), guard = 0; pe_ram_pointer(n) && guard < 64;
         n = psx_mod_read_word(n + N_NEXT), ++guard)
        if (psx_mod_read_word(n + N_MODE) == 1 && psx_mod_read_word(n + N_SELECTED) == selected) return n;
    return 0;
}

static void pe_sort_from_use_item(void) {
    static int s_square_was;
    const int square = (psx_mod_read_word(PE_FIELD_PAD) & 0x80000000u) != 0;
    const int pressed = square && !s_square_was;
    s_square_was = square;
    if (pe_find_panel(PE_SORT_SCREEN)) {
        const uint32_t n = pe_find_panel(PE_SORT_SCREEN);
        if (psx_mod_read_word(n + N_UPDATE) == PE_FN_SORT_INPUT) pe_bag_sort_screen_seen();
    }
    if (!pressed || s_cm.open || !pe_loaded()) return;
    const uint32_t list = psx_mod_read_word(PE_CURRENT_NODE);
    if (!pe_ram_pointer(list) || psx_mod_read_word(list + N_DRAW) != PE_DRAW_EQUIP_SLOTS) return;
    const uint32_t panel = psx_mod_read_word(list + N_PARENT);
    if (!pe_ram_pointer(panel) || psx_mod_read_word(panel + N_MODE) != 1 ||
        psx_mod_read_word(panel + N_SELECTED) != 1 || psx_mod_read_word(panel + N_UPDATE) != PE_FN_INV_INPUT)
        return;
    const uint32_t main_list = psx_mod_read_word(panel + N_PARENT);
    if (!pe_ram_pointer(main_list) || psx_mod_read_word(main_list + N_DRAW) != PE_DRAW_ITEM_LIST) return;
    if (psx_mod_read_word(PE_SUBMENU_OPEN) || (int32_t)psx_mod_read_word(PE_INV_SELECTED) >= 0 ||
        (int32_t)psx_mod_read_word(list + N_TARGET_X) >= 0 || psx_mod_read_word(PE_MENU_EQUIP_MODE) ||
        psx_mod_read_word(PE_BATTLE_EQUIP_MODE))
        return;                               /* mid-action, mid-move, Wayne's or battle */
    const uint32_t sd = PE_SIMPLE_DESC + PE_SORT_SCREEN * 0x10u, gd = PE_GRID_DESC + PE_SORT_SCREEN * 0x20u;
    if (psx_mod_read_word(gd + 0x0Cu) < psx_mod_read_word(gd + 0x10u)) return;   /* would need a scroll bar */

    pe_node_destroy(panel, 0);                /* panel 1 and its list */
    pe_node_destroy(pe_find_panel(PE_USE_ITEM_HEADER), 0);
    if (pe_free_nodes() < 2) { psx_mod_write_word(PE_CURRENT_NODE, main_list); return; }

    /* MenuWidget_CreateSimpleNode(0x3A, main list, 0, 0) */
    const uint32_t node = pe_node_alloc();
    psx_mod_write_word(node + N_PARENT, main_list);
    psx_mod_write_word(node + N_MODE, 1);
    psx_mod_write_word(node + N_SELECTED, PE_SORT_SCREEN);
    {
        const int dx = (int)psx_mod_read_word(sd + 0u), dy = (int)psx_mod_read_word(sd + 4u);
        const int w = (int)psx_mod_read_word(sd + 8u), h = (int)psx_mod_read_word(sd + 12u);
        psx_mod_write_word(node + N_X, (uint32_t)(dx ? dx : 0xA0 - (w >> 1)));
        psx_mod_write_word(node + N_Y, (uint32_t)(dx ? dy : (dy ? 0x50 : 0x78) - (h >> 1)));
        psx_mod_write_word(node + N_GRID_W, (uint32_t)w);
        psx_mod_write_word(node + N_ROWS, (uint32_t)h);
    }
    psx_mod_write_word(node + N_UPDATE, PE_FN_SORT_INPUT);
    /* MenuWidget_CreateNode(0x3A, node, node) */
    const uint32_t sl = pe_node_alloc();
    psx_mod_write_word(sl + N_PARENT, node);
    psx_mod_write_word(node + N_CHILD0, sl);
    psx_mod_write_word(sl + N_MODE, 2);
    psx_mod_write_word(sl + N_SELECTED, PE_SORT_SCREEN);
    psx_mod_write_word(sl + N_AUX, PE_SORT_SCREEN);
    psx_mod_write_word(sl + N_X, psx_mod_read_word(gd + 0x00u));
    psx_mod_write_word(sl + N_Y, psx_mod_read_word(gd + 0x04u));
    psx_mod_write_word(sl + N_UPDATE, PE_FN_STEP_SCROLL);
    psx_mod_write_word(sl + N_DRAW, PE_DRAW_SORT_LIST);
    psx_mod_write_word(sl + N_GRID_W, psx_mod_read_word(gd + 0x08u));
    psx_mod_write_word(sl + N_X_LIMIT, psx_mod_read_word(gd + 0x08u));
    psx_mod_write_word(sl + N_ROWS, psx_mod_read_word(gd + 0x0Cu));
    psx_mod_write_word(sl + N_ROW_CAP, psx_mod_read_word(gd + 0x0Cu));
    psx_mod_write_word(sl + N_Y_LIMIT, psx_mod_read_word(gd + 0x10u));
    psx_mod_write_word(sl + N_CELL_W, psx_mod_read_word(gd + 0x14u));
    psx_mod_write_word(sl + N_ROW_H, psx_mod_read_word(gd + 0x18u));
    psx_mod_write_word(sl + N_LAYOUT, psx_mod_read_word(gd + 0x1Cu));
    psx_mod_write_word(sl + N_TARGET_X, 0xFFFFFFFFu);
    psx_mod_write_word(sl + N_TARGET_Y, 0xFFFFFFFFu);
    psx_mod_write_word(PE_CURRENT_NODE, sl);
    psx_mod_write_word(PE_INV_VIEW_MODE, 0);
    pe_bag_sort_screen_seen();
}

/* Once a frame. */
static void pe_on_frame(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    pe_sort_from_use_item();
    if (s_color_saved) {
        psx_mod_write_word(PE_TEXT_COLOR, s_saved_color);
        psx_mod_write_word(PE_TEXT_COLOR_DIM, s_saved_color_dim);
        s_color_saved = 0;
    }
    if (s_bp_restore_list) {
        if (psx_mod_read_word(s_bp_restore_list + N_DRAW) == PE_DRAW_ITEM_LIST)
            psx_mod_write_word(s_bp_restore_list + N_CURSOR_Y, s_bp_restore_row);
        s_bp_restore_list = 0;
    }
    if (!s_cm.open) return;
    /* Still ours? (The game resets the whole widget pool when the menu closes.) */
    const uint32_t cur = psx_mod_read_word(PE_CURRENT_NODE);
    if (psx_mod_read_word(s_cm.list[P_SLOTS] + N_DRAW) != PE_FN_OPTION_LIST ||
        psx_mod_read_word(s_cm.panel[P_SLOTS] + N_SELECTED) != PE_OUR_PANEL) {
        memset(&s_cm, 0, sizeof s_cm);
        return;
    }
    if (s_cm.confirm) {                       /* the dialog: it reports through our hook */
        if (cur == s_cm.dlg_list && psx_mod_read_word(s_cm.dlg + N_UPDATE) == PE_FN_CONFIRM_DIALOG) return;
        s_cm.confirm = 0;                     /* gone some other way */
        if (cur == s_cm.dlg_list) psx_mod_write_word(PE_CURRENT_NODE, s_cm.list[P_EFFECTS]);
        return;
    }
    if (cur != s_cm.list[P_SLOTS] && cur != s_cm.list[P_EFFECTS]) { pe_close(); return; }
    const uint32_t buttons = s_cm.pending;
    s_cm.pending = 0;
    if (buttons) pe_act(buttons);
    if (!s_cm.open) return;
    if (s_cm.mode == MODE_KEYS) { pe_fit_list(P_SLOTS); return; }
    pe_skip_names();
    pe_fit_list(P_EFFECTS);
}

/* ---- drawing ------------------------------------------------------------------------ */
static int pe_our_list(uint32_t node) {
    if (!s_cm.open) return -1;
    for (int p = 0; p < P_COUNT; ++p) if (node == s_cm.list[p]) return p;
    return -1;
}

/* Can the focused list's row be picked? (Its Cross sound.) */
static int pe_row_ok(int p) {
    char why[48];
    if (s_cm.mode == MODE_KEYS) return 0;     /* nothing to pick: the error sound */
    if (p == P_SLOTS) {
        const Row s = pe_chosen_slot();
        return s.kind == R_SLOT && !pe_cat_is_bag_mod(s.which, pe_cat_slot_effect(s.which, s.slot));
    }
    const Row r = pe_effect_row((int)psx_mod_read_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y));
    return r.kind == R_REMOVE || (r.kind == R_EFFECT && !pe_effect_blocker(r, why, sizeof why)[0]);
}

/* The dummy swap mark: one column right of the cursor on the focused list. */
static void pe_arm(int p) {
    const uint32_t list = s_cm.list[p];
    if (p != s_cm.focus) {
        psx_mod_write_word(list + N_TARGET_X, 0xFFFFFFFFu);
        return;
    }
    psx_mod_write_word(list + N_TARGET_X, 1);
    psx_mod_write_word(list + N_TARGET_Y, psx_mod_read_word(list + N_CURSOR_Y));
    psx_mod_write_word(list + N_ITEM_ACTION, pe_row_ok(p) ? PE_FN_SAY_YES : PE_FN_SAY_NO);
}

static void pe_on_option_list(struct CPUState *cpu, uint32_t address) {
    (void)address;
    s_drawing = pe_our_list(cpu->gpr[4]) >= 0 ? cpu->gpr[4] : 0;
    s_drawing_row = -1;
    if (!s_drawing) return;
    const int p = pe_our_list(s_drawing);
    if (p != P_INFO && psx_mod_read_word(s_drawing + N_CURSOR_X) == 1) {
        psx_mod_write_word(s_drawing + N_CURSOR_X, 0);     /* Circle dropped the mark */
        s_cm.pending |= IN_CANCEL;
    }
    pe_fit_list(p);
    if (p != P_INFO) pe_arm(p);
}

/* The focused list's itemAction, called by Menu_StepScrollCursor on Cross. */
static void pe_on_item_action(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_cm.open) return;
    const uint32_t ra = cpu->gpr[31];
    if (s_cm.confirm && ra >= PE_FN_CONFIRM_DIALOG && ra < PE_FN_CONFIRM_DIALOG_END) {
        /* g_MenuConfirmCallback(node, ok): the dialog has freed its nodes. */
        s_cm.confirm = 0;
        psx_mod_write_word(PE_CONFIRM_CALLBACK, 0);
        psx_mod_write_word(PE_CURRENT_NODE, s_cm.list[P_EFFECTS]);
        if (cpu->gpr[5]) pe_install_row(s_cm.confirm_row);
        return;
    }
    if (ra < PE_FN_STEP_SCROLL || ra >= PE_FN_STEP_SCROLL_END) return;
    if (psx_mod_read_word(PE_CURRENT_NODE) != s_cm.list[s_cm.focus]) return;
    s_cm.pending |= IN_CONFIRM;
}

/* Vanilla mod badges: an economy armor effect has none (see the top). */
static void pe_on_code_item(struct CPUState *cpu, uint32_t address) {
    (void)address;
    s_blank_badge = 0;
    const uint32_t data = psx_mod_read_word(PE_ACTION_ITEM_DATA);
    if (!pe_ram_pointer(data) || psx_mod_read_word(PE_ITEM_IS_GUN)) return;
    const uint32_t row = cpu->gpr[4];
    if (row >= psx_mod_read_byte(data + 0x14u)) return;
    const char *label = pe_text_badge(WHICH_ARMOR, psx_mod_read_byte(data + 0x15u + row) & 0x1F);
    if (!label) return;
    s_blank_badge = 1;
    pe_emit_text_badge(label, psx_mod_read_word(PE_TEXT_X), psx_mod_read_word(PE_TEXT_Y),
                       (int)psx_mod_read_word(PE_TEXT_DIMMED));
}

static void pe_on_option_row(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_drawing) return;
    s_drawing_row = (int)cpu->gpr[4];
}

static void pe_on_centered_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (s_color_saved) {                      /* a name row's yellow is done */
        psx_mod_write_word(PE_TEXT_COLOR, s_saved_color);
        psx_mod_write_word(PE_TEXT_COLOR_DIM, s_saved_color_dim);
        s_color_saved = 0;
    }
    if (s_cm.confirm && cpu->gpr[4] == PE_ITEM_LABEL_BUF) {   /* our dialog: the other lines */
        const uint32_t x0 = psx_mod_read_word(PE_TEXT_X), y0 = psx_mod_read_word(PE_TEXT_Y);
        const int box = (int)cpu->gpr[5];
        const uint32_t color = psx_mod_read_word(PE_TEXT_COLOR);
        for (int i = 0; i < s_dlg_lines; ++i) {
            const PeText *l = &s_dlg_text[i];
            uint32_t x = x0 + (uint32_t)((box - (pe_text_width(l->b, l->n) + 4)) >> 1);
            for (unsigned k = 0; k < l->n; ++k)
                x += (uint32_t)(pe_emit_glyph(l->b[k], x, y0 + 14u * (uint32_t)(i + 1), color) + 1);
        }
        return;
    }
    if (!s_drawing || s_drawing_row < 0) return;
    const int p = pe_our_list(s_drawing);
    const int row = s_drawing_row;
    s_drawing_row = -1;
    if (p < 0 || !pe_mem(&s_rowbuf, 96)) return;
    PeText left, right;
    memset(&left, 0, sizeof left);
    memset(&right, 0, sizeof right);
    int dim = 0, header = 0;
    if (row < pe_row_count(p)) pe_row_text(p, row, &left, &right, &dim, &header);
    const int cell = (int)psx_mod_read_word(s_drawing + N_CELL_W) - 8;
    if (right.n) {                            /* right-align with spaces */
        const uint8_t space = pe_glyph(' ');
        const int sw = pe_text_width(&space, 1);
        int gap = cell - pe_text_width(left.b, left.n) - pe_text_width(right.b, right.n);
        if (gap < sw) gap = sw;
        for (; gap >= sw; gap -= sw) pe_t_byte(&left, space);
        for (unsigned i = 0; i < right.n; ++i) pe_t_byte(&left, right.b[i]);
    }
    if (p == P_SLOTS && s_cm.mode == MODE_KEYS) {
        const uint16_t id = pe_keys_item(row);
        if (id) pe_emit_sprite(pe_item_icon_sprite(id), psx_mod_read_word(PE_TEXT_X),
                               psx_mod_read_word(PE_TEXT_Y), dim || psx_mod_read_word(PE_TEXT_DIMMED));
    } else if (p == P_SLOTS) {
        const Row r = pe_slot_row(row);
        const uint32_t badge = r.kind == R_SLOT ? pe_badge(r.which, pe_cat_slot_effect(r.which, r.slot)) : 0;
        const uint32_t bx = psx_mod_read_word(PE_TEXT_X) + 20u, by = psx_mod_read_word(PE_TEXT_Y) + 1u;
        const int bdim = dim || psx_mod_read_word(PE_TEXT_DIMMED);
        const char *label = r.kind == R_SLOT ? pe_text_badge(r.which, pe_cat_slot_effect(r.which, r.slot)) : 0;
        if (badge) pe_emit_sprite(badge, bx, by, bdim);
        else if (label) pe_emit_text_badge(label, bx, by, bdim);
    }
    pe_put(s_rowbuf, &left);
    cpu->gpr[4] = s_rowbuf;
    cpu->gpr[5] = (uint32_t)(pe_text_width(left.b, left.n) + 4);  /* centred in itself: left */
    if (dim) psx_mod_write_word(PE_TEXT_DIMMED, 1);
    if (header && !psx_mod_read_word(PE_TEXT_DIMMED)) {
        s_saved_color = psx_mod_read_word(PE_TEXT_COLOR);
        s_saved_color_dim = psx_mod_read_word(PE_TEXT_COLOR_DIM);
        s_color_saved = 1;
        psx_mod_write_word(PE_TEXT_COLOR, PE_HEADER_COLOR);
    }
}

/* The title bar: "Mod Catalog" on our main menu row; the description of what
 * is highlighted while the Catalog is open. */
static void pe_on_help_text(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    s_help_pending = s_cm.open;
}

static uint32_t pe_description(void) {
    PeText t;
    memset(&t, 0, sizeof t);
    if (s_cm.mode == MODE_KEYS) {
        const uint16_t id = pe_keys_item((int)psx_mod_read_word(s_cm.list[P_SLOTS] + N_CURSOR_Y));
        const uint32_t d = id ? pe_text_entry(0x0Cu, (unsigned)id - 1u) : 0;  /* item descriptions */
        if (d) return d;
        pe_t_str(&t, "Key Items");
    } else if (s_cm.focus == P_EFFECTS) {
        const Row r = pe_effect_row((int)psx_mod_read_word(s_cm.list[P_EFFECTS] + N_CURSOR_Y));
        if (r.kind == R_EFFECT) return pe_fx_description(r.which, r.effect);
        if (r.kind == R_REMOVE) pe_t_str(&t, "Take the mod out of this slot");
        else if (r.kind == R_LOCKED) pe_t_str(&t, "Dismantle gear with this effect");
    } else {
        const Row s = pe_chosen_slot();
        const int e = s.kind == R_SLOT ? pe_cat_slot_effect(s.which, s.slot) : 0;
        if (e) return pe_fx_description(s.which, e);
        pe_t_str(&t, s.kind == R_SLOT ? "Empty slot" : "Mod Catalog");
    }
    if (!pe_mem(&s_descbuf, 64)) return 0;
    pe_put(s_descbuf, &t);
    return s_descbuf;
}

static void pe_on_print_raw_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!cpu->gpr[4]) return;
    if (s_cm.open) {
        if (!s_help_pending) return;
        /* With our list focused the title bar picks one of the main menu's
         * titles (0x27..0x31): put the description there. */
        const uint32_t first = pe_text_entry(0x10u, 0x27u), last = pe_text_entry(0x10u, PE_TITLE_ACTION9);
        if (cpu->gpr[4] < first || cpu->gpr[4] > last) return;
        s_help_pending = 0;
        const uint32_t d = pe_description();
        if (d) cpu->gpr[4] = d;
        return;
    }
    const uint32_t list = pe_main_list();
    if (!list || !pe_has_our_row(list)) return;
    const int role = pe_row_role(pe_cursor_row(list));
    const uint32_t row9 = pe_text_entry(0x10u, PE_TITLE_ACTION9), bp = pe_text_entry(0x10u, PE_TITLE_BP);
    if (role == ROLE_BP && cpu->gpr[4] == row9) {
        cpu->gpr[4] = bp;                                     /* the BP row, moved down */
        return;
    }
    if (role == ROLE_KEYS && cpu->gpr[4] == row9) {
        static uint32_t s_keys_title;
        if (!s_keys_title) {
            if (!pe_mem(&s_keys_title, 16)) return;
            PeText t;
            memset(&t, 0, sizeof t);
            pe_t_str(&t, "Key Items");
            pe_put(s_keys_title, &t);
        }
        cpu->gpr[4] = s_keys_title;
        return;
    }
    if (role != ROLE_CATALOG || cpu->gpr[4] != (pe_swapped() ? bp : row9)) return;
    if (!s_title) {
        if (!pe_mem(&s_title, 16)) return;
        PeText t;
        memset(&t, 0, sizeof t);
        pe_t_str(&t, "Mod Catalog");
        pe_put(s_title, &t);
    }
    cpu->gpr[4] = s_title;
}

/* Save states: s_cm rides along and the widgets are in the snapshot's RAM. */
static void pe_cm_state_loaded(int result, uint32_t saved_version, const void *saved,
                               uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_cm, 0, sizeof s_cm);
    s_drawing = 0;
    s_color_saved = 0;
    s_bp_restore_list = 0;
}

/* Called from pe_mod_menu.c after pe_wayne_menu_register. */
void pe_catalog_menu_register(void) {
    (void)psx_mod_register_function_entry_plugin("pe.catalog.rows", PE_FN_SET_ROWS, pe_on_set_rows);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.icons", PE_FN_ALLOC_SPRITE,
                                                 pe_on_alloc_sprite);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.title", PE_FN_PRINT_RAW_TEXT,
                                                 pe_on_print_raw_text);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.help", PE_FN_HELP_TEXT, pe_on_help_text);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.open", PE_FN_ITEM_SELECT,
                                                 pe_on_item_select);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.frame", PE_FN_DRAW_TEXTBOXES,
                                                 pe_on_frame);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.list", PE_FN_OPTION_LIST,
                                                 pe_on_option_list);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.row", PE_FN_OPTION_ROW,
                                                 pe_on_option_row);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.text", PE_FN_CENTERED_TEXT,
                                                 pe_on_centered_text);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.yes", PE_FN_SAY_YES, pe_on_item_action);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.no", PE_FN_SAY_NO, pe_on_item_action);
    (void)psx_mod_register_function_entry_plugin("pe.catalog.badge", PE_FN_CODE_ITEM, pe_on_code_item);
    (void)psx_mod_register_state("pe.catalog.ui", 2u, &s_cm, sizeof s_cm, pe_cm_state_loaded);
}
