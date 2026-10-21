/*
 * Collection: the Bestiary and the Wardrobe (bridge findings pe-skins-bestiary-design-2026-10-19.md).
 * F1 > PE Enhancements > Collection. Everything here is cosmetic or information: it never changes
 * a fight.
 *
 * Unlocks are PERMANENT across games: they live in pe_collection.dat beside the game (written
 * with fopen, like the play log), not in the memory card save or save states, so a new game, an
 * older save or the EX Game keeps them. Junk is spent from the running game's counter
 * (pe_loot_spend_junk), and the unlock is written to the file at once.
 *
 * Bestiary (75 creatures, story and Chrysler; data generated from the bridge's Bestiary draft,
 * pe_collection_data.h): per creature
 *   basics   (name, HP, defense, where)  first kill, or any purchase;
 *   weakness (weak / strong elements)    casting Scan on it, 5 kills (bosses: defeat), or Junk;
 *   loot     (drop and steal odds)       15 kills, or Junk (bosses: Junk only).
 * Kills: every battle frame the enemy slots (0x800A5D58, 7 x 0xDC: u8 active, EnemyCombatant at +4)
 * are watched; an enemy whose HP (+0x10) goes from above 0 to 0 or less is a kill of the creature
 * found by (map, identity +0x04). Scan: Battle_ApplySpellEffect (0x80024250) with power 7, a1 =
 * the target. The map comes from g_SceneDispatchToken (0x8009D280, base 32, as the play log).
 *
 * Wardrobe: Aya's outfits are the game's own player bundles, picked by g_SceneAreaType
 * (0x800B0CE2, = g_GameState.requested_entity_bank): the room sets it when it loads, and the
 * player's model, textures and per-day animation rows (Scene_LoadEntityTexture / Scene_LoadEntity
 * Textures) all follow it. Wearing an outfit rewrites it from 10 / 12 / 13 (the gameplay outfits)
 * at the entry of the room load's texture step; character skins replace the bundle at the same
 * steps (see "Wardrobe: costumes" below). 11 (Day 1 long dress) and 14 (transformed form) have no
 * full player moveset and are not offered. Character skins are listed but not sold until their
 * bundles are built. F1 pages: a filter row, an item row browsed with left / right, details, one
 * action.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"
#include "pe_collection_data.h"

#define PE_FN_APPLY_SPELL     0x80024250u
#define PE_PLAYER_ENTITY      0x8009D254u
#define PE_GAME_BLOCK         0x8009D1A0u   /* & 2: in battle */
#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_ROOM_TOKEN         0x8009D280u
#define PE_BASE32_TABLE       0x800930B4u
#define PE_AREA_TYPE          0x800B0CE2u
#define PE_ENEMY_SLOT_BASE    0x800A5D58u
#define PE_ENEMY_SLOT_STRIDE  0xDCu
#define PE_ENEMY_SLOTS        7
#define E_IDENTITY            0x04u
#define E_HP                  0x10u

#define PE_FILE        "pe_collection.dat"
#define PE_MAGIC       0x4C434550u         /* "PECL" */
#define PE_VERSION     1u
enum { C_MET = 1u, C_WEAK = 2u, C_LOOT = 4u };   /* bits 0-2; kills in bits 3-7 (max 31) */

typedef struct PeProfile {
    uint32_t magic, version;
    uint64_t skins;                        /* bit i: g_pe_skins[i] owned */
    uint8_t worn, reserved[7];
    uint8_t creature[128];
    uint32_t checksum;
} PeProfile;
static PeProfile s_prof;
static int s_dirty;

static int pe_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x80200000u; }
static int pe_loaded(void) { return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0; }

/* ---- profile file --------------------------------------------------------------------- */
static uint32_t pe_sum(void) {
    const uint8_t *p = (const uint8_t *)&s_prof; uint32_t s = 0x5A17C0DEu;
    for (size_t i = 0; i < offsetof(PeProfile, checksum); ++i) s = (s << 5) + s + p[i];
    return s;
}
static void pe_profile_defaults(void) {
    memset(&s_prof, 0, sizeof s_prof);
    s_prof.magic = PE_MAGIC; s_prof.version = PE_VERSION;
    s_prof.skins = 1u;                      /* Aya */
}
static void pe_profile_load(void) {
    pe_profile_defaults();
    FILE *f = fopen(PE_FILE, "rb");
    if (!f) return;
    PeProfile tmp;
    const size_t n = fread(&tmp, 1, sizeof tmp, f);
    fclose(f);
    if (n != sizeof tmp || tmp.magic != PE_MAGIC || tmp.version != PE_VERSION) return;
    const PeProfile keep = s_prof;
    s_prof = tmp;
    if (pe_sum() != s_prof.checksum) s_prof = keep;   /* damaged: start over rather than trust it */
    s_prof.skins |= 1u;
    if (s_prof.worn >= PE_SKINS) s_prof.worn = 0;
}
static void pe_profile_save(void) {
    s_prof.checksum = pe_sum();
    FILE *f = fopen(PE_FILE, "wb");
    if (!f) return;
    fwrite(&s_prof, 1, sizeof s_prof, f);
    fclose(f);
    s_dirty = 0;
}

/* ---- creatures -------------------------------------------------------------------------- */
static int pe_current_map(void) {
    const uint32_t token = psx_mod_read_word(PE_ROOM_TOKEN);
    int map = 0;
    for (int i = 2; i < 5; ++i) {
        const char c = (char)psx_mod_read_byte(PE_BASE32_TABLE + ((token >> ((5 - i) * 5 + 2)) & 0x1Fu));
        if (c < '0' || c > '9') return -1;
        map = map * 10 + (c - '0');
    }
    return map;
}
static int pe_creature_of(int map, unsigned identity) {
    int lo = 0, hi = PE_CREATURE_KEYS - 1;      /* keys are sorted by (map, id) */
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const PeCreatureKey *k = &g_pe_creature_keys[mid];
        if (k->map == map && k->id == identity) return k->creature;
        if (k->map < map || (k->map == map && k->id < identity)) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static unsigned pe_kills(int c) { return s_prof.creature[c] >> 3; }
static void pe_unlock(int c, uint8_t bits, const char *why) {
    const uint8_t old = s_prof.creature[c];
    const uint8_t now = (uint8_t)(old | bits | C_MET);
    if (now == old) return;
    s_prof.creature[c] = now;
    s_dirty = 1;
    char msg[96];
    const uint8_t gained = (uint8_t)(now & ~old);
    snprintf(msg, sizeof msg, "Bestiary: %s%s%s%s", g_pe_creatures[c].name,
             gained & C_WEAK ? " - weaknesses" : "", gained & C_LOOT ? " - loot" : "",
             gained == C_MET ? " - new entry" : "");
    psx_mod_osd_message(msg);
    pe_log("bestiary", "creature=%s bits=0x%02X why=%s", g_pe_creatures[c].name, now & 7u, why);
}
static void pe_kill(int c) {
    unsigned k = pe_kills(c);
    if (k < 31u) { ++k; s_prof.creature[c] = (uint8_t)((s_prof.creature[c] & 7u) | (k << 3)); s_dirty = 1; }
    uint8_t bits = C_MET;
    if (g_pe_creatures[c].boss) bits |= C_WEAK;            /* defeated: its weaknesses are known */
    else { if (k >= 5u) bits |= C_WEAK; if (k >= 15u) bits |= C_LOOT; }
    pe_unlock(c, bits, "kill");
}

/* Kills: watch the enemy slots during battle. */
static struct { uint8_t tracking; uint8_t entity_id; uint16_t identity; int32_t hp; } s_slot[PE_ENEMY_SLOTS];
static void pe_watch_slots(void) {
    const int map = pe_current_map();
    for (int s = 0; s < PE_ENEMY_SLOTS; ++s) {
        const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)s * PE_ENEMY_SLOT_STRIDE;
        const uint32_t core = slot + 4u;
        if (!psx_mod_read_byte(slot)) { s_slot[s].tracking = 0; continue; }
        const uint8_t eid = psx_mod_read_byte(core + 0x07u);
        const int32_t hp = (int32_t)psx_mod_read_word(core + E_HP);
        const uint16_t id = psx_mod_read_half(core + E_IDENTITY);
        if (!s_slot[s].tracking || s_slot[s].entity_id != eid) {
            s_slot[s].tracking = 1; s_slot[s].entity_id = eid; s_slot[s].hp = hp; s_slot[s].identity = id;
            continue;
        }
        if (s_slot[s].hp > 0 && hp <= 0 && map > 0) {
            const int c = pe_creature_of(map, s_slot[s].identity ? s_slot[s].identity : id);
            if (c >= 0) pe_kill(c);
        }
        s_slot[s].hp = hp;
        if (id) s_slot[s].identity = id;
    }
}

/* Scan: Battle_ApplySpellEffect(7, target). */
static void pe_on_spell(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (cpu->gpr[4] != 7u || !pe_loaded()) return;
    const uint32_t target = cpu->gpr[5];
    if (!pe_ptr(target) || target == psx_mod_read_word(PE_PLAYER_ENTITY)) return;
    const uint32_t core = psx_mod_read_word(target);
    if (!pe_ptr(core)) return;
    const int c = pe_creature_of(pe_current_map(), psx_mod_read_half(core + E_IDENTITY));
    if (c >= 0) pe_unlock(c, C_WEAK, "scan");
}

/* ---- Wardrobe: costumes ---------------------------------------------------------------------- *
 * Rooms load the player through Draw_InitPipeline (draw/pipeline.c): Scene_LoadEntityTexture
 * (texture blob from PE image entry area+3 into the scratch buffer, uploaded to VRAM in phase 3;
 * then the bundle, entry area+8, into the voice buffer +0x154 and its pointers resolved in phase 6),
 * then Scene_LoadEntityTextures (per-day rows into +0x158, pointers resolved in subphase 6). Both
 * run in tight loops, so the costume goes in from function-entry hooks, never from the VBlank tick:
 *   - Scene_LoadEntityTexture entry, phase 0: a worn outfit rewrites g_SceneAreaType (10 / 12 / 13
 *     only) and forces the reload if the loaded bundle differs. A worn character skin puts its
 *     texture.bin in the scratch buffer and starts the loader at phase 3, so the game uploads our
 *     TIMs and then reads its own bundle (which we replace next). Not during Liberate (flags
 *     0x20000: the transformation's own sequence waits in phase 2).
 *   - Scene_LoadEntityTextures entry (the next call of the room load): model.bin goes over the
 *     bundle and bank_slots[0] / bank_rows point into it; the day blob goes into +0x158 and the
 *     loader starts at subphase 6, so the game resolves our rows (and stream tables) itself.
 * Taking a costume off forces the game's own bundle and rows back (flags 0x200000, day flags 1).
 * Wearing from F1 asks for the game's same-room reload (Scene_SelectMapById: g_GameStateFlags
 * |= 0x2000 and flags |= 0x800 end the frame loop and the room loads again), so it shows as soon
 * as the menu closes. (Until 2026-10-20 this keyed on Scene_InitEntityPlayer's steps 0x20-0x27,
 * which only Liberate runs, and the outfit byte was rewritten from the VBlank tick, racing the
 * room load: character skins never went in and outfits only sometimes.) */
#define PE_GS          0x800B0CD8u
#define GS_FLAGS       0x000u
#define GS_LOADED_BANK 0x00Bu               /* g_SavedSceneAreaType */
#define GS_DAY         0x00Cu
#define GS_DAYFLAGS    0x00Eu
#define GS_ETPHASE     0x0ECu
#define GS_SSUB        0x0EEu
#define GS_VOICE       0x154u
#define GS_ETEX        0x158u
#define GS_SCRATCH     0x194u
#define GS_SLOTS       0x198u
#define GS_ROWS        0x1C0u
#define PE_FN_ENTITY_TEX   0x8006BECCu      /* Scene_LoadEntityTexture */
#define PE_FN_ENTITY_TEXS  0x8006C5BCu      /* Scene_LoadEntityTextures */
#define PE_MODEL_BUF_MAX 0xE000u
typedef struct { uint8_t *data; uint32_t size; } PeBlob;
static struct { int loaded, ok; PeBlob model, tex, day[9]; } s_bundle[PE_SKINS];
static int s_have = -2;                     /* in the buffers: -2 unknown, -1 the game's, else a skin */
static int s_pending = -1;                  /* skin whose model / rows go in at the next row load */

static int pe_read_file(const char *path, PeBlob *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 0x40000) { fclose(f); return 0; }
    b->data = (uint8_t *)malloc((size_t)n); b->size = (uint32_t)n;
    const size_t got = b->data ? fread(b->data, 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}
static int pe_bundle(int i) {
    if (i < 0 || i >= PE_SKINS || g_pe_skins[i].kind != 1 || !g_pe_skins[i].folder[0]) return 0;
    if (s_bundle[i].loaded) return s_bundle[i].ok;
    s_bundle[i].loaded = 1;
    char p[160];
    snprintf(p, sizeof p, "pe_skins/%s/model.bin", g_pe_skins[i].folder);
    if (!pe_read_file(p, &s_bundle[i].model) || s_bundle[i].model.size > PE_MODEL_BUF_MAX) return 0;
    snprintf(p, sizeof p, "pe_skins/%s/texture.bin", g_pe_skins[i].folder);
    if (!pe_read_file(p, &s_bundle[i].tex)) return 0;
    for (int d = 1; d <= 8; ++d) {
        snprintf(p, sizeof p, "pe_skins/%s/day%d.bin", g_pe_skins[i].folder, d);
        if (!pe_read_file(p, &s_bundle[i].day[d])) return 0;
    }
    return s_bundle[i].ok = 1;
}
static uint32_t rd32(const uint8_t *b, uint32_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; }
static void pe_put_blob(uint32_t dst, const PeBlob *b) {
    for (uint32_t i = 0; i + 4 <= b->size; i += 4) psx_mod_write_word(dst + i, rd32(b->data, i));
}
/* Point bank_slots[0] / bank_rows[0][id] at a bundle written to `base` (SceneAssetBlob: directory
 * at word 1; +0x0C model records, +0x10 row records; records 12 bytes, word 1 = offset | id << 24),
 * as Scene_LoadEntityTexture phase 6 does. */
static void pe_point_bundle(uint32_t base, const PeBlob *b) {
    const uint32_t dir = rd32(b->data, 4);
    const uint32_t mdesc = rd32(b->data, dir + 0x0C), mat = mdesc & 0x3FFFFFu;
    if (mdesc >> 22) psx_mod_write_word(PE_GS + GS_SLOTS, base + (rd32(b->data, mat + 4) & 0xFFFFFFu));
    const uint32_t desc = rd32(b->data, dir + 0x10), at = desc & 0x3FFFFFu;
    for (uint32_t i = 0; i < (desc >> 22); ++i) {
        const uint32_t w = rd32(b->data, at + 12u * i + 4u);
        if ((w >> 24) < 48u) psx_mod_write_word(PE_GS + GS_ROWS + (w >> 24) * 4u, base + (w & 0xFFFFFFu));
    }
}
static int pe_gameplay_area(unsigned a) { return a == 10u || a == 12u || a == 13u; }
static int pe_skin_wearable(int i) {
    if (i < 0 || i >= PE_SKINS) return 0;
    if (g_pe_skins[i].kind == 1) return pe_bundle(i);
    return pe_gameplay_area(g_pe_skins[i].area);
}

static void pe_on_entity_texture(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!pe_loaded() || psx_mod_read_byte(PE_GS + GS_ETPHASE) != 0) return;
    uint32_t flags = psx_mod_read_word(PE_GS + GS_FLAGS);
    uint8_t dayflags = psx_mod_read_byte(PE_GS + GS_DAYFLAGS);
    unsigned area = psx_mod_read_byte(PE_AREA_TYPE);
    const int w = s_prof.worn < PE_SKINS ? s_prof.worn : 0;
    const PeSkin *k = &g_pe_skins[w];
    if (w && k->kind == 0 && pe_skin_wearable(w) && pe_gameplay_area(area) && area != k->area) {
        area = k->area;                                      /* an outfit: the game's own bundle */
        psx_mod_write_byte(PE_AREA_TYPE, (uint8_t)area);
        if (area != psx_mod_read_byte(PE_GS + GS_LOADED_BANK)) flags |= 0x200000u;
        dayflags |= 1u;
        pe_log("skin", "outfit=%s area=%u", k->name, area);
    }
    const int active = k->kind == 1 && pe_bundle(w) && pe_gameplay_area(area) && !(flags & 0x20000u);
    const uint32_t scratch = psx_mod_read_word(PE_GS + GS_SCRATCH);
    if (active && pe_ptr(scratch)) {
        if ((flags & 0x200000u) || s_have != w) {            /* our texture through the game's upload */
            pe_put_blob(scratch, &s_bundle[w].tex);
            psx_mod_write_byte(PE_GS + GS_ETPHASE, 3);
        }
        s_pending = w;                                       /* model and rows at the row load */
    } else {
        if (s_have != -1) { flags |= 0x200000u; dayflags |= 1u; }   /* the game's own back */
        if (s_have >= 0) pe_log("skin", "worn=Aya game bundle back");
        s_have = -1; s_pending = -1;
    }
    if (flags & 0x200000u) dayflags |= 1u;                  /* a new bundle needs its own rows */
    psx_mod_write_word(PE_GS + GS_FLAGS, flags);
    psx_mod_write_byte(PE_GS + GS_DAYFLAGS, dayflags);
}

static void pe_on_entity_textures(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const int w = s_pending;
    if (w < 0 || psx_mod_read_byte(PE_GS + GS_SSUB) != 0 || psx_mod_read_byte(PE_GS + GS_ETPHASE) != 0) return;
    s_pending = -1;
    const uint32_t voice = psx_mod_read_word(PE_GS + GS_VOICE), etex = psx_mod_read_word(PE_GS + GS_ETEX);
    if (!pe_ptr(voice) || !pe_ptr(etex)) return;
    pe_put_blob(voice, &s_bundle[w].model);                  /* over the bundle the game just read */
    pe_point_bundle(voice, &s_bundle[w].model);
    int day = (int8_t)psx_mod_read_byte(PE_GS + GS_DAY);
    if (day < 1 || day > 8) day = 1;
    pe_put_blob(etex, &s_bundle[w].day[day]);                /* the game resolves our rows */
    psx_mod_write_byte(PE_GS + GS_SSUB, 6);
    s_have = w;
    pe_log("skin", "worn=%s model+texture+day%d rows in", g_pe_skins[w].name, day);
}

/* The game's same-room reload, so a new costume shows when F1 closes. */
static int pe_request_reload(void) {
    if (!pe_loaded()) return 0;
    if (psx_mod_read_word(PE_GAME_BLOCK) & 2u) return 0;     /* battle: the next room does it */
    if (psx_mod_read_byte(PE_AREA_TYPE) - 10u >= 5u) return 0;
    psx_mod_write_word(PE_GAME_BLOCK, psx_mod_read_word(PE_GAME_BLOCK) | 0x2000u);
    psx_mod_write_word(PE_GS + GS_FLAGS, psx_mod_read_word(PE_GS + GS_FLAGS) | 0x800u);
    pe_log("skin", "reload worn=%s", g_pe_skins[s_prof.worn].name);
    return 1;
}

/* ---- F1 pages ------------------------------------------------------------------------------- *
 * Few rows, left / right to browse: a filter row, the item row (only what the filter shows), the
 * details, one action. (Until 2026-10-20 these were grids of 75 / 27 tiles.) */
#define PE_PAGE_B "Collection/Bestiary"
#define PE_PAGE_W "Collection/Wardrobe"
static int s_bcur, s_dummy;
static char s_line[4][96];

/* Bestiary filters: area (the generator's sections, in order) and how much is known. */
#define PE_MAX_SECTIONS 12
static const char *s_sections[PE_MAX_SECTIONS + 1] = { "All areas" };
static unsigned s_nsections = 1;
static int s_barea, s_bshow, s_bsel;
static const char *const s_show_names[] = { "All", "Met", "Not complete" };
static int s_blist[PE_CREATURES], s_bcount;
static char s_bnames[PE_CREATURES][48];

static void pe_build_sections(void) {
    for (int c = 0; c < PE_CREATURES; ++c) {
        unsigned j = 1;
        while (j < s_nsections && strcmp(s_sections[j], g_pe_creatures[c].section)) ++j;
        if (j == s_nsections && s_nsections <= PE_MAX_SECTIONS) s_sections[s_nsections++] = g_pe_creatures[c].section;
    }
}
static void pe_bestiary_filter(void) {
    s_bcount = 0;
    for (int c = 0; c < PE_CREATURES; ++c) {
        const uint8_t s = s_prof.creature[c];
        if (s_barea > 0 && (unsigned)s_barea < s_nsections && strcmp(g_pe_creatures[c].section, s_sections[s_barea])) continue;
        if (s_bshow == 1 && !(s & C_MET)) continue;
        if (s_bshow == 2 && (s & C_WEAK) && (s & C_LOOT)) continue;
        s_blist[s_bcount++] = c;
    }
    if (s_bsel >= s_bcount) s_bsel = 0;
    s_bcur = s_bcount ? s_blist[s_bsel] : 0;
}
static unsigned pe_b_count(void) { pe_bestiary_filter(); return s_bcount ? (unsigned)s_bcount : 1u; }
static const char *pe_b_name(unsigned i) {
    pe_bestiary_filter();
    if (!s_bcount) return "(none)";
    if (i >= (unsigned)s_bcount) i = 0;
    const int c = s_blist[i];
    const uint8_t s = s_prof.creature[c];
    if (!(s & C_MET)) snprintf(s_bnames[c], sizeof s_bnames[c], "???  (%u/%d)", i + 1, s_bcount);
    else snprintf(s_bnames[c], sizeof s_bnames[c], "%s%s%s  (%u/%d)", g_pe_creatures[c].ex ? "EX " : "", g_pe_creatures[c].name,
                  (s & C_WEAK) && (s & C_LOOT) ? " *" : g_pe_creatures[c].boss ? " (boss)" : "", i + 1, s_bcount);
    return s_bnames[c];
}

static const char *pe_elems(uint8_t bits) {
    static char buf[64]; static const char *n[5] = { "Heat", "Frost", "Cyanide", "Acid", "Tranq" };
    buf[0] = 0;
    for (int i = 0; i < 5; ++i) if (bits & (1u << i)) { if (buf[0]) strcat(buf, ", "); strcat(buf, n[i]); }
    return buf[0] ? buf : "none";
}
static unsigned pe_next_price(int c, uint8_t *bit) {
    const uint8_t s = s_prof.creature[c];
    if (!(s & C_WEAK)) { *bit = C_WEAK; return g_pe_creatures[c].price_weak; }
    if (!(s & C_LOOT)) { *bit = C_LOOT; return g_pe_creatures[c].price_loot; }
    *bit = 0; return 0;
}
static void pe_bestiary_lines(void) {
    pe_bestiary_filter();
    const int c = s_bcur < 0 || s_bcur >= PE_CREATURES ? 0 : s_bcur;
    const PeCreature *k = &g_pe_creatures[c];
    const uint8_t s = s_prof.creature[c];
    if (s & C_MET) snprintf(s_line[0], sizeof s_line[0], "%s%s  HP %u  Def %u  %s  kills %u", k->ex ? "EX " : "",
                            k->name, k->hp, k->defense, k->section, pe_kills(c));
    else snprintf(s_line[0], sizeof s_line[0], "???  (%s)", k->section);
    if (s & C_WEAK) snprintf(s_line[1], sizeof s_line[1], "Weak: %s", pe_elems(k->weak)),
                    snprintf(s_line[1] + strlen(s_line[1]), sizeof s_line[1] - strlen(s_line[1]), "  Strong: %s", pe_elems(k->strong));
    else snprintf(s_line[1], sizeof s_line[1], "Weaknesses: Scan, %s or %u Junk", k->boss ? "defeat" : "5 kills", k->price_weak);
    if (s & C_LOOT) {
        snprintf(s_line[2], sizeof s_line[2], "Drop: %s", k->drop);
        snprintf(s_line[3], sizeof s_line[3], "Steal: %s", k->steal);
    } else {
        snprintf(s_line[2], sizeof s_line[2], "Loot: %s%u Junk", k->boss ? "" : "15 kills or ", k->price_loot);
        s_line[3][0] = 0;
    }
}
static unsigned pe_one(void) { return 1; }
static const char *pe_line0(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[0]; }
static const char *pe_line1(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[1]; }
static const char *pe_line2(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[2]; }
static const char *pe_line3(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[3][0] ? s_line[3] : "-"; }
static const char *pe_bprogress(unsigned i) {
    static char buf[64]; (void)i;
    int met = 0, done = 0;
    for (int c = 0; c < PE_CREATURES; ++c) {
        met += (s_prof.creature[c] & C_MET) != 0;
        done += (s_prof.creature[c] & (C_WEAK | C_LOOT)) == (C_WEAK | C_LOOT);
    }
    snprintf(buf, sizeof buf, "Met %d/%d  Complete %d  Junk %u", met, PE_CREATURES, done, (unsigned)pe_loot_junk());
    return buf;
}

static void pe_buy_tier(void) {
    if (!pe_loaded()) { psx_mod_osd_message("Load a game first (Junk comes from it)"); return; }
    pe_bestiary_filter();
    if (!s_bcount) return;
    const int c = s_bcur;
    uint8_t bit; const unsigned price = pe_next_price(c, &bit);
    char msg[96];
    if (!bit) { psx_mod_osd_message("Already complete"); return; }
    if (!pe_loot_spend_junk(price)) {
        snprintf(msg, sizeof msg, "Need %u Junk (have %u)", price, (unsigned)pe_loot_junk());
        psx_mod_osd_message(msg); return;
    }
    pe_unlock(c, bit, "junk");
    pe_profile_save();
}

/* Wardrobe groups: Aya's outfits, then characters by price tier. */
static const char *const s_group_names[] = { "Aya's outfits", "Main cast", "Supporting", "Extras" };
static int s_wgroup, s_wsel;
static int s_wlist[PE_SKINS], s_wcount;
static char s_wnames[PE_SKINS][56];
static int pe_skin_group(int i) {
    const PeSkin *k = &g_pe_skins[i];
    if (k->kind == 0) return 0;
    return k->price >= 250 ? 1 : k->price >= 150 ? 2 : 3;
}
static void pe_wardrobe_filter(void) {
    s_wcount = 0;
    for (int i = 0; i < PE_SKINS; ++i) if (pe_skin_group(i) == s_wgroup) s_wlist[s_wcount++] = i;
    if (s_wsel >= s_wcount) s_wsel = 0;
}
static unsigned pe_w_count(void) { pe_wardrobe_filter(); return s_wcount ? (unsigned)s_wcount : 1u; }
static const char *pe_w_name(unsigned n) {
    pe_wardrobe_filter();
    if (!s_wcount) return "(none)";
    if (n >= (unsigned)s_wcount) n = 0;
    const int i = s_wlist[n];
    const PeSkin *k = &g_pe_skins[i];
    const char *state = !pe_skin_wearable(i) ? "soon" : s_prof.worn == i ? "worn" : ((s_prof.skins >> i) & 1u) ? "owned" : NULL;
    if (state) snprintf(s_wnames[i], sizeof s_wnames[i], "%s  [%s]  (%u/%d)", k->name, state, n + 1, s_wcount);
    else snprintf(s_wnames[i], sizeof s_wnames[i], "%s  %u Junk  (%u/%d)", k->name, k->price, n + 1, s_wcount);
    return s_wnames[i];
}
static const char *pe_w_status(unsigned n) {
    static char buf[80]; (void)n;
    snprintf(buf, sizeof buf, "Wearing %s   Junk %u", g_pe_skins[s_prof.worn < PE_SKINS ? s_prof.worn : 0].name,
             (unsigned)pe_loot_junk());
    return buf;
}
static void pe_wear(int i) {
    char msg[96];
    s_prof.worn = (uint8_t)i;
    pe_profile_save();
    snprintf(msg, sizeof msg, pe_request_reload() ? "Wearing %s" : "Wearing %s from the next room", g_pe_skins[i].name);
    psx_mod_osd_message(msg);
}
static void pe_w_accept(void) {
    pe_wardrobe_filter();
    if (!s_wcount) return;
    const int i = s_wlist[s_wsel];
    const PeSkin *k = &g_pe_skins[i];
    char msg[96];
    if (!pe_skin_wearable(i)) { psx_mod_osd_message("Not available yet (build it with build_skin.py)"); return; }
    if (s_prof.worn == i) { psx_mod_osd_message("Already wearing it"); return; }
    if (!((s_prof.skins >> i) & 1u)) {
        if (!pe_loaded()) { psx_mod_osd_message("Load a game first (Junk comes from it)"); return; }
        if (!pe_loot_spend_junk(k->price)) {
            snprintf(msg, sizeof msg, "Need %u Junk (have %u)", (unsigned)k->price, (unsigned)pe_loot_junk());
            psx_mod_osd_message(msg); return;
        }
        s_prof.skins |= 1ull << i;
        pe_log("wardrobe", "bought=%s price=%u", k->name, (unsigned)k->price);
    }
    pe_log("wardrobe", "wear=%s", k->name);
    pe_wear(i);
}
static void pe_w_default(void) {
    if (s_prof.worn == 0) { psx_mod_osd_message("Already wearing Aya"); return; }
    pe_log("wardrobe", "wear=Aya");
    pe_wear(0);
}

/* For Wayne's Workshop (pe_wayne_menu.c): the skins that can be worn or bought now, by list index. */
static int pe_ward_skin(int k) {
    for (int i = 0; i < PE_SKINS; ++i)
        if ((i == 0 || pe_skin_wearable(i)) && k-- == 0) return i;
    return -1;
}
int pe_ward_count(void) { int n = 0; while (pe_ward_skin(n) >= 0) ++n; return n; }
const char *pe_ward_name(int k) { const int i = pe_ward_skin(k); return i >= 0 ? g_pe_skins[i].name : ""; }
unsigned pe_ward_price(int k) { const int i = pe_ward_skin(k); return i >= 0 ? g_pe_skins[i].price : 0u; }
int pe_ward_state(int k) {                     /* 0 locked, 1 owned, 2 worn */
    const int i = pe_ward_skin(k);
    if (i < 0) return 0;
    if (s_prof.worn == i) return 2;
    return (int)((s_prof.skins >> i) & 1u);
}
int pe_ward_pick(int k, char *msg, size_t n) {   /* buy if needed, then wear (shows when the room changes) */
    const int i = pe_ward_skin(k);
    if (i < 0) return 0;
    if (!((s_prof.skins >> i) & 1u)) {
        if (!pe_loot_spend_junk(g_pe_skins[i].price)) {
            snprintf(msg, n, "Need %u Junk", (unsigned)g_pe_skins[i].price); return 0;
        }
        s_prof.skins |= 1ull << i;
        pe_log("wardrobe", "bought=%s price=%u at=wayne", g_pe_skins[i].name, (unsigned)g_pe_skins[i].price);
    }
    s_prof.worn = (uint8_t)i;
    pe_profile_save();
    snprintf(msg, n, "Wearing %s when you leave", g_pe_skins[i].name);
    return 1;
}

static void pe_reset_collection(void) {
    pe_profile_defaults(); pe_profile_save();
    psx_mod_osd_message("Collection reset");
}

/* ---- tick / register ------------------------------------------------------------------------ */
static void pe_collection_tick(void) {
    if (!pe_loaded()) { memset(s_slot, 0, sizeof s_slot); s_have = -2; s_pending = -1; return; }
    if (psx_mod_read_word(PE_GAME_BLOCK) & 2u) pe_watch_slots();
    else memset(s_slot, 0, sizeof s_slot);
    if (s_dirty) pe_profile_save();
}
static void pe_collection_restored(void) { s_have = -2; s_pending = -1; }   /* a save state brings its own buffers */

void pe_collection_register(void) {
    pe_profile_load();
    pe_build_sections();
    (void)pe_menu_register_choice(PE_PAGE_B, "Area", "Which creatures to browse.", &s_barea, s_sections, s_nsections);
    (void)pe_menu_register_choice(PE_PAGE_B, "Show", "All, met only, or not complete.", &s_bshow, s_show_names, 3);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Creature", "Left / right to browse. * = complete.", &s_bsel,
                                          PE_CREATURES, pe_b_count, pe_b_name);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Info", "Name, HP, defense, where.", &s_dummy, 1, pe_one, pe_line0);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Elements", "Weak and strong elements.", &s_dummy, 1, pe_one, pe_line1);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Drop", "What it drops (odds).", &s_dummy, 1, pe_one, pe_line2);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Steal", "What can be stolen (odds).", &s_dummy, 1, pe_one, pe_line3);
    (void)pe_menu_register_action(PE_PAGE_B, "Buy next tier", "Spend Junk: weaknesses, then loot.", pe_buy_tier);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Progress", "Bestiary totals.", &s_dummy, 1, pe_one, pe_bprogress);
    (void)pe_menu_register_choice(PE_PAGE_W, "Group", "Aya's outfits or characters.", &s_wgroup, s_group_names, 4);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_W, "Costume", "Left / right to browse.", &s_wsel, PE_SKINS, pe_w_count, pe_w_name);
    (void)pe_menu_register_action(PE_PAGE_W, "Buy / wear", "Buy once (every game), wear now: the room reloads.", pe_w_accept);
    (void)pe_menu_register_action(PE_PAGE_W, "Wear Aya", "Back to Aya's own clothes.", pe_w_default);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_W, "Status", "What you wear, Junk.", &s_dummy, 1, pe_one, pe_w_status);
    (void)pe_menu_register_action("Debug", "Reset collection", "Forget all Bestiary / skin unlocks.", pe_reset_collection);
    (void)pe_menu_register_tick(pe_collection_tick);
    (void)psx_mod_register_restore_callback(pe_collection_restored);
    (void)psx_mod_register_function_entry_plugin("pe.collection.scan", PE_FN_APPLY_SPELL, pe_on_spell);
    (void)psx_mod_register_function_entry_plugin("pe.collection.etex", PE_FN_ENTITY_TEX, pe_on_entity_texture);
    (void)psx_mod_register_function_entry_plugin("pe.collection.etexs", PE_FN_ENTITY_TEXS, pe_on_entity_textures);
}
