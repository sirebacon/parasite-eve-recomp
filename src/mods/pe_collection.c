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
 * Textures) all follow it. Wearing an outfit rewrites it, every frame, from 10 / 12 / 13 (the
 * gameplay outfits) to the worn one before the scene start-up reads it (it runs over several frames
 * after the room data arrives). 11 (Day 1 long dress) and 14 (transformed form) have no full player
 * moveset and are not offered; resume's 0x0E is left alone. Character skins are listed but not
 * sold until their bundles are built.
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

/* ---- Wardrobe: character bundles ------------------------------------------------------------- *
 * pe_skins/<folder>/ holds model.bin (the player bundle: model + bundle rows), texture.bin (the
 * player texture blob) and day1..day8.bin (per-day rows 4-12), built from the user's disc by the
 * bridge's tools/pe_assets/build_skin.py. The game's scene start-up runs one step a frame
 * (g_GameState +0xED: 0x24 Scene_LoadEntityTexture, 0x25, 0x26 Scene_LoadEntityTextures, 0x27 the
 * player's render object), so between its steps:
 *   - before 0x24, a bundle reload is forced (flags +0x00 |= 0x200000) and the day rows too
 *     (story_day_flags +0x0E |= 1) while the buffers don't hold this skin yet;
 *   - at 0x25 after a texture load (entity phase +0xEC went through 1-5), our texture blob goes into
 *     the scratch buffer (+0x194) and the loader is sent back to its upload step (phase 3, scene
 *     step 0x24): the game uploads our TIMs itself, then re-reads its bundle from disc;
 *   - at 0x25 again, our model.bin goes over that bundle (+0x154, 0xE000 bytes) and bank_slots[0] /
 *     bank_rows[0][id] (+0x198 / +0x1C0) point into it;
 *   - at 0x27 after a day-row load (subphase +0xEE 4-6), our day blob goes over the game's (+0x158)
 *     and rows 4-12 point into it.
 * A room that reloads nothing keeps our data (the game re-resolves its pointers from the same
 * buffers, in the same blob format). Only gameplay outfit rooms (area 10 / 12 / 13) are touched. */
#define PE_GS          0x800B0CD8u
#define GS_FLAGS       0x000u
#define GS_DAY         0x00Cu
#define GS_DAYFLAGS    0x00Eu
#define GS_ETPHASE     0x0ECu
#define GS_SINIT       0x0EDu
#define GS_SSUB        0x0EEu
#define GS_VOICE       0x154u
#define GS_ETEX        0x158u
#define GS_SCRATCH     0x194u
#define GS_SLOTS       0x198u
#define GS_ROWS        0x1C0u
#define PE_MODEL_BUF_MAX 0xE000u
typedef struct { uint8_t *data; uint32_t size; } PeBlob;
static struct { int loaded, ok; PeBlob model, tex, day[9]; } s_bundle[PE_SKINS];
static int s_have = -1, s_have_day = -1, s_tex_loading, s_day_loading, s_stage;

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
/* Point bank_slots[0] / bank_rows[0][id] at a blob written to `base` (SceneAssetBlob: directory at
 * word 1; +0x0C model records, +0x10 row records; records 12 bytes, word 1 = offset | id << 24). */
static void pe_point_blob(uint32_t base, const PeBlob *b, int model) {
    const uint32_t dir = rd32(b->data, 4);
    if (model) {
        const uint32_t desc = rd32(b->data, dir + 0x0C), at = desc & 0x3FFFFFu;
        if (desc >> 22) psx_mod_write_word(PE_GS + GS_SLOTS, base + (rd32(b->data, at + 4) & 0xFFFFFFu));
    }
    const uint32_t desc = rd32(b->data, dir + 0x10), at = desc & 0x3FFFFFu;
    for (uint32_t i = 0; i < (desc >> 22); ++i) {
        const uint32_t w = rd32(b->data, at + 12u * i + 4u);
        if ((w >> 24) < 48u) psx_mod_write_word(PE_GS + GS_ROWS + (w >> 24) * 4u, base + (w & 0xFFFFFFu));
    }
}
static void pe_skin_tick(void) {
    const int w = s_prof.worn;
    const uint8_t sinit = psx_mod_read_byte(PE_GS + GS_SINIT), et = psx_mod_read_byte(PE_GS + GS_ETPHASE);
    const uint8_t sub = psx_mod_read_byte(PE_GS + GS_SSUB), area = psx_mod_read_byte(PE_AREA_TYPE);
    if (et >= 1 && et <= 5) s_tex_loading = 1;
    if (sinit == 0x26 && sub >= 4 && sub <= 6) s_day_loading = 1;
    const int active = pe_bundle(w) && (area == 10 || area == 12 || area == 13);
    if (!active) {                                  /* a game bundle went in: ours is gone from the buffers */
        if (sinit == 0x25 && s_tex_loading) { s_have = -1; s_tex_loading = 0; }
        if (sinit == 0x27 && s_day_loading) { s_have_day = -1; s_day_loading = 0; }
        s_stage = 0;
        return;
    }
    if (sinit >= 0x20 && sinit <= 0x23) {           /* before the bundle step: make sure it reloads */
        if (s_have != w) psx_mod_write_word(PE_GS + GS_FLAGS, psx_mod_read_word(PE_GS + GS_FLAGS) | 0x200000u);
        if (s_have_day != w) psx_mod_write_byte(PE_GS + GS_DAYFLAGS, psx_mod_read_byte(PE_GS + GS_DAYFLAGS) | 1u);
    }
    if (sinit == 0x25 && s_tex_loading) {
        s_tex_loading = 0;
        if (s_stage == 0) {                         /* 1: our texture through the game's own upload */
            const uint32_t scratch = psx_mod_read_word(PE_GS + GS_SCRATCH);
            if (!pe_ptr(scratch)) return;
            pe_put_blob(scratch, &s_bundle[w].tex);
            psx_mod_write_byte(PE_GS + GS_ETPHASE, 3);
            psx_mod_write_byte(PE_GS + GS_SINIT, 0x24);
            s_stage = 1;
        } else {                                    /* 2: our model and rows over the reloaded bundle */
            const uint32_t voice = psx_mod_read_word(PE_GS + GS_VOICE);
            if (!pe_ptr(voice)) { s_stage = 0; return; }
            pe_put_blob(voice, &s_bundle[w].model);
            pe_point_blob(voice, &s_bundle[w].model, 1);
            s_have = w; s_stage = 0;
            pe_log("skin", "worn=%s model+texture in", g_pe_skins[w].name);
        }
    }
    if (sinit == 0x27 && s_day_loading) {            /* 3: the per-day rows */
        s_day_loading = 0;
        int day = (int8_t)psx_mod_read_byte(PE_GS + GS_DAY);
        if (day < 1 || day > 8) day = 1;
        const uint32_t etex = psx_mod_read_word(PE_GS + GS_ETEX);
        if (!pe_ptr(etex)) return;
        pe_put_blob(etex, &s_bundle[w].day[day]);
        pe_point_blob(etex, &s_bundle[w].day[day], 0);
        s_have_day = w;
        pe_log("skin", "worn=%s day%d rows in", g_pe_skins[w].name, day);
    }
}

/* ---- Wardrobe: Aya's outfits ---------------------------------------------------------------- */
static int pe_skin_wearable(int i) {
    if (i < 0 || i >= PE_SKINS) return 0;
    if (g_pe_skins[i].kind == 1) return pe_bundle(i);
    return g_pe_skins[i].area == 10 || g_pe_skins[i].area == 12 || g_pe_skins[i].area == 13;
}
static void pe_outfit_tick(void) {
    if (!s_prof.worn || !pe_skin_wearable(s_prof.worn) || g_pe_skins[s_prof.worn].kind != 0) return;
    const uint8_t area = psx_mod_read_byte(PE_AREA_TYPE), want = g_pe_skins[s_prof.worn].area;
    if ((area == 10 || area == 12 || area == 13) && area != want) psx_mod_write_byte(PE_AREA_TYPE, want);
}

/* ---- F1 pages ------------------------------------------------------------------------------- */
#define PE_PAGE_B "Collection/Bestiary"
#define PE_PAGE_W "Collection/Wardrobe"
static int s_bcur, s_wcur, s_dummy;
static char s_line[4][96];

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
static unsigned pe_b_count(void) { return PE_CREATURES; }
static int pe_b_cell(int i, char *label, unsigned size, unsigned *flags) {
    if (i == -1) {
        int met = 0; for (int c = 0; c < PE_CREATURES; ++c) met += (s_prof.creature[c] & C_MET) != 0;
        snprintf(label, size, "%d/%d", met, PE_CREATURES); return 1;
    }
    if (i < 0 || i >= PE_CREATURES) return 0;
    const uint8_t s = s_prof.creature[i];
    if (s & C_MET) snprintf(label, size, "%s%s", g_pe_creatures[i].ex ? "EX " : "", g_pe_creatures[i].name);
    else snprintf(label, size, "???");
    *flags = !(s & C_MET) ? PE_CELL_EMPTY : (s & C_WEAK) && (s & C_LOOT) ? PE_CELL_HIGHLIGHT : 0;
    if (g_pe_creatures[i].boss) *flags |= PE_CELL_MARK;
    return 1;
}
static unsigned pe_one(void) { return 1; }
static const char *pe_line0(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[0]; }
static const char *pe_line1(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[1]; }
static const char *pe_line2(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[2]; }
static const char *pe_line3(unsigned i) { (void)i; pe_bestiary_lines(); return s_line[3][0] ? s_line[3] : "-"; }

static void pe_buy_tier(void) {
    if (!pe_loaded()) { psx_mod_osd_message("Load a game first (Junk comes from it)"); return; }
    const int c = s_bcur < 0 || s_bcur >= PE_CREATURES ? 0 : s_bcur;
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

static unsigned pe_w_count(void) { return PE_SKINS; }
static int pe_w_cell(int i, char *label, unsigned size, unsigned *flags) {
    if (i == -1) { snprintf(label, size, "Junk %u", (unsigned)pe_loot_junk()); return 1; }
    if (i < 0 || i >= PE_SKINS) return 0;
    const PeSkin *k = &g_pe_skins[i];
    const int owned = (s_prof.skins >> i) & 1u;
    if (!pe_skin_wearable(i) && i != 0) { snprintf(label, size, "%s (soon)", k->name); *flags = PE_CELL_EMPTY; return 1; }
    if (owned) snprintf(label, size, "%s%s", k->name, s_prof.worn == i ? " [worn]" : "");
    else snprintf(label, size, "%s %u", k->name, k->price);
    *flags = s_prof.worn == i ? PE_CELL_HIGHLIGHT : owned ? 0 : PE_CELL_DIM;
    return 1;
}
static void pe_w_accept(void) {
    const int i = s_wcur;
    if (i < 0 || i >= PE_SKINS) return;
    const PeSkin *k = &g_pe_skins[i];
    char msg[96];
    if (!pe_skin_wearable(i) && i != 0) { psx_mod_osd_message("Not available yet (build it with build_skin.py)"); return; }
    if (!((s_prof.skins >> i) & 1u)) {
        if (!pe_loaded()) { psx_mod_osd_message("Load a game first (Junk comes from it)"); return; }
        if (!pe_loot_spend_junk(k->price)) {
            snprintf(msg, sizeof msg, "Need %u Junk (have %u)", (unsigned)k->price, (unsigned)pe_loot_junk());
            psx_mod_osd_message(msg); return;
        }
        s_prof.skins |= 1ull << i;
        snprintf(msg, sizeof msg, "Unlocked: %s (every game)", k->name);
        psx_mod_osd_message(msg);
        pe_log("wardrobe", "bought=%s price=%u", k->name, (unsigned)k->price);
    }
    s_prof.worn = (uint8_t)i;
    snprintf(msg, sizeof msg, "Wearing %s from the next room", k->name);
    psx_mod_osd_message(msg);
    pe_profile_save();
}

static void pe_reset_collection(void) {
    pe_profile_defaults(); pe_profile_save();
    psx_mod_osd_message("Collection reset");
}

/* ---- tick / register ------------------------------------------------------------------------ */
static void pe_collection_tick(void) {
    if (!pe_loaded()) { memset(s_slot, 0, sizeof s_slot); s_have = s_have_day = -1; s_stage = 0; return; }
    pe_outfit_tick();
    pe_skin_tick();
    if (psx_mod_read_word(PE_GAME_BLOCK) & 2u) pe_watch_slots();
    else memset(s_slot, 0, sizeof s_slot);
    if (s_dirty) pe_profile_save();
}

void pe_collection_register(void) {
    pe_profile_load();
    (void)pe_menu_register_grid(PE_PAGE_B, "Creatures", "Pick one; details below.", &s_bcur,
                                PE_CREATURES, 3, pe_b_count, pe_b_cell, 0, 0);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Info", "Name, HP, defense, where.", &s_dummy, 1, pe_one, pe_line0);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Elements", "Weak and strong elements.", &s_dummy, 1, pe_one, pe_line1);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Drop", "What it drops (odds).", &s_dummy, 1, pe_one, pe_line2);
    (void)pe_menu_register_dynamic_choice(PE_PAGE_B, "Steal", "What can be stolen (odds).", &s_dummy, 1, pe_one, pe_line3);
    (void)pe_menu_register_action(PE_PAGE_B, "Buy next tier", "Spend Junk: weaknesses, then loot.", pe_buy_tier);
    (void)pe_menu_register_grid(PE_PAGE_W, "Skins", "Buy once, wear in every game.", &s_wcur,
                                PE_SKINS, 2, pe_w_count, pe_w_cell, pe_w_accept, 0);
    (void)pe_menu_register_action("Debug", "Reset collection", "Forget all Bestiary / skin unlocks.", pe_reset_collection);
    (void)pe_menu_register_tick(pe_collection_tick);
    (void)psx_mod_register_function_entry_plugin("pe.collection.scan", PE_FN_APPLY_SPELL, pe_on_spell);
}
