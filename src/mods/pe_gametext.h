/* Building messages in the game's glyph code for the field text box
 * (g_TextboxEntries), shared by Wayne's Workshop (pe_wayne_menu.c) and the
 * Mod Catalog (pe_catalog_menu.c). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mod_plugins.h"

#define PE_STR_ORIGIN            0x800A8028u
#define PE_STR_DIRECTORY_OFFSET  0x800A802Cu

#define G_NEWLINE 0xF7u
#define G_END     0xFFu
#define G_CODE    0xFBu
#define G_CHOICE  0x09u
#define G_RED     0x04u                       /* our "can't pick" marker, recolored gray */
#define G_NORMAL  0x05u
#define PE_TEXT_MAX  768

static inline int pe_ram_pointer(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }

typedef struct { uint8_t b[PE_TEXT_MAX]; unsigned n; unsigned line; unsigned widest; int lines; } PeText;

static inline void pe_t_byte(PeText *t, uint8_t c) {
    if (t->n < PE_TEXT_MAX - 2) t->b[t->n++] = c;
    if (c == G_NEWLINE) { ++t->lines; t->line = 0; }
    else if (c < 0xF0) { if (++t->line > t->widest) t->widest = t->line; }
}

static inline uint8_t pe_glyph(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x10 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x30 + (c - 'a'));
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    switch (c) {                              /* from the game's own dialogue */
    case '.': return 0x2F; case ',': return 0x4B; case ':': return 0x4A;
    case '!': return 0x2B; case '\'': return 0x2E; case '/': return 0x4C;
    default: return 0x0F;
    }
}

static inline void pe_t_str(PeText *t, const char *s) { while (*s) pe_t_byte(t, pe_glyph(*s++)); }

static inline void pe_t_num(PeText *t, unsigned v) {
    char d[12];
    snprintf(d, sizeof d, "%u", v);
    pe_t_str(t, d);
}

/* Entry `index` of the game's text table at directory offset table_off. */
static inline uint32_t pe_text_entry(uint32_t table_off, unsigned index) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory)) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + table_off);
    if (!pe_ram_pointer(table) || index >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + index * 2u);
}

/* The game's own name for an item (our names for the two relabelled ones). */
static inline void pe_t_item(PeText *t, uint16_t id) {
    if (id == 0x18) { pe_t_str(t, "Smoke Grenade"); return; }
    if (id == 0x19) { pe_t_str(t, "Panacea"); return; }
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

static inline void pe_t_choice(PeText *t, int count) {
    pe_t_byte(t, G_CODE); pe_t_byte(t, G_CHOICE); pe_t_byte(t, (uint8_t)count);
}

static inline void pe_t_option_start(PeText *t, int first) {
    if (!first) pe_t_byte(t, G_NEWLINE);
    pe_t_str(t, "    ");
}
