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
 *     our copy with "Workshop" above Cancel and "Discard junk <your Junk>,
 *     <his count>/300" (plus "bag full" when a hand-in has no free slot, or
 *     "Day 4" when he holds 300 but can't build yet),
 *     one line taller;
 *   - confirm on Workshop is taken away from the game (the press is cleared)
 *     and the box shows our screens instead: Workshop (Requisition / Rebuild /
 *     Back), Requisition (goods with prices, then the economy mods not yet
 *     unlocked, 5 a page, in the game's dimmed
 *     gray when they can't be bought now) and Rebuild (dismantled gear with prices). The title line
 *     shows your Junk or what the last purchase did. Cancel (Circle) goes back
 *     a screen, Left/Right turn pages;
 *   - Mod Permit (above Workshop, 2026-10-14): the bag's guns and armor with
 *     their slots (n/max); picking one asks Yes / No, and only Yes spends a
 *     permit for one more slot (vanilla's Torres tune-up, without the risk);
 *   - confirm on our Cancel moves the cursor up two first, so the script gets
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
#include "pe_gametext.h"
#include "pe_log.h"

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
#define TB_STYLE   0x08u
#define TB_BACKGROUND 0x09u
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
#define PE_PAGE_SIZE 5
#define PE_AYA_SLOTS_ADDR        0x800C0E48u
#define PE_SLOT_COUNT_ADDR       0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR      0x8009D018u
#define PE_STORAGE_ADDR          0x800C1EB8u   /* Wayne's storage, 100 slots */
#define PE_EQUIP_RECORDS_ADDR    0x800C0EACu   /* bag value 0x100 + r -> record r */
#define PE_ITEM_MOD_PERMIT       0x37u
#define REC_SLOT_MAX  0x01u                    /* tailCapacity: the item's own slot maximum */
#define REC_ITEM_ID   0x04u
#define REC_KIND      0x06u
#define REC_SLOTS     0x14u                    /* tailCount: mod slots */
#define REC_MODS      0x15u
enum { SCR_NONE, SCR_WAYNE, SCR_ROOT, SCR_REQ, SCR_REB, SCR_PERMIT, SCR_PERMIT_ASK };

static struct {
    int screen;
    uint32_t entry;                           /* the text box we drive */
    uint32_t orig_msg;                        /* Wayne's own message */
    uint16_t orig_y, orig_h, orig_w;
    int wayne_count;                          /* options in Wayne's own menu */
    int page;
    char note[48];                            /* title line after an action */
    int pick;                                 /* Mod Permit: the bag slot being asked about */
} s_ui;

static uint32_t s_buf;                        /* our message, mod memory */
static uint8_t s_orig[PE_TEXT_MAX];           /* copy of Wayne's message */
static unsigned s_orig_len;

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
    const int warner = msg && msg == pe_field_wayne_text();   /* Warner's (pe_field_wayne.c) */
    if (!pe_ram_pointer(msg) && !warner) return 0;
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
    if (!warner && !pe_contains(b, n, s_discard_junk)) return 0;
    const int count = b[choice_at] & 7;
    if (count < 2 || count + 1 > 7) return 0; /* room for Workshop (and Mod Permit, see pe_compact) */
    memcpy(s_orig, b, n + 1);
    s_orig_len = n;
    s_ui.wayne_count = count;
    return 1;
}

/* A choice holds at most 7 options (FB 09 n, 3 bits). Wayne's Chrysler menu has 6
 * (Rare T Card collection), so there only Workshop is added and Mod Permit moves
 * into the Workshop screen. */
static int pe_compact(void) { return s_ui.wayne_count + 2 > 7; }

/* Wayne's options with Workshop above Cancel and the Junk on Discard junk. */
static void pe_build_wayne(PeText *t) {
    unsigned i = 0;
    while (i < s_orig_len && !(s_orig[i] == G_CODE && i + 1 < s_orig_len && s_orig[i + 1] == G_CHOICE)) {
        pe_t_byte(t, s_orig[i]);
        ++i;
    }
    i += 3;                                   /* FB 09 n */
    pe_t_choice(t, s_ui.wayne_count + (pe_compact() ? 1 : 2));
    int option = 0;
    unsigned start = i;
    for (; i <= s_orig_len; ++i) {
        if (i < s_orig_len && s_orig[i] != G_NEWLINE) continue;
        if (option == s_ui.wayne_count - 1) { /* the last one is Cancel */
            if (!pe_compact()) {
                pe_t_str(t, "    Mod Permit");
                pe_t_byte(t, G_NEWLINE);
            }
            pe_t_str(t, "    Workshop");
            pe_t_byte(t, G_NEWLINE);
        }
        for (unsigned k = start; k < i; ++k) pe_t_byte(t, s_orig[k]);
        if (pe_contains(s_orig + start, i - start, s_discard_junk)) {
            /* your Junk, then his count toward the next gun */
            pe_t_str(t, " ");
            pe_t_num(t, (unsigned)pe_loot_junk());
            pe_t_str(t, ", ");
            pe_t_num(t, psx_mod_read_half(PE_WAYNE_COUNTER));
            pe_t_str(t, "/300");
            if (pe_loot_handin_blocked()) pe_t_str(t, " bag full");     /* needs one free slot */
            else if (pe_loot_wayne_waiting()) pe_t_str(t, " Day 4");    /* he builds from Day 4 */
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
    pe_t_choice(t, pe_compact() ? 4 : 3);
    pe_t_option_start(t, 1); pe_t_str(t, "Requisition");
    pe_t_option_start(t, 0); pe_t_str(t, "Rebuild");
    if (pe_compact()) { pe_t_option_start(t, 0); pe_t_str(t, "Mod Permit"); }
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
        const char *label = rebuild ? 0 : pe_shop_label(i);
        if (label) pe_t_str(t, label);        /* an economy mod */
        else pe_t_item(t, rebuild ? pe_rebuild_item(i) : pe_shop_item(i));
        if (rebuild && pe_rebuild_bought_slots(i)) {   /* built up with Mod Permits */
            pe_t_str(t, " +");
            pe_t_num(t, pe_rebuild_bought_slots(i));
            pe_t_str(t, " slots");
        }
        pe_t_str(t, "  ");
        pe_t_num(t, (unsigned)(rebuild ? pe_rebuild_price_index(i) : pe_shop_price_index(i)));
        if (red) { pe_t_byte(t, G_CODE); pe_t_byte(t, G_NORMAL); }
    }
    if (pe_pages(count) > 1) { pe_t_option_start(t, items == 0); pe_t_str(t, "Next page"); }
    pe_t_option_start(t, items == 0 && pe_pages(count) == 1);
    pe_t_str(t, "Back");
}

/* ---- Mod Permit: a slot more on a gun or armor ---------------------------------- */
/* Vanilla: show Torres a Mod Permit and an item gets one more mod slot
 * (Menu_ItemUseAction: tailCount + 1, up to the item's own tailCapacity). Here
 * it is a list of the guns and armor in the bag and a Yes / No question; the
 * permit is only spent on Yes. */
static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

static uint32_t pe_gear_record(int bag_slot) {
    if (bag_slot < 0 || (uint32_t)bag_slot >= pe_bag_limit()) return 0;
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)bag_slot * 2u);
    if (v < 0x100u || v >= 0x180u) return 0;
    const uint32_t rec = PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 0x100u) * 0x20u;
    const unsigned kind = psx_mod_read_byte(rec + REC_KIND);
    return kind >= 1u && kind <= 9u ? rec : 0;
}

/* Bag slots holding guns and armor, in bag order. */
static int pe_gear_list(int *out) {
    int n = 0;
    for (uint32_t i = 0; i < pe_bag_limit(); ++i)
        if (pe_gear_record((int)i)) out[n++] = (int)i;
    return n;
}

static unsigned pe_permits(void) {
    unsigned n = 0;
    for (uint32_t i = 0; i < 50u; ++i) n += psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == PE_ITEM_MOD_PERMIT;
    for (uint32_t i = 0; i < 100u; ++i) n += psx_mod_read_half(PE_STORAGE_ADDR + i * 2u) == PE_ITEM_MOD_PERMIT;
    return n;
}

static int pe_take_permit(void) {                /* the bag's first, else storage */
    for (uint32_t i = 0; i < 150u; ++i) {
        const uint32_t a = i < 50u ? PE_AYA_SLOTS_ADDR + i * 2u : PE_STORAGE_ADDR + (i - 50u) * 2u;
        if (psx_mod_read_half(a) == PE_ITEM_MOD_PERMIT) { psx_mod_write_half(a, 0); return 1; }
    }
    return 0;
}

static int pe_at_max(uint32_t rec) {
    const unsigned slots = psx_mod_read_byte(rec + REC_SLOTS), cap = psx_mod_read_byte(rec + REC_SLOT_MAX);
    return slots >= cap || slots >= 10u;
}

static void pe_build_permit_list(PeText *t) {
    int gear[50];
    const int count = pe_gear_list(gear);
    int items;
    const int options = pe_list_options(count, &items);
    if (s_ui.note[0]) pe_t_str(t, s_ui.note);
    else if (!count) pe_t_str(t, "Mod Permit: no guns or armor");
    else { pe_t_str(t, "Mod Permit   have "); pe_t_num(t, pe_permits()); }
    pe_t_byte(t, G_NEWLINE);
    pe_t_choice(t, options);
    const int none = pe_permits() == 0;
    for (int k = 0; k < items; ++k) {
        const uint32_t rec = pe_gear_record(gear[s_ui.page * PE_PAGE_SIZE + k]);
        pe_t_option_start(t, k == 0);
        const int red = none || pe_at_max(rec);
        if (red) { pe_t_byte(t, G_CODE); pe_t_byte(t, G_RED); }
        pe_t_item(t, psx_mod_read_byte(rec + REC_ITEM_ID));
        pe_t_str(t, "  ");
        pe_t_num(t, psx_mod_read_byte(rec + REC_SLOTS));
        pe_t_str(t, "/");
        pe_t_num(t, psx_mod_read_byte(rec + REC_SLOT_MAX) < 10u ? psx_mod_read_byte(rec + REC_SLOT_MAX) : 10u);
        if (red) { pe_t_byte(t, G_CODE); pe_t_byte(t, G_NORMAL); }
    }
    if (pe_pages(count) > 1) { pe_t_option_start(t, items == 0); pe_t_str(t, "Next page"); }
    pe_t_option_start(t, items == 0 && pe_pages(count) == 1);
    pe_t_str(t, "Back");
}

static void pe_build_permit_ask(PeText *t) {
    const uint32_t rec = pe_gear_record(s_ui.pick);
    pe_t_str(t, "Add a slot to ");
    if (rec) pe_t_item(t, psx_mod_read_byte(rec + REC_ITEM_ID));
    pe_t_str(t, "?");
    pe_t_byte(t, G_NEWLINE);
    pe_t_str(t, "Uses 1 Mod Permit, you have ");
    pe_t_num(t, pe_permits());
    pe_t_byte(t, G_NEWLINE);
    pe_t_choice(t, 2);
    pe_t_option_start(t, 1); pe_t_str(t, "Yes");
    pe_t_option_start(t, 0); pe_t_str(t, "No");
}

/* Write the current screen into the box, sized to fit. */
static void pe_show(int screen, int cursor) {
    PeText t;
    memset(&t, 0, sizeof t);
    t.lines = 1;
    s_ui.screen = screen;
    if (screen == SCR_WAYNE) pe_build_wayne(&t);
    else if (screen == SCR_ROOT) pe_build_root(&t);
    else if (screen == SCR_PERMIT) pe_build_permit_list(&t);
    else if (screen == SCR_PERMIT_ASK) pe_build_permit_ask(&t);
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
        {                                     /* the list may have got shorter */
            const int left = (rebuild ? pe_rebuild_count() : pe_shop_count()) - s_ui.page * PE_PAGE_SIZE;
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
        if (pe_field_wayne_box_ours()) {       /* Warner's menu: his box shows our text */
            for (uint32_t i = 0; i < PE_TEXTBOX_COUNT; ++i) {
                const uint32_t e = PE_TEXTBOX_BASE + i * PE_TEXTBOX_STRIDE;
                if (psx_mod_read_byte(e + TB_STATE) == 0) continue;
                const uint32_t msg = psx_mod_read_word(e + TB_MESSAGE);
                if (msg && msg != s_buf && msg != pe_field_wayne_text()) {
                    psx_mod_write_word(e + TB_MESSAGE, pe_field_wayne_text());
                    /* his line is style 0, drawn at a fixed spot near the bottom (0x14, 0xAE);
                     * a menu needs style 1: a window at the box's own x / y, like Wayne's */
                    psx_mod_write_byte(e + TB_STYLE, 1);
                    psx_mod_write_byte(e + TB_BACKGROUND, 0);
                }
            }
        }
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
    /* our options sit where his Cancel was: Mod Permit, Workshop (compact: Workshop only) */
    const int permit = pe_compact() ? -1 : s_ui.wayne_count - 1;
    const int workshop = s_ui.wayne_count - (pe_compact() ? 1 : 0);
    const int his_cancel = s_ui.wayne_count - 1;
    uint32_t taken = 0;
    switch (s_ui.screen) {
    case SCR_WAYNE:
        if (pressed & PE_IN_CONFIRM) {
            if (cursor == permit) {
                taken = PE_IN_CONFIRM;
                s_ui.note[0] = '\0';
                s_ui.page = 0;
                pe_show(SCR_PERMIT, 0);
            } else if (cursor == workshop) {
                taken = PE_IN_CONFIRM;
                s_ui.note[0] = '\0';
                pe_show(SCR_ROOT, 0);
            } else if (cursor == workshop + 1) {
                psx_mod_write_byte(PE_CHOICE_CURSOR, (uint8_t)his_cancel);
            }
        }
        break;
    case SCR_PERMIT: {
        int gear[50];
        const int count = pe_gear_list(gear);
        int items;
        const int options = pe_list_options(count, &items);
        if (pressed & PE_IN_CONFIRM) {
            taken = PE_IN_CONFIRM;
            if (cursor < items) {
                const int slot = gear[s_ui.page * PE_PAGE_SIZE + cursor];
                const uint32_t rec = pe_gear_record(slot);
                if (!pe_permits()) { snprintf(s_ui.note, sizeof s_ui.note, "No Mod Permit"); pe_show(SCR_PERMIT, cursor); }
                else if (!rec || pe_at_max(rec)) { snprintf(s_ui.note, sizeof s_ui.note, "That one has all its slots"); pe_show(SCR_PERMIT, cursor); }
                else { s_ui.pick = slot; s_ui.note[0] = '\0'; pe_show(SCR_PERMIT_ASK, 1); }   /* on No */
            } else if (pe_pages(count) > 1 && cursor == items) {
                s_ui.page = (s_ui.page + 1) % pe_pages(count);
                s_ui.note[0] = '\0';
                pe_show(SCR_PERMIT, 0);
            } else if (cursor == options - 1) {
                s_ui.note[0] = '\0';
                if (pe_compact()) pe_show(SCR_ROOT, 2); else pe_show(SCR_WAYNE, permit);
            }
        } else if (pressed & PE_IN_CANCEL) {
            taken = PE_IN_CANCEL;
            s_ui.note[0] = '\0';
            if (pe_compact()) pe_show(SCR_ROOT, 2); else pe_show(SCR_WAYNE, permit);
        } else if ((pressed & (PE_IN_LEFT | PE_IN_RIGHT)) && pe_pages(count) > 1) {
            const int pages = pe_pages(count);
            s_ui.page = (s_ui.page + ((pressed & PE_IN_RIGHT) ? 1 : pages - 1)) % pages;
            s_ui.note[0] = '\0';
            pe_show(SCR_PERMIT, 0);
        }
        break;
    }
    case SCR_PERMIT_ASK: {
        int gear[50];
        const int count = pe_gear_list(gear);
        int back = 0;                          /* the picked item's row on the list */
        for (int i = 0; i < count; ++i) if (gear[i] == s_ui.pick) back = i - s_ui.page * PE_PAGE_SIZE;
        if (back < 0 || back >= PE_PAGE_SIZE) back = 0;
        if (pressed & PE_IN_CONFIRM) {
            taken = PE_IN_CONFIRM;
            const uint32_t rec = pe_gear_record(s_ui.pick);
            if (cursor == 0 && rec && !pe_at_max(rec) && pe_take_permit()) {
                const unsigned slots = psx_mod_read_byte(rec + REC_SLOTS);
                psx_mod_write_byte(rec + REC_MODS + slots, 0);     /* the new slot starts empty */
                psx_mod_write_byte(rec + REC_SLOTS, (uint8_t)(slots + 1u));
                snprintf(s_ui.note, sizeof s_ui.note, "Slot added, now %u", slots + 1u);
                pe_log("permit_slot", "item=0x%02X slots=%u permits_left=%u",
                       (unsigned)psx_mod_read_byte(rec + REC_ITEM_ID), slots + 1u, pe_permits());
            } else {
                s_ui.note[0] = '\0';
            }
            pe_show(SCR_PERMIT, back);
        } else if (pressed & PE_IN_CANCEL) {
            taken = PE_IN_CANCEL;
            s_ui.note[0] = '\0';
            pe_show(SCR_PERMIT, back);
        }
        break;
    }
    case SCR_ROOT:
        if (pressed & PE_IN_CONFIRM) {
            taken = PE_IN_CONFIRM;
            s_ui.page = 0;
            s_ui.note[0] = '\0';
            if (cursor == 0) pe_show(SCR_REQ, 0);
            else if (cursor == 1) pe_show(SCR_REB, 0);
            else if (cursor == 2 && pe_compact()) { s_ui.page = 0; pe_show(SCR_PERMIT, 0); }
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
    if (s_ui.screen != SCR_REQ && s_ui.screen != SCR_REB && s_ui.screen != SCR_PERMIT) return;
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
    (void)psx_mod_register_state("pe.wayne.ui", 2u, &s_ui, sizeof s_ui, pe_wayne_state_loaded);
    (void)psx_mod_register_restore_callback(pe_wayne_restored);
}
