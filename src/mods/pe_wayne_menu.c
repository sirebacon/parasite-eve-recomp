/*
 * Loot overhaul, phase 5: the Workshop in Wayne's own menu (bridge findings
 * pe-loot-overhaul-design-2026-10-11.md decisions 27, 31, 47).
 *
 * Wayne's menu is a script choice box: a text box (g_TextboxEntries,
 * 0x800BCEA8, 0x38 each: +0 state, +4 message, +8 style, +0x0C flags, +0x12
 * x, +0x14 y, +0x16 width, +0x18 height) showing a room message such as
 * "<FB 09 04>    Store equipment<F7>    Store item<F7>    Discard junk<F7>
 * Cancel<FF>". FB 09 n opens a choice of n options (3 bits, so at most 7);
 * Menu_DrawTextboxEntries moves the cursor (D_8009CEA0) on up/down and, on
 * confirm, stores it as the result (D_8009CEA4) the script reads.
 *
 * At the entry of Menu_DrawTextboxEntries (0x80037870), every frame:
 *   - a box showing Wayne's menu (a choice with "Discard junk") is pointed at
 *     our copy with "Workshop" above Cancel and "Discard junk  <your Junk>",
 *     one line taller;
 *   - confirm on Workshop is taken away from the game (the press is cleared)
 *     and the box shows our screens instead: Workshop (Requisition / Rebuild /
 *     Back), Requisition (goods with prices, 5 a page, in the game's dimmed
 *     gray when they can't be bought now) and Rebuild (dismantled gear with prices). The title line
 *     shows your Junk or what the last purchase did. Cancel (Circle) goes back
 *     a screen, Left/Right turn pages;
 *   - confirm on our Cancel moves the cursor up one first, so the script gets
 *     its own Cancel; every other option is untouched.
 * Text is the game's glyph code; item names are copied from its tables.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"

#define PE_FN_DRAW_TEXTBOXES     0x80037870u   /* Menu_DrawTextboxEntries */
#define PE_FN_AFTER_TEXTBOXES    0x800661A4u   /* Render_SetGteScreenOffset: next in the frame */
#define PE_TEXT_POOL_TABLE       0x800B0E44u   /* D_800B0E38.other_buffers[1 + frame] */
#define PE_DRAW_FRAME            0x8009CDDCu   /* D_8009CDDC: 0/1 */
#define PE_DIM_GRAY              0x40u         /* menus' dimmed text: (0x80 >> 1) */
#define PE_GLYPH_PACKET          28u           /* TextboxGlyphPacket */
#define PE_GLYPH_MAX             600u
#define PE_TEXTBOX_BASE          0x800BCEA8u   /* g_TextboxEntries */
#define PE_TEXTBOX_STRIDE        0x38u
#define PE_TEXTBOX_COUNT         4u
#define TB_STATE   0x00u
#define TB_MESSAGE 0x04u
#define TB_X       0x12u
#define TB_Y       0x14u
#define TB_W       0x16u
#define TB_H       0x18u
#define PE_CHOICE_CURSOR         0x8009CEA0u   /* s8 */
#define PE_PAD_PRESSED           0x8009D1F4u   /* this frame's new field inputs */
#define PE_IN_CONFIRM 0x0100u                  /* Cross */
#define PE_IN_CANCEL  0x0400u                  /* Circle */
#define PE_IN_LEFT    0x0040u
#define PE_IN_RIGHT   0x0010u
#define PE_MAX_HP_ADDR           0x800C0E06u
#define PE_WAYNE_COUNTER         0x800A78B0u
#define PE_STR_ORIGIN            0x800A8028u
#define PE_STR_DIRECTORY_OFFSET  0x800A802Cu

#define G_NEWLINE 0xF7u
#define G_END     0xFFu
#define G_CODE    0xFBu
#define G_CHOICE  0x09u
#define G_RED     0x04u                       /* our "can't buy" marker, recolored gray */
#define G_NORMAL  0x05u
#define PE_PAGE_SIZE 5
#define PE_TEXT_MAX  768

enum { SCR_NONE, SCR_WAYNE, SCR_ROOT, SCR_REQ, SCR_REB };

static struct {
    int screen;
    uint32_t entry;                           /* the text box we drive */
    uint32_t orig_msg;                        /* Wayne's own message */
    uint16_t orig_y, orig_h, orig_w;
    int wayne_count;                          /* options in Wayne's own menu */
    int page;
    char note[48];                            /* title line after an action */
} s_ui;

static uint32_t s_buf;                        /* our message, mod memory */
static uint8_t s_orig[PE_TEXT_MAX];           /* copy of Wayne's message */
static unsigned s_orig_len;

static int pe_ram_pointer(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

/* ---- text ------------------------------------------------------------------- */
typedef struct { uint8_t b[PE_TEXT_MAX]; unsigned n; unsigned line; unsigned widest; int lines; } PeText;

static void pe_t_byte(PeText *t, uint8_t c) {
    if (t->n < PE_TEXT_MAX - 2) t->b[t->n++] = c;
    if (c == G_NEWLINE) { ++t->lines; t->line = 0; }
    else if (c < 0xF0) { if (++t->line > t->widest) t->widest = t->line; }
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

static void pe_t_str(PeText *t, const char *s) { while (*s) pe_t_byte(t, pe_glyph(*s++)); }

static void pe_t_num(PeText *t, unsigned v) {
    char d[12];
    snprintf(d, sizeof d, "%u", v);
    pe_t_str(t, d);
}

static uint32_t pe_text_entry(uint32_t table_off, unsigned index) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory)) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + table_off);
    if (!pe_ram_pointer(table) || index >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + index * 2u);
}

/* The game's own name for an item (our names for the two relabelled ones). */
static void pe_t_item(PeText *t, uint16_t id) {
    if (id == 0x18) { pe_t_str(t, "Smoke Grenade"); return; }
    if (id == 0x19) { pe_t_str(t, "Lucky Charm"); return; }
    const uint32_t name = pe_text_entry(0x08u, (unsigned)id - 1u);
    if (!name) { pe_t_str(t, "?"); return; }
    for (unsigned i = 0; i < 24; ++i) {
        const uint8_t c = psx_mod_read_byte(name + i);
        if (c == G_END) break;
        pe_t_byte(t, c);
    }
    if (id == 0x02) pe_t_str(t, " x15");      /* both bullet boxes are "Bullets" */
    if (id == 0x03) pe_t_str(t, " x30");
}

static void pe_t_choice(PeText *t, int count) {
    pe_t_byte(t, G_CODE); pe_t_byte(t, G_CHOICE); pe_t_byte(t, (uint8_t)count);
}

static void pe_t_option_start(PeText *t, int first) {
    if (!first) pe_t_byte(t, G_NEWLINE);
    pe_t_str(t, "    ");
}

/* ---- Wayne's menu ----------------------------------------------------------- */
static const char s_discard_junk[] = "Discard junk";

static int pe_contains(const uint8_t *b, unsigned n, const char *ascii) {
    const unsigned m = (unsigned)strlen(ascii);
    for (unsigned i = 0; i + m <= n; ++i) {
        unsigned k = 0;
        while (k < m && b[i + k] == pe_glyph(ascii[k])) ++k;
        if (k == m) return 1;
    }
    return 0;
}

/* Is the message at `msg` Wayne's menu? Copies it into s_orig if so. */
static int pe_is_wayne_menu(uint32_t msg) {
    if (!pe_ram_pointer(msg)) return 0;
    uint8_t b[PE_TEXT_MAX];
    unsigned n = 0, choice_at = PE_TEXT_MAX;
    while (n < PE_TEXT_MAX - 1) {
        b[n] = psx_mod_read_byte(msg + n);
        if (b[n] == G_END) break;
        if (n >= 2 && b[n - 2] == G_CODE && b[n - 1] == G_CHOICE && choice_at == PE_TEXT_MAX)
            choice_at = n;
        ++n;
    }
    if (choice_at == PE_TEXT_MAX || n >= PE_TEXT_MAX - 1) return 0;
    if (!pe_contains(b, n, s_discard_junk)) return 0;
    const int count = b[choice_at] & 7;
    if (count < 2 || count >= 7) return 0;    /* no room for one more option */
    memcpy(s_orig, b, n + 1);
    s_orig_len = n;
    s_ui.wayne_count = count;
    return 1;
}

/* Wayne's options with Workshop above Cancel and the Junk on Discard junk. */
static void pe_build_wayne(PeText *t) {
    unsigned i = 0;
    while (i < s_orig_len && !(s_orig[i] == G_CODE && i + 1 < s_orig_len && s_orig[i + 1] == G_CHOICE)) {
        pe_t_byte(t, s_orig[i]);
        ++i;
    }
    i += 3;                                   /* FB 09 n */
    pe_t_choice(t, s_ui.wayne_count + 1);
    int option = 0;
    unsigned start = i;
    for (; i <= s_orig_len; ++i) {
        if (i < s_orig_len && s_orig[i] != G_NEWLINE) continue;
        if (option == s_ui.wayne_count - 1) { /* the last one is Cancel */
            pe_t_str(t, "    Workshop");
            pe_t_byte(t, G_NEWLINE);
        }
        for (unsigned k = start; k < i; ++k) pe_t_byte(t, s_orig[k]);
        if (pe_contains(s_orig + start, i - start, s_discard_junk)) {
            pe_t_str(t, "  ");
            pe_t_num(t, (unsigned)pe_loot_junk());
        }
        if (i < s_orig_len) pe_t_byte(t, G_NEWLINE);
        ++option;
        start = i + 1;
    }
}

/* ---- our screens ------------------------------------------------------------ */
static void pe_title(PeText *t, const char *name) {
    if (s_ui.note[0]) pe_t_str(t, s_ui.note);
    else { pe_t_str(t, name); pe_t_str(t, "   Junk "); pe_t_num(t, (unsigned)pe_loot_junk()); }
    pe_t_byte(t, G_NEWLINE);
}

static void pe_build_root(PeText *t) {
    pe_title(t, "Workshop");
    pe_t_choice(t, 3);
    pe_t_option_start(t, 1); pe_t_str(t, "Requisition");
    pe_t_option_start(t, 0); pe_t_str(t, "Rebuild");
    pe_t_option_start(t, 0); pe_t_str(t, "Back");
}

static int pe_pages(int count) { return count <= PE_PAGE_SIZE ? 1 : (count + PE_PAGE_SIZE - 1) / PE_PAGE_SIZE; }

/* Options shown on a list page: items, then Next page (if more), then Back. */
static int pe_list_options(int count, int *items_out) {
    int first = s_ui.page * PE_PAGE_SIZE;
    int items = count - first;
    if (items > PE_PAGE_SIZE) items = PE_PAGE_SIZE;
    if (items < 0) items = 0;
    *items_out = items;
    return items + (pe_pages(count) > 1 ? 1 : 0) + 1;
}

static void pe_build_list(PeText *t, int rebuild) {
    const int count = rebuild ? pe_rebuild_count() : pe_shop_count();
    int items;
    const int options = pe_list_options(count, &items);
    pe_title(t, rebuild ? (count ? "Rebuild" : "Rebuild: nothing dismantled") : "Requisition");
    pe_t_choice(t, options);
    for (int k = 0; k < items; ++k) {
        const int i = s_ui.page * PE_PAGE_SIZE + k;
        pe_t_option_start(t, k == 0);
        const int red = rebuild ? pe_rebuild_price_index(i) > pe_loot_junk()
                                : pe_shop_blocked_index(i) != 0;
        if (red) { pe_t_byte(t, G_CODE); pe_t_byte(t, G_RED); }
        pe_t_item(t, rebuild ? pe_rebuild_item(i) : pe_shop_item(i));
        pe_t_str(t, "  ");
        pe_t_num(t, (unsigned)(rebuild ? pe_rebuild_price_index(i) : pe_shop_price_index(i)));
        if (red) { pe_t_byte(t, G_CODE); pe_t_byte(t, G_NORMAL); }
    }
    if (pe_pages(count) > 1) { pe_t_option_start(t, items == 0); pe_t_str(t, "Next page"); }
    pe_t_option_start(t, items == 0 && pe_pages(count) == 1);
    pe_t_str(t, "Back");
}

/* Write the current screen into the box, sized to fit. */
static void pe_show(int screen, int cursor) {
    PeText t;
    memset(&t, 0, sizeof t);
    t.lines = 1;
    s_ui.screen = screen;
    if (screen == SCR_WAYNE) pe_build_wayne(&t);
    else if (screen == SCR_ROOT) pe_build_root(&t);
    else pe_build_list(&t, screen == SCR_REB);
    pe_t_byte(&t, G_END);
    if (!s_buf) s_buf = psx_mod_alloc_guest_memory(PE_TEXT_MAX, 4);
    if (!s_buf) return;
    for (unsigned i = 0; i < t.n; ++i) psx_mod_write_byte(s_buf + i, t.b[i]);
    const uint32_t e = s_ui.entry;
    const unsigned height = (unsigned)t.lines * 12u + 10u;
    const unsigned bottom = (unsigned)s_ui.orig_y + s_ui.orig_h;
    unsigned width = t.widest * 9u + 16u;
    if (width < s_ui.orig_w) width = s_ui.orig_w;
    const unsigned x = psx_mod_read_half(e + TB_X);
    if (x + width > 312u) width = 312u - x;
    psx_mod_write_word(e + TB_MESSAGE, s_buf);
    psx_mod_write_half(e + TB_H, (uint16_t)height);
    psx_mod_write_half(e + TB_Y, (uint16_t)(bottom > height ? bottom - height : 0));
    psx_mod_write_half(e + TB_W, (uint16_t)width);
    psx_mod_write_byte(PE_CHOICE_CURSOR, (uint8_t)cursor);
}

/* ---- input ------------------------------------------------------------------ */
static void pe_list_confirm(int rebuild, int cursor) {
    const int count = rebuild ? pe_rebuild_count() : pe_shop_count();
    int items;
    const int options = pe_list_options(count, &items);
    if (cursor < items) {
        const int i = s_ui.page * PE_PAGE_SIZE + cursor;
        if (rebuild) pe_rebuild_index(i, s_ui.note, sizeof s_ui.note);
        else pe_shop_buy_index(i, s_ui.note, sizeof s_ui.note);
        /* A shorter title: drop the toast's details after the colon. */
        char *colon = strchr(s_ui.note, ':');
        if (colon) *colon = '\0';
        char *paren = strchr(s_ui.note, '(');
        if (paren) *paren = '\0';
        int keep = cursor;
        if (rebuild) {                        /* the list got shorter */
            const int left = pe_rebuild_count() - s_ui.page * PE_PAGE_SIZE;
            if (left <= 0 && s_ui.page > 0) { --s_ui.page; keep = 0; }
            else if (keep >= left) keep = left > 0 ? left - 1 : 0;
        }
        pe_show(rebuild ? SCR_REB : SCR_REQ, keep);
    } else if (pe_pages(count) > 1 && cursor == items) {
        s_ui.page = (s_ui.page + 1) % pe_pages(count);
        s_ui.note[0] = '\0';
        pe_show(rebuild ? SCR_REB : SCR_REQ, 0);
    } else if (cursor == options - 1) {
        s_ui.note[0] = '\0';
        pe_show(SCR_ROOT, rebuild ? 1 : 0);
    }
}

static void pe_reset(void) {
    memset(&s_ui, 0, sizeof s_ui);
}

/* Save states: s_ui rides along (block "pe.wayne.ui"), but our text lives in
 * mod memory, which snapshots don't hold -- so after a load the box is pointed
 * at a freshly written copy of the screen it showed (Wayne's own text is read
 * again from the room, which the snapshot restored). */
static void pe_wayne_state_loaded(int result, uint32_t saved_version, const void *saved,
                                  uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) pe_reset();
}

static void pe_wayne_restored(void) {
    if (s_ui.screen == SCR_NONE) return;
    const uint32_t e = s_ui.entry;
    if (e < PE_TEXTBOX_BASE || e >= PE_TEXTBOX_BASE + PE_TEXTBOX_COUNT * PE_TEXTBOX_STRIDE ||
        psx_mod_read_byte(e + TB_STATE) != 2 || !pe_is_wayne_menu(s_ui.orig_msg)) {
        pe_reset();
        return;
    }
    pe_show(s_ui.screen, (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR));
}

static void pe_on_draw_textboxes(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!psx_mod_game_started() || !psx_mod_read_half(PE_MAX_HP_ADDR)) return;

    /* Still driving our box? */
    if (s_ui.screen != SCR_NONE) {
        const uint32_t e = s_ui.entry;
        const uint32_t msg = psx_mod_read_word(e + TB_MESSAGE);
        if (psx_mod_read_byte(e + TB_STATE) != 2 || (msg != s_buf && msg != s_ui.orig_msg)) {
            pe_reset();                       /* the box closed or moved on */
        } else if (msg == s_ui.orig_msg) {
            pe_show(SCR_WAYNE, (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR));
        }
    }
    if (s_ui.screen == SCR_NONE) {
        if (!pe_loot_enabled()) return;
        for (uint32_t i = 0; i < PE_TEXTBOX_COUNT; ++i) {
            const uint32_t e = PE_TEXTBOX_BASE + i * PE_TEXTBOX_STRIDE;
            if (psx_mod_read_byte(e + TB_STATE) != 2) continue;
            const uint32_t msg = psx_mod_read_word(e + TB_MESSAGE);
            if (msg == s_buf || !pe_is_wayne_menu(msg)) continue;
            s_ui.entry = e;
            s_ui.orig_msg = msg;
            s_ui.orig_y = psx_mod_read_half(e + TB_Y);
            s_ui.orig_h = psx_mod_read_half(e + TB_H);
            s_ui.orig_w = psx_mod_read_half(e + TB_W);
            pe_show(SCR_WAYNE, (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR));
            break;
        }
        if (s_ui.screen == SCR_NONE) return;
    }

    const uint32_t pressed = psx_mod_read_word(PE_PAD_PRESSED);
    const int cursor = (int8_t)psx_mod_read_byte(PE_CHOICE_CURSOR);
    const int workshop = s_ui.wayne_count - 1;
    uint32_t taken = 0;
    switch (s_ui.screen) {
    case SCR_WAYNE:
        if (pressed & PE_IN_CONFIRM) {
            if (cursor == workshop) {
                taken = PE_IN_CONFIRM;
                s_ui.note[0] = '\0';
                pe_show(SCR_ROOT, 0);
            } else if (cursor == workshop + 1) {
                psx_mod_write_byte(PE_CHOICE_CURSOR, (uint8_t)workshop);  /* his Cancel */
            }
        }
        break;
    case SCR_ROOT:
        if (pressed & PE_IN_CONFIRM) {
            taken = PE_IN_CONFIRM;
            s_ui.page = 0;
            s_ui.note[0] = '\0';
            if (cursor == 0) pe_show(SCR_REQ, 0);
            else if (cursor == 1) pe_show(SCR_REB, 0);
            else pe_show(SCR_WAYNE, workshop);
        } else if (pressed & PE_IN_CANCEL) {
            taken = PE_IN_CANCEL;
            pe_show(SCR_WAYNE, workshop);
        }
        break;
    case SCR_REQ:
    case SCR_REB: {
        const int rebuild = s_ui.screen == SCR_REB;
        const int count = rebuild ? pe_rebuild_count() : pe_shop_count();
        if (pressed & PE_IN_CONFIRM) {
            taken = PE_IN_CONFIRM;
            pe_list_confirm(rebuild, cursor);
        } else if (pressed & PE_IN_CANCEL) {
            taken = PE_IN_CANCEL;
            s_ui.note[0] = '\0';
            pe_show(SCR_ROOT, rebuild ? 1 : 0);
        } else if ((pressed & (PE_IN_LEFT | PE_IN_RIGHT)) && pe_pages(count) > 1) {
            const int pages = pe_pages(count);
            s_ui.page = (s_ui.page + ((pressed & PE_IN_RIGHT) ? 1 : pages - 1)) % pages;
            s_ui.note[0] = '\0';
            pe_show(s_ui.screen, 0);
        }
        break;
    }
    default:
        break;
    }
    if (taken) psx_mod_write_word(PE_PAD_PRESSED, pressed & ~taken);
}

/* The field text box knows only normal and red. Menus dim what can't be
 * picked to half brightness (Draw_AllocTexturedQuad: g_DrawColorShaded =
 * (color >> 1) & 0x7F7F7F, so 0x80 -> 0x40). Our lists mark those rows red;
 * right after this frame's text boxes are built (Render_SetGteScreenOffset
 * follows Menu_DrawTextboxEntries in Boot_FrameLoop), their red 12x12 glyph
 * packets -- in the frame's text pool, which starts afresh every frame -- are
 * turned to that gray. Only while one of our lists is up, so the game's own
 * red highlights stay red. */
static void pe_on_after_textboxes(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (s_ui.screen != SCR_REQ && s_ui.screen != SCR_REB) return;
    const uint32_t frame = psx_mod_read_word(PE_DRAW_FRAME) & 1u;
    const uint32_t pool = psx_mod_read_word(PE_TEXT_POOL_TABLE + frame * 4u);
    if (!pe_ram_pointer(pool)) return;
    for (uint32_t i = 0; i < PE_GLYPH_MAX; ++i) {
        const uint32_t p = pool + i * PE_GLYPH_PACKET;
        if (psx_mod_read_half(p + 24u) != 12u || psx_mod_read_half(p + 26u) != 12u) break;
        if (psx_mod_read_byte(p + 12u) == 0xFFu && psx_mod_read_byte(p + 13u) == 0 &&
            psx_mod_read_byte(p + 14u) == 0) {
            psx_mod_write_byte(p + 12u, PE_DIM_GRAY);
            psx_mod_write_byte(p + 13u, PE_DIM_GRAY);
            psx_mod_write_byte(p + 14u, PE_DIM_GRAY);
        }
    }
}

/* Called from pe_mod_menu.c after pe_native_menu_register. */
void pe_wayne_menu_register(void) {
    (void)psx_mod_register_function_entry_plugin("pe.wayne.dim-rows",
        PE_FN_AFTER_TEXTBOXES, pe_on_after_textboxes);
    (void)psx_mod_register_function_entry_plugin("pe.wayne.workshop",
        PE_FN_DRAW_TEXTBOXES, pe_on_draw_textboxes);
    (void)psx_mod_register_state("pe.wayne.ui", 1u, &s_ui, sizeof s_ui, pe_wayne_state_loaded);
    (void)psx_mod_register_restore_callback(pe_wayne_restored);
}
