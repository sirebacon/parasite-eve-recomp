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

/* ---- Wardrobe: Aya's outfits ---------------------------------------------------------------- */
static int pe_skin_wearable(int i) { return i >= 0 && i < PE_SKINS && g_pe_skins[i].kind == 0 &&
                                     (g_pe_skins[i].area == 10 || g_pe_skins[i].area == 12 || g_pe_skins[i].area == 13); }
static void pe_outfit_tick(void) {
    if (!s_prof.worn || !pe_skin_wearable(s_prof.worn)) return;   /* Aya as the story dresses her */
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
    if (k->kind != 0 || (!pe_skin_wearable(i) && i != 0)) { snprintf(label, size, "%s (soon)", k->name); *flags = PE_CELL_EMPTY; return 1; }
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
    if (k->kind != 0 || (!pe_skin_wearable(i) && i != 0)) { psx_mod_osd_message("Not available yet"); return; }
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
    if (!pe_loaded()) { memset(s_slot, 0, sizeof s_slot); return; }
    pe_outfit_tick();
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
