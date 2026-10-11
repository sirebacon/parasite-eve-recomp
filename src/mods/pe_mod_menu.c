/*
 * In-game mod menu (skeleton).
 *
 * Open/close: hold L1 + R1 + Select. While open: D-pad up/down moves, Cross
 * (or D-pad left/right) toggles, Circle closes.
 *
 * INPUT. The game registers two raw libpad buffers with
 * PadInitDirect(g_AnalogStickState, g_AnalogStickState + 0x22)
 * (Boot_InitSystemFlow.c). Port 1's buttons are the u16 at 0x800BE9A2,
 * active-low (a cleared bit is a pressed button). Verified live 2026-10-10 by
 * injecting Start: 0xFFFF4100 -> 0xFFF74100 for the whole buffer word.
 * While the menu is open the buttons are overwritten with 0xFFFF so the game
 * sees nothing pressed. Whether that mask lands before the game reads the
 * buffer is NOT yet established; it must be checked in play.
 *
 * DRAWING. There is no text API for plugins, so this issues GP0 commands
 * through the runtime's own gpu_write_gp0(): filled rectangles and a built-in
 * 5x7 bitmap font, drawn every VBlank into the buffer currently being
 * displayed. The game's draw mode (E1), draw area (E3/E4) and draw offset (E5)
 * are saved first and restored after, so the game's next primitives are not
 * affected. Risk: a VBlank that lands mid GP0 packet from the guest would
 * corrupt that packet; the runtime exposes no way to detect that.
 *
 * STATE is plain C statics (not guest memory, not in any save structure), so it
 * resets on every launch. Persistence is an open question.
 *
 * Toggles register through pe_menu_register_toggle() (pe_mod_menu.h).
 */
#include <stdint.h>
#include <stddef.h>

#include "mod_plugins.h"
#include "gpu.h"
#include "pe_mod_menu.h"

/* ---- pad ----------------------------------------------------------------- */
#define PE_PAD1_BUFFER_ADDR  0x800BE9A0u
#define PE_PAD1_BUTTONS_ADDR 0x800BE9A2u

#define PE_BTN_SELECT   (1u << 0)
#define PE_BTN_UP       (1u << 4)
#define PE_BTN_RIGHT    (1u << 5)
#define PE_BTN_DOWN     (1u << 6)
#define PE_BTN_LEFT     (1u << 7)
#define PE_BTN_L1       (1u << 10)
#define PE_BTN_R1       (1u << 11)
#define PE_BTN_CIRCLE   (1u << 13)
#define PE_BTN_CROSS    (1u << 14)
#define PE_MENU_COMBO   (PE_BTN_L1 | PE_BTN_R1 | PE_BTN_SELECT)

/* ---- registry ------------------------------------------------------------ */
#define PE_MENU_MAX_ITEMS 16

typedef struct {
    const char    *label;
    int           *flag;
    PeMenuChanged  on_change;
    PeMenuPerFrame per_frame;
} PeMenuItem;

static PeMenuItem s_items[PE_MENU_MAX_ITEMS];
static int        s_item_count;

int pe_menu_register_toggle(const char *label, int *flag,
                            PeMenuChanged on_change, PeMenuPerFrame per_frame) {
    if (!label || !flag || s_item_count >= PE_MENU_MAX_ITEMS) return -1;
    s_items[s_item_count].label     = label;
    s_items[s_item_count].flag      = flag;
    s_items[s_item_count].on_change = on_change;
    s_items[s_item_count].per_frame = per_frame;
    s_item_count++;
    return 0;
}

/* ---- first toggle: faster field movement (live code write) --------------- */
/* Entity_ComputeVelocity (0x80018598): lui $a0,0x0005 (5.0x) -> 0x0014 (20.0x).
 * Written with psx_mod_write_code_word so a restored savestate cannot leave a
 * stale compiled instruction. Only the two known encodings are ever replaced;
 * anything else means another mod owns the word, so it is left alone. */
#define PE_MOVE_ADDR   0x800185C0u
#define PE_MOVE_RETAIL 0x3C040005u
#define PE_MOVE_FAST   0x3C040014u

static int s_move_fast = 1; /* default on: matches the shipped live patch */

static void pe_move_tick(void) {
    const uint32_t want = s_move_fast ? PE_MOVE_FAST : PE_MOVE_RETAIL;
    const uint32_t cur  = psx_mod_read_word(PE_MOVE_ADDR);
    if (cur == want) return;
    if (cur != PE_MOVE_RETAIL && cur != PE_MOVE_FAST) return;
    psx_mod_write_code_word(PE_MOVE_ADDR, want);
}

/* ---- 5x7 font ------------------------------------------------------------ */
typedef struct { char c; uint8_t row[7]; } PeGlyph; /* 5 bits per row, MSB left */

static const PeGlyph s_font[] = {
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'>',{0x10,0x08,0x04,0x02,0x04,0x08,0x10}}, {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {':',{0x00,0x04,0x00,0x00,0x00,0x04,0x00}}, {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}}, {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
};

static const uint8_t *glyph_rows(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++)
        if (s_font[i].c == c) return s_font[i].row;
    return NULL; /* space and anything unknown draws nothing */
}

/* ---- GPU drawing --------------------------------------------------------- */
#define PE_SCALE   2
#define PE_CHAR_W  (6 * PE_SCALE)
#define PE_LINE_H  (10 * PE_SCALE)

#define PE_COL_PANEL  0x301810u /* BGR: dark blue */
#define PE_COL_BORDER 0xC0C0C0u
#define PE_COL_TEXT   0xFFFFFFu
#define PE_COL_CURSOR 0x00FFFFu /* yellow */
#define PE_COL_ON     0x40FF40u
#define PE_COL_OFF    0x8080FFu /* light red */

static int s_base_x, s_base_y; /* VRAM origin of the displayed buffer */

static void rect(int x, int y, int w, int h, uint32_t bgr) {
    if (w <= 0 || h <= 0) return;
    gpu_write_gp0(0x60000000u | (bgr & 0xFFFFFFu));
    gpu_write_gp0(((uint32_t)(y + s_base_y) << 16) | (uint32_t)(x + s_base_x));
    gpu_write_gp0(((uint32_t)h << 16) | (uint32_t)w);
}

static void draw_glyph(int x, int y, char c, uint32_t col) {
    const uint8_t *rows = glyph_rows(c);
    if (!rows) return;
    for (int r = 0; r < 7; r++) {
        int c0 = 0;
        while (c0 < 5) {
            if (!(rows[r] & (0x10u >> c0))) { c0++; continue; }
            int c1 = c0;
            while (c1 < 5 && (rows[r] & (0x10u >> c1))) c1++;
            rect(x + c0 * PE_SCALE, y + r * PE_SCALE, (c1 - c0) * PE_SCALE, PE_SCALE, col);
            c0 = c1;
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col) {
    for (; *s; s++, x += PE_CHAR_W) draw_glyph(x, y, *s, col);
}

static int text_width(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n * PE_CHAR_W;
}

static void draw_menu(int cursor) {
    GpuDisplayInfo di;
    GpuDrawArea da;
    gpu_get_display_info(&di);
    if (di.disabled || di.width < 128 || di.height < 96) return;
    gpu_get_draw_area(&da);
    const uint32_t stat = gpu_read_gpustat();
    s_base_x = (int)di.display_x;
    s_base_y = (int)di.display_y;

    /* Draw into the displayed buffer: allow drawing to the display area, open
     * the clip to all of VRAM, zero the offset. */
    gpu_write_gp0(0xE1000000u | (stat & 0x7FFu) | 0x400u);
    gpu_write_gp0(0xE3000000u);
    gpu_write_gp0(0xE4000000u | (511u << 10) | 1023u);
    gpu_write_gp0(0xE5000000u);

    const int pw = (int)di.width - 16 < 304 ? (int)di.width - 16 : 304;
    const int ph = 12 + PE_LINE_H * (s_item_count + 3);
    const int px = 8, py = 8;
    rect(px - 2, py - 2, pw + 4, ph + 4, PE_COL_BORDER);
    rect(px, py, pw, ph, PE_COL_PANEL);

    int y = py + 6;
    draw_text(px + 8, y, "PE MOD MENU", PE_COL_TEXT);
    y += PE_LINE_H + 4;
    if (s_item_count == 0) draw_text(px + 8, y, "NO TOGGLES", PE_COL_OFF);
    for (int i = 0; i < s_item_count; i++) {
        const int on = *s_items[i].flag != 0;
        if (i == cursor) draw_text(px + 6, y, ">", PE_COL_CURSOR);
        draw_text(px + 6 + PE_CHAR_W, y, s_items[i].label, i == cursor ? PE_COL_CURSOR : PE_COL_TEXT);
        const char *state = on ? "ON" : "OFF";
        draw_text(px + pw - 8 - text_width(state), y, state, on ? PE_COL_ON : PE_COL_OFF);
        y += PE_LINE_H;
    }
    y += 4;
    draw_text(px + 8, y, "X TOGGLE  O CLOSE", PE_COL_TEXT);

    /* Put the game's draw environment back. */
    gpu_write_gp0(0xE1000000u | (stat & 0x7FFu) | (((stat >> 15) & 1u) << 11));
    gpu_write_gp0(0xE3000000u | ((da.top & 0x3FFu) << 10) | (da.left & 0x3FFu));
    gpu_write_gp0(0xE4000000u | ((da.bottom & 0x3FFu) << 10) | (da.right & 0x3FFu));
    gpu_write_gp0(0xE5000000u | (((uint32_t)da.offset_y & 0x7FFu) << 11) | ((uint32_t)da.offset_x & 0x7FFu));
}

/* ---- per-frame ----------------------------------------------------------- */
static int      s_open;
static int      s_cursor;
static uint16_t s_prev_pressed;

/* The raw pad buffer is all zero until the game's libpad initialises it, and an
 * all-zero buttons word means "every button pressed" (active-low). Trust the
 * buttons only once the buffer carries a real controller header: byte 0 is the
 * status (0 = ok) and byte 1 the pad id (0x41 digital, 0x73 analog, ...). */
static int s_pad_seen;

static int pad_header_valid(void) {
    const uint32_t w  = psx_mod_read_word(PE_PAD1_BUFFER_ADDR);
    const uint32_t id = (w >> 8) & 0xFFu;
    return (w & 0xFFu) == 0x00u && id != 0x00u && id != 0xFFu;
}

static void pe_menu_vblank(void) {
    if (!psx_mod_game_started()) return;

    uint16_t edge = 0;
    int combo_edge = 0;
    uint16_t pressed = 0;
    if (pad_header_valid()) {
        pressed = (uint16_t)(~psx_mod_read_half(PE_PAD1_BUTTONS_ADDR));
        if (!s_pad_seen) {
            /* First valid sample: just record it, so held buttons are not edges. */
            s_pad_seen = 1;
            s_prev_pressed = pressed;
        }
        edge = (uint16_t)(pressed & ~s_prev_pressed);
        combo_edge = ((pressed & PE_MENU_COMBO) == PE_MENU_COMBO) &&
                     ((s_prev_pressed & PE_MENU_COMBO) != PE_MENU_COMBO);
        s_prev_pressed = pressed;
    } else {
        s_pad_seen = 0;
        s_prev_pressed = 0;
    }

    if (combo_edge) s_open = !s_open;

    if (s_open && s_item_count > 0) {
        if (edge & PE_BTN_UP)   s_cursor = (s_cursor + s_item_count - 1) % s_item_count;
        if (edge & PE_BTN_DOWN) s_cursor = (s_cursor + 1) % s_item_count;
        if (edge & (PE_BTN_CROSS | PE_BTN_LEFT | PE_BTN_RIGHT)) {
            PeMenuItem *it = &s_items[s_cursor];
            *it->flag = !*it->flag;
            if (it->on_change) it->on_change(*it->flag);
        }
    }
    if (s_open && (edge & PE_BTN_CIRCLE)) s_open = 0;

    for (int i = 0; i < s_item_count; i++)
        if (s_items[i].per_frame) s_items[i].per_frame();

    if (s_open) {
        psx_mod_write_half(PE_PAD1_BUTTONS_ADDR, 0xFFFFu); /* game sees no buttons */
        draw_menu(s_cursor);
    }
}

PSX_MOD_CONSTRUCTOR(pe_register_mod_menu_plugin) {
    pe_menu_register_toggle("FASTER MOVEMENT 20X", &s_move_fast, NULL, pe_move_tick);
    (void)psx_mod_register_vblank_plugin("pe.mod-menu", pe_menu_vblank);
}
