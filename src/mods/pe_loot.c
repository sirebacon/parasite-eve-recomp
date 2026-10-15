/*
 * Loot overhaul, phase 1: the Junk counter (bridge findings
 * pe-loot-overhaul-design-2026-10-11.md, decisions 2, 13, 16, 17, 20, 21, 22,
 * 24, 28, 30, 32, 35, 41; checklist pe-loot-overhaul-checklist.md).
 *
 * Junk never takes a bag slot: it lives in one counter.
 *   - Battle end (hook on Aya_SetTotalExp, once per won battle, a0 = the
 *     battle's EXP, a2 = the reward list): vanilla Junk entries leave the
 *     list for the counter; the battle pays floor + EXP / divisor (capped),
 *     plus a no-hit bonus (hits each enemy landed on Aya, +0x9C), plus Junk
 *     stolen during the battle. Fleeing never reaches this hook, so stolen
 *     Junk is forfeited (decision 41). The battle's Junk is shown on the game's
 *     own items-found screen as one entry, Junk (0x14), whose name row reads
 *     "Junk xN" (Draw_PrintRawText while Sfx_DrawSlotRow draws the list, as the
 *     rare drops' gold names; decision 21). As with vanilla drops it's the
 *     player's choice (user, 2026-10-13): taking it moves a real Junk into the
 *     bag, which goes straight back out while all N go into the counter; left
 *     behind, it's lost, and a full bag can't take it. With no room in the list
 *     for the entry, the Junk is credited directly. No toast (in-game messages
 *     only).
 *   - Inv_AddItem(0x14) (chests, story gifts, steals): the id is rewritten to
 *     0 before the body runs, which adds nothing and reports success
 *     (Item_LookupBaseData(-1) is out of range; the game's own pickup message
 *     still names the Junk), and the Junk is counted --
 *     or banked until the battle is won when it comes from a steal
 *     (Battle_ResolveHitOnTimer). So Junk is taken even with a full bag.
 *   - Wayne's "Discard junk" counts Junk ONCE with the script opcode
 *     Task_CountItemsByValue, then removes that many with Inv_DispatchCommand
 *     1112, adding each to his counter (0x800A78B0); he crafts at 300. The
 *     count is pointed at a scratch list of N Junk (N = what his next gun
 *     needs in "Next gun" mode, or all), and each removal gets one Junk from
 *     the counter placed just in time, so only one free bag slot is needed.
 *   - A sweep (free field only, not right after a hand-in count) moves any
 *     Junk found in the bag or Wayne's storage into the counter: an existing
 *     save's Junk on first load, Junk the Item Editor gives, a leftover
 *     placed Junk.
 *   - Phase 2, rare drops (decisions 18, 23): each won battle rolls a rare
 *     drop with soft pity (base %, +step % per dry battle, certain at the
 *     guarantee); a hit walks the cascade (2026-10-14, bridge findings
 *     pe-loot-cascade-design-2026-10-14.md: economy mod 15% tiered 1/2/4/3 with a
 *     guarantee after 8 rares, Super Tool 6%, Mod Permit 15% skipped at 2 held with a
 *     guarantee, Lucky Charm 8%, Duper 4%, Super 8%, Tool 85/65/40/20/10% by Tools
 *     held, else a stat booster by battle EXP tier) and is appended to the reward list, so it shows on the game's own
 *     items-found screen, its name drawn in gold (Draw_PrintRawText hook, rows
 *     drawn by Sfx_DrawSlotRow while the items-found list is active); a full
 *     list carries it to the next battle.
 *   - Steal messages: when Aya steals an item, its name in the battle message
 *     ("<item> obtained", built in D_800A1B50 by Inv_GetItemEffectData) is
 *     wrapped in the textbox's own red highlight codes (0xFB 0x04 ... 0xFB
 *     0x05); the textbox has no other color. User request, 2026-10-12.
 *   - Phase 7 (balance estimate 2026-10-13, section 9): battle Junk is capped
 *     at 12 a fight, and boss fights (any enemy flagged "can't escape",
 *     statusFlags2 & 0x40000, seen during the fight) use a cap of 25. With the
 *     Tool Kit a rare Tool roll becomes a stat booster (Super Tool rolls too
 *     with the Super Tool Kit).
 *   - Phase 6 (custom items doc C1-C7, decisions 19, 39): difficulty presets
 *     Relaxed / Normal / Grind scale battle Junk (x1.5 / x1 / x0.75), the
 *     rare chance's base and step (x1.5 / x1 / x0.5 and x1.5 / x1 / x0.6:
 *     one rare per ~7.6 / 9.8 / 13.8 battles) and guarantee (18 / 25 / 33),
 *     and every price (-25% / 0 / +25%, caps still apply); the economy armor mods Scavenger (+50% battle Junk)
 *     and Lucky Find (pity builds twice as fast) are read from the equipped
 *     armor; a carried Lucky Charm is used up at the next won battle and
 *     doubles pity progress for 10 won battles; the rare table also unlocks
 *     the four economy mods straight into the Mod Catalog and drops Lucky
 *     Charms; an empty steal banks a little Junk, paid on a win (19, 35).
 *     An economy mod has no item of its own: it shows on the items-found
 *     screen as a Mod Permit entry (0x37) whose name row reads "<mod> mod" in
 *     gold; taking it unlocks the mod (the permit leaves the bag at once),
 *     leaving it loses it (it can roll again). It never shares a list with a
 *     real Mod Permit (it waits for the next battle instead).
 *   - Saved in the save's free tail (0x1C90 of the 8 KB block, covered by the
 *     game's CRC) and in save states (psx_mod_register_state "pe.loot").
 *     A new game resets it (Inv_DispatchCommand 1117).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"

#define PE_FN_SET_TOTAL_EXP     0x8004B70Cu  /* game.toml mod_function_entry_funcs */
#define PE_FN_INV_DISPATCH      0x8005D2B4u
#define PE_FN_INV_ADD_ITEM      0x80053D2Cu
#define PE_FN_SAVE_SERIALIZE    0x8003F800u
#define PE_FN_SAVE_DESERIALIZE  0x8003FBD8u
#define PE_FN_TASK_COUNT_ITEMS  0x800194F8u
#define PE_FN_RESOLVE_HIT_START 0x800236E8u  /* Battle_ResolveHitOnTimer (steals) */
#define PE_FN_RESOLVE_HIT_END   0x80023E14u
#define PE_FN_DRAW_PRINT_RAW_TEXT 0x8005F27Cu
#define PE_FN_SLOT_ROW_START    0x800534E4u  /* Sfx_DrawSlotRow: item name rows */
#define PE_FN_SLOT_ROW_END      0x800536B8u

#define PE_INV_CMD_REMOVE_ITEM    1112
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_ITEM_JUNK          0x14u
#define PE_ITEM_LUCKY_CHARM   0x19u        /* ITEM25, relabelled (pe_custom.c) */
#define PE_ARMOR_SLOT_ADDR    0x800C0E22u  /* s8 bag slot of the equipped armor */
#define PE_EQUIP_RECORDS_ADDR 0x800C0EACu
#define PE_CHARM_BATTLES      10u
#define PE_WAYNE_COUNTER      0x800A78B0u  /* u16 script variable */
#define PE_WAYNE_CRAFT_COST   300u
#define PE_AYA_SLOTS_ADDR     0x800C0E48u
#define PE_AYA_SLOT_COUNT     50u
#define PE_STORAGE_ADDR       0x800C1EB8u  /* Wayne's storage */
#define PE_STORAGE_COUNT      100u
#define PE_ACTIVE_LIST_PTR    0x8009D048u  /* g_InvItemPtr */
#define PE_ACTIVE_LIST_LIMIT  0x8009D050u  /* g_InvSlotLimit */
#define PE_MAX_HP_ADDR        0x800C0E06u  /* 0 until a game is loaded */
#define PE_GAME_BLOCK_ADDR    0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE  0x2u
#define PE_FIELD_BUSY_ADDR    0x800B0CD8u  /* free field: !(this & 0xB600) && ... */
#define PE_ENEMY_SLOT_BASE    0x800A5D58u  /* 7 x { u8 active; pad; EnemyCombatant } */
#define PE_ENEMY_SLOT_STRIDE  0xDCu
#define PE_ENEMY_SLOTS        7
#define EN_HITS_ON_AYA        (4u + 0x9Cu)
#define EN_STATUS_FLAGS2      (4u + 0xCCu)    /* & 0x40000: can't escape (bosses) */

#define PE_REWARD_SLOTS       10u
#define PE_INV_OVERRIDE_LIST  0x8009D04Cu  /* g_InvActiveListOverride (items found) */
#define PE_FOUND_ITEMS        0x800A1FD4u  /* g_InvActiveListItems: the items-found list */
#define PE_FOUND_COUNT        0x8009D078u  /* g_InvActiveListCount */
#define PE_DRAW_COLOR         0x8009D110u  /* g_DrawPrimColor (Draw_SetColor) */
#define PE_DRAW_COLOR_SHADED  0x8009D114u  /* g_DrawColorShaded = (color >> 1) & 0x7F7F7F */
#define PE_RARE_COLOR         0x0030A8D0u  /* gold (0xBBGGRR, 0x80 = normal) */
#define PE_STR_ORIGIN         0x800A8028u
#define PE_STR_DIRECTORY_OFFSET 0x800A802Cu
#define PE_STEAL_MSG_PTR      0x8009D1F8u  /* message the battle textbox shows */
#define PE_ITEM_MSG_BUF       0x800A1B50u  /* Inv_GetItemEffectData's buffer */
#define PE_ITEM_MSG_SIZE      0x40u        /* up to the next data at 0x800A1B90 */

/* Save tail: the 8 KB block's CRC sits at 0x1C8C; 0x1C90..0x1FFF is free
 * (zero in every save checked) and still inside the CRC'd range. */
#define PE_CARD_BUFFER        0x8009EED0u  /* g_MemCardFileBuffer */
#define PE_TAIL_OFFSET        0x1C90u
#define PE_TAIL_MAGIC         0x544C4550u  /* "PELT" */
#define PE_TAIL_VERSION       4u

#define PE_HANDIN_GUARD_FRAMES 120u        /* no sweep this soon after a count */
#define PE_HANDIN_MAX          999u        /* Junk per hand-in */

/* ---- state ---------------------------------------------------------------- */
#define PE_LOOT_STATE_VERSION 4u
static struct {
    uint32_t junk;
    uint16_t steal_bank;          /* Junk stolen this battle, paid on a win */
    uint16_t hits[PE_ENEMY_SLOTS];
    uint16_t rare_dry;            /* pity progress since the last rare drop */
    uint16_t rare_carry;          /* a rare drop that didn't fit the list */
    uint32_t rng;                 /* xorshift state (rides in save states) */
    /* v3 */
    uint16_t charm_left;          /* won battles of Lucky Charm left */
    uint8_t preset;               /* 0 Relaxed, 1 Normal, 2 Grind (39) */
    uint8_t disabled;             /* "Junk economy" switched off for this game */
    /* v4: the cascade's guarantees (rares since the last of each) */
    uint8_t econ_since, permit_since;
    uint16_t pad4;
} s_st = { .preset = 1 };
#define PE_LOOT_STATE_V2_SIZE 28u
#define PE_LOOT_STATE_V3_SIZE 32u

static int s_enabled = 1;          /* "Junk economy", on by default (28) */
static int s_handin_mode;          /* 0 = up to Wayne's next gun, 1 = all */
static int s_tune_floor = 1;       /* decision 16 */
static int s_tune_exp_per_junk = 8;/* decision 17 (placeholder until phase 7) */
static int s_tune_battle_cap = 12;   /* phase 7: was 30 */
static int s_tune_boss_cap = 25;     /* phase 7: boss fights */
static int s_tune_nohit_bonus = 25;/* decision 32, percent */
static int s_tune_rare_base = 4;   /* decision 18: soft pity, percent */
static int s_tune_rare_step = 1;   /* + percent per dry battle */
static int s_tune_rare_guarantee = 25; /* certain by this many battles */
static int s_tune_econ_guarantee = 8;  /* cascade: rares until an economy mod is certain */
static int s_tune_permit_guarantee = 8;/* ... a Mod Permit */
static int s_tune_empty_steal = 2; /* decision 19: Junk for an empty steal */

/* Presets (39): battle Junk %, rare guarantee %, price %. */
static const char *const s_preset_names[] = { "Relaxed", "Normal", "Grind" };
static const uint16_t s_preset_junk[3]  = { 150, 100, 75 };
static const uint16_t s_preset_pity[3]  = { 72, 100, 132 };   /* guarantee: 18 / 25 / 33 */
static const uint16_t s_preset_pity_base[3] = { 150, 100, 50 };  /* base chance % */
static const uint16_t s_preset_pity_step[3] = { 150, 100, 60 };  /* step % */
static const uint16_t s_preset_price[3] = { 75, 100, 125 };

static int s_in_battle;

/* The items-found screen this battle: what our entries are. Kept in save
 * states ("pe.loot.found"), so a state loaded on that screen still draws the
 * rare in gold and pays the "Junk xN" line. */
#define PE_FOUND_STATE_VERSION 1u
static struct {
    uint16_t rare_shown[2];        /* rare drops to draw in gold this battle */
    uint32_t rare_before[2];       /* bag count of each shown rare (play log) */
    int32_t rare_is_econ[2];
    int32_t rare_list_seen;        /* the items-found list has been shown */
    uint32_t junk_shown;           /* this battle's Junk, shown as "Junk xN" */
    int32_t junk_list_seen;        /* ... and its items-found list has been open */
    uint32_t bag_junk_before;      /* Junk in the bag when the battle paid out */
    int32_t econ_shown;            /* economy mod index on the list (as a Mod Permit), or -1 */
    uint32_t permits_before;       /* Mod Permits in the bag when it was listed */
    int32_t econ_list_seen;
} s_found = { .econ_shown = -1 };
static uint32_t s_frames, s_last_count_frame;
static int s_handin_active;
static uint32_t s_handin_left;     /* removals still to feed this hand-in */
static uint32_t s_scratch_list;   /* guest list of Junk the count reads */
static uint32_t s_saved_limit;
static int s_list_redirected;
static uint32_t s_free_frames;     /* consecutive frames of free movement */
static int s_color_saved;
static uint32_t s_saved_color, s_saved_color_shaded;
static int s_boss_battle;          /* an enemy this fight can't be escaped from */
static int s_wayne_waiting;        /* a hand-in reached 300 and he built nothing (before Day 4) */
static uint16_t s_wayne_prev;      /* Wayne's count during a hand-in */
static int s_wayne_dropped;        /* ... it went down: he made a gun */
static uint32_t s_junk_label;      /* "Junk xN" in game text, mod memory */
static uint32_t s_econ_label;      /* "<mod> mod" in game text, mod memory */
static uint16_t s_steal_item;      /* Aya just stole this; highlight its name */
static uint32_t s_steal_frame;

static const char *const s_handin_names[] = { "Next gun", "All Junk" };

static int pe_game_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_in_battle(void) {
    return (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE) != 0;
}

static int pe_free_field(void) {
    return !(psx_mod_read_half(PE_FIELD_BUSY_ADDR) & 0xB600u) &&
           !(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2006u);
}

static int pe_ram_pointer(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u;
}

static void pe_add_junk(uint32_t n) {
    s_st.junk = s_st.junk + n > 99999u ? 99999u : s_st.junk + n;
}

/* Super Junk and Duper Junk are Junk bundles while the economy is on: worth a
 * quarter and a half of a Wayne gun (his 300), as vanilla's (never usable)
 * effect was a quarter and a half of max PE. The sweep turns them into Junk
 * wherever they land: a rare drop, Wayne's bad roll (that is his refund now,
 * pe_wayne_refund.c steps aside), or an old save's bag (2026-10-13). */
#define PE_ITEM_SUPER_JUNK  0x0Bu
#define PE_ITEM_DUPER_JUNK  0x0Cu
#define PE_SUPER_JUNK_VALUE 75u
#define PE_DUPER_JUNK_VALUE 150u

static uint32_t pe_bundle_value(uint16_t v) {
    return v == PE_ITEM_SUPER_JUNK ? PE_SUPER_JUNK_VALUE : v == PE_ITEM_DUPER_JUNK ? PE_DUPER_JUNK_VALUE : 0u;
}

/* pe_loot.h */
int pe_loot_enabled(void) { return s_enabled; }
uint32_t pe_loot_junk(void) { return s_st.junk; }
int pe_loot_spend_junk(uint32_t n) {
    if (s_st.junk < n) return 0;
    s_st.junk -= n;
    return 1;
}
void pe_loot_give_junk(uint32_t n) { pe_add_junk(n); }

/* The equipped armor carries effect `effect` (low 5 bits of a mod byte). */
int pe_loot_armor_has(int effect) {
    const int slot = (int8_t)psx_mod_read_byte(PE_ARMOR_SLOT_ADDR);
    if (slot < 0 || slot >= (int)PE_AYA_SLOT_COUNT) return 0;
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u);
    if (v < 256 || v >= 384) return 0;
    const uint32_t rec = PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * 0x20u;
    if (psx_mod_read_byte(rec + 0x06u) != 9) return 0;
    int n = psx_mod_read_byte(rec + 0x14u);
    if (n > 10) n = 10;
    for (int m = 0; m < n; ++m)
        if ((psx_mod_read_byte(rec + 0x15u + (uint32_t)m) & 0x1F) == effect) return 1;
    return 0;
}

/* A price after the preset and Bargain (C4); callers apply their caps after. */
uint32_t pe_loot_price(uint32_t base) {
    if (!base) return 0;
    uint32_t pct = s_preset_price[s_st.preset < 3 ? s_st.preset : 1];
    if (pe_loot_armor_has(PE_FX_BARGAIN)) pct = pct * 85u / 100u;
    const uint32_t p = (base * pct + 50u) / 100u;
    return p ? p : 1u;
}

/* ---- rare drops (phase 2) ---------------------------------------------------- */
#define PE_RARE_BOOSTER 0u                 /* table marker: pick a stat booster */
#define PE_RARE_ECON    1u                 /* table marker: unlock an economy mod */
/* Cascading rare drops (bridge findings pe-loot-cascade-design-2026-10-14.md):
 * once the pity gate passes, walk this list from the top; an entry whose skip
 * check says it is no use now is passed over (its share falls through), the
 * first entry to pass its check and its roll (or its guarantee) is the drop,
 * and the stat booster at the end always hits. Rarest and most valuable first. */
enum { RC_ECON, RC_SUPER_TOOL, RC_PERMIT, RC_CHARM, RC_DUPER, RC_SUPER, RC_TOOL, RC_BOOSTER, RC_COUNT };
static const struct { uint8_t chance; uint16_t item; const char *name; } s_cascade[RC_COUNT] = {
    [RC_ECON]       = { 15, PE_RARE_ECON,        "econ" },    /* tiered, guarantee */
    [RC_SUPER_TOOL] = {  6, 0x17u,               "supertool" },
    [RC_PERMIT]     = { 15, 0x37u,               "permit" },  /* guarantee */
    [RC_CHARM]      = {  8, PE_ITEM_LUCKY_CHARM, "charm" },
    [RC_DUPER]      = {  4, 0x0Cu,               "duper" },   /* worth 150 Junk */
    [RC_SUPER]      = {  8, 0x0Bu,               "super" },   /* worth 75 Junk */
    [RC_TOOL]       = {  0, 0x16u,               "tool" },    /* chance by Tools held */
    [RC_BOOSTER]    = {100, PE_RARE_BOOSTER,     "booster" },
};
static const uint8_t s_tool_chance[5] = { 85, 65, 40, 20, 10 };   /* by Tools held; 5+: skipped */
/* Economy mods, with drop weights by worth (Salvager most often). */
static const struct { int effect; const char *name; uint8_t weight; } s_econ_mods[] = {
    { PE_FX_SCAVENGER, "Scavenger mod", 1 },
    { PE_FX_LUCKY_FIND, "Lucky Find mod", 2 },
    { PE_FX_SALVAGER, "Salvager mod", 4 },
    { PE_FX_BARGAIN, "Bargain mod", 3 },
};
static char s_cascade_path[96];             /* the last pick, for the play log */
#define PE_ITEM_MOD_PERMIT 0x37u           /* carries an economy mod on the items-found screen */
#define PE_ECON_DROP       0x8000u         /* a rare "add": 0x8000 | econ index */
#define PE_ECON_COUNT 4
/* Offense, Range, Bullet Cap, Defense, CR Evade, PE: tier 1 at these ids. */
static const uint16_t s_booster_base[] = { 0x1Du, 0x21u, 0x25u, 0x29u, 0x2Du, 0x31u };

static uint32_t pe_rand(void) {
    uint32_t x = s_st.rng ? s_st.rng : (0x9E3779B9u ^ (s_frames * 2654435761u)) | 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_st.rng = x;
    return x;
}

/* Economy mods not unlocked yet, written to `out`; returns how many. */
static int pe_econ_missing(int *out) {
    int n = 0;
    for (int i = 0; i < PE_ECON_COUNT; ++i)
        if (!pe_catalog_armor_unlocked(s_econ_mods[i].effect)) out[n++] = i;
    return n;
}

/* Item value anywhere in the bag or Wayne's storage. */
static int pe_carried(uint16_t v) {
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == v) return 1;
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i)
        if (psx_mod_read_half(PE_STORAGE_ADDR + i * 2u) == v) return 1;
    return 0;
}

/* Count of item value v in the bag and Wayne's storage. */
static uint32_t pe_held(uint16_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i) n += psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == v;
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i) n += psx_mod_read_half(PE_STORAGE_ADDR + i * 2u) == v;
    return n;
}

/* An economy mod not unlocked yet, weighted by worth. */
static int pe_pick_econ(const int *missing, int n) {
    uint32_t total = 0;
    for (int i = 0; i < n; ++i) total += s_econ_mods[missing[i]].weight;
    uint32_t r = pe_rand() % (total ? total : 1u);
    for (int i = 0; i < n; ++i) {
        if (r < s_econ_mods[missing[i]].weight) return missing[i];
        r -= s_econ_mods[missing[i]].weight;
    }
    return missing[0];
}

/* Is cascade entry k of no use now? Its chance in *pct (Tools: by Tools held). */
static int pe_cascade_skip(int k, int econ_missing, uint32_t *pct) {
    *pct = s_cascade[k].chance;
    switch (k) {
    case RC_ECON:       return econ_missing == 0;
    case RC_SUPER_TOOL: return pe_carried(0x36u);                    /* Super Tool Kit */
    case RC_PERMIT:     return pe_held(0x37u) >= 2u;                  /* spend one first */
    case RC_CHARM:      return 1;   /* removed 2026-10-15: item 0x19 is Panacea now */
    case RC_TOOL: {
        if (pe_carried(0x35u) || pe_carried(0x36u)) return 1;         /* either kit: Tools are free */
        const uint32_t held = pe_held(0x16u);
        if (held >= 5u) return 1;
        *pct = s_tool_chance[held];
        return 0;
    }
    default:            return 0;
    }
}

static uint16_t pe_pick_rare(uint32_t exp) {
    int missing[PE_ECON_COUNT];
    const int econ = pe_econ_missing(missing);
    int hit = RC_BOOSTER;
    unsigned o = 0;
    s_cascade_path[0] = '\0';
    for (int k = 0; k < RC_COUNT; ++k) {
        uint32_t pct;
        if (pe_cascade_skip(k, econ, &pct)) {
            if (o < sizeof s_cascade_path - 12u)
                o += (unsigned)snprintf(s_cascade_path + o, sizeof s_cascade_path - o, "-%s", s_cascade[k].name);
            continue;
        }
        uint32_t guarantee = 0, since = 0;
        if (k == RC_ECON) { guarantee = (uint32_t)s_tune_econ_guarantee; since = s_st.econ_since; }
        if (k == RC_PERMIT) { guarantee = (uint32_t)s_tune_permit_guarantee; since = s_st.permit_since; }
        const int certain = guarantee && since + 1u >= guarantee;
        if (k == RC_BOOSTER || certain || pe_rand() % 100u < pct) {
            hit = k;
            if (o < sizeof s_cascade_path - 16u)
                snprintf(s_cascade_path + o, sizeof s_cascade_path - o, "+%s%s", s_cascade[k].name, certain ? "!" : "");
            break;
        }
    }
    /* Guarantees count rares without the entry, only while it isn't skipped. */
    if (econ) s_st.econ_since = hit == RC_ECON ? 0 : (uint8_t)(s_st.econ_since < 250 ? s_st.econ_since + 1 : 250);
    if (pe_held(0x37u) < 2u)
        s_st.permit_since = hit == RC_PERMIT ? 0 : (uint8_t)(s_st.permit_since < 250 ? s_st.permit_since + 1 : 250);
    if (hit == RC_ECON)                           /* shown as "<mod> mod", taken to unlock */
        return (uint16_t)(PE_ECON_DROP | (uint32_t)pe_pick_econ(missing, econ));
    if (hit != RC_BOOSTER) return s_cascade[hit].item;
    const uint32_t tier = exp < 20u ? 0u : exp < 60u ? 1u : exp < 150u ? 2u : 3u;
    return (uint16_t)(s_booster_base[pe_rand() % 6u] + tier);
}

/* Rare guarantee after the preset (39). */
static uint32_t pe_rare_guarantee(void) {
    const uint32_t g = (uint32_t)s_tune_rare_guarantee * s_preset_pity[s_st.preset < 3 ? s_st.preset : 1] / 100u;
    return g ? g : 1u;
}

/* Pity progress a dry battle adds: 1, x2 with Lucky Find (C2), x2 while a
 * Lucky Charm is active (C7). */
static uint32_t pe_pity_step(void) {
    uint32_t step = pe_loot_armor_has(PE_FX_LUCKY_FIND) ? 2u : 1u;
    if (s_st.charm_left) step *= 2u;
    return step;
}

/* Soft pity: base + step per unit of progress, certain at the guarantee. */
static uint16_t pe_roll_rare(uint32_t exp) {
    const uint32_t dry = s_st.rare_dry;
    const uint32_t preset = s_st.preset < 3 ? s_st.preset : 1u;
    /* Per mille, so the presets can scale base and step (7). */
    const uint32_t chance = (uint32_t)s_tune_rare_base * 10u * s_preset_pity_base[preset] / 100u +
                            (uint32_t)s_tune_rare_step * 10u * s_preset_pity_step[preset] / 100u * dry;
    const int certain = dry + 1u >= pe_rare_guarantee();
    if (certain || pe_rand() % 1000u < chance) {
        s_st.rare_dry = 0;
        return pe_pick_rare(exp);
    }
    const uint32_t next = dry + pe_pity_step();
    s_st.rare_dry = next > 0xFFFFu ? 0xFFFFu : (uint16_t)next;
    return 0;
}

/* The Lucky Charm is gone (2026-10-15: its item, 0x19, is Panacea, and a carried charm simply
 * becomes one); a charm already running counts down its remaining battles. */
static void pe_charm_battle_won(void) {
    if (s_st.charm_left) --s_st.charm_left;
}

static uint32_t pe_bag_count_of(uint16_t v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == v) ++n;
    return n;
}

static uint32_t pe_bag_junk_count(void) { return pe_bag_count_of(PE_ITEM_JUNK); }


/* ---- battle end ------------------------------------------------------------ */
static void pe_on_set_total_exp(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled) return;
    const uint32_t exp = cpu->gpr[4];
    const uint32_t list = cpu->gpr[6];

    /* Floor + EXP-scaled, capped, with the no-hit bonus (16, 17, 32). */
    const uint32_t per = s_tune_exp_per_junk > 0 ? (uint32_t)s_tune_exp_per_junk : 1u;
    uint32_t earned = (uint32_t)s_tune_floor + exp / per;
    const uint32_t cap = (uint32_t)(s_boss_battle ? s_tune_boss_cap : s_tune_battle_cap);
    if (earned > cap) earned = cap;
    uint32_t hits = 0;
    for (int s = 0; s < PE_ENEMY_SLOTS; ++s) hits += s_st.hits[s];
    if (hits < 4) earned += earned * (uint32_t)s_tune_nohit_bonus * (4u - hits) / 400u;
    earned = earned * s_preset_junk[s_st.preset < 3 ? s_st.preset : 1] / 100u;   /* 39 */
    if (pe_loot_armor_has(PE_FX_SCAVENGER)) earned += earned / 2u;               /* C1 */

    /* Vanilla Junk leaves the reward list for the counter; the battle's whole
     * Junk comes back as one "Junk xN" entry at the end. Last, because Take all
     * (Inv_TransferItemAlt) puts entries into free bag slots in list order: at
     * the top it took the last free slot from a rare. Junk needs no slot, so if
     * none is left for it, it is paid when the list closes all the same. */
    uint32_t vanilla = 0;
    s_found.junk_shown = 0;
    s_found.junk_list_seen = 0;
    if (pe_ram_pointer(list)) {
        uint32_t keep[PE_REWARD_SLOTS];
        uint32_t kept = 0;
        for (uint32_t i = 0; i < PE_REWARD_SLOTS; ++i) {
            const uint32_t entry = psx_mod_read_word(list + i * 4u);
            if ((entry & 0xFFFFu) == 0) break;
            if ((entry & 0xFFFFu) == PE_ITEM_JUNK) ++vanilla;
            else keep[kept++] = entry;
        }
        const uint32_t shown = earned + vanilla + s_st.steal_bank;
        /* Rare drop (and one carried over from a full list) after the vanilla
         * items, as {id, extra = -1}. */
        pe_charm_battle_won();
        const unsigned dry_before = s_st.rare_dry, carried = s_st.rare_carry;
        const uint16_t rolled = pe_roll_rare(exp);
        pe_log("loot", "exp=%u junk=%u hits=%u vanilla=%u steal=%u shown=%u boss=%d preset=%u scav=%d rare_dry=%u rolled=0x%04X carried=0x%04X charm=%u path=%s",
               (unsigned)exp, (unsigned)earned, (unsigned)hits, (unsigned)vanilla,
               (unsigned)s_st.steal_bank, (unsigned)s_found.junk_shown, s_boss_battle, (unsigned)s_st.preset,
               pe_loot_armor_has(PE_FX_SCAVENGER), dry_before, rolled, carried, (unsigned)s_st.charm_left,
               rolled ? s_cascade_path : "-");
        const uint16_t adds[2] = { s_st.rare_carry, rolled };
        s_st.rare_carry = 0;
        s_found.rare_shown[0] = s_found.rare_shown[1] = 0;
        s_found.rare_list_seen = 0;
        s_found.econ_shown = -1;
        s_found.econ_list_seen = 0;
        for (int a = 0; a < 2; ++a) {
            if (!adds[a]) continue;
            uint16_t id = adds[a];
            /* A carried-over economy mod may have been unlocked since (bought at
             * Requisition): roll a fresh rare instead of a duplicate. */
            if ((id & PE_ECON_DROP) &&
                pe_catalog_armor_unlocked(s_econ_mods[(id & 0xFFu) % PE_ECON_COUNT].effect))
                id = pe_pick_rare(exp);
            if (id & PE_ECON_DROP) {
                /* Carried by a Mod Permit entry: never next to a real one, never two. */
                if (s_found.econ_shown >= 0 || adds[1 - a] == PE_ITEM_MOD_PERMIT || kept >= PE_REWARD_SLOTS) {
                    if (!s_st.rare_carry) s_st.rare_carry = id;
                    continue;
                }
                s_found.econ_shown = (int)(id & 0xFFu) % PE_ECON_COUNT;
                s_found.permits_before = pe_bag_count_of(PE_ITEM_MOD_PERMIT);
                id = PE_ITEM_MOD_PERMIT;
            }
            if (kept < PE_REWARD_SLOTS) {
                keep[kept++] = 0xFFFF0000u | id;
                s_found.rare_shown[a] = id;                /* drawn in gold */
                s_found.rare_before[a] = pe_bag_count_of(id);
                s_found.rare_is_econ[a] = id == PE_ITEM_MOD_PERMIT && s_found.econ_shown >= 0;
            } else {
                s_st.rare_carry = id;                /* next battle (never an economy mod here) */
            }
        }
        if (shown && kept < PE_REWARD_SLOTS) {   /* no room: paid straight away, below */
            keep[kept++] = 0xFFFF0000u | PE_ITEM_JUNK;
            s_found.junk_shown = shown;
            s_found.bag_junk_before = pe_bag_junk_count();
        }
        for (uint32_t i = 0; i < PE_REWARD_SLOTS; ++i)
            psx_mod_write_word(list + i * 4u, i < kept ? keep[i] : 0xFFFF0000u);
    }

    const uint32_t total = earned + vanilla + s_st.steal_bank;
    s_st.steal_bank = 0;
    if (!s_found.junk_shown) pe_add_junk(total);   /* else paid when the "Junk xN" entry is taken */
}

/* ---- gold names for rare drops on the items-found screen ---------------------- */
/* Address of Str_LookupTable8(id - 1) (item names), or 0. */
static uint32_t pe_item_name_text(unsigned id) {
    const uint32_t directory = PE_STR_ORIGIN + psx_mod_read_word(PE_STR_DIRECTORY_OFFSET);
    if (!pe_ram_pointer(directory) || !id) return 0;
    const uint32_t table = directory + psx_mod_read_word(directory + 0x08u);
    if (!pe_ram_pointer(table) || id - 1u >= psx_mod_read_half(table)) return 0;
    return table + (uint32_t)(int32_t)(int16_t)psx_mod_read_half(table + 2u + (id - 1u) * 2u);
}

static void pe_restore_text_color(void) {
    if (!s_color_saved) return;
    psx_mod_write_word(PE_DRAW_COLOR, s_saved_color);
    psx_mod_write_word(PE_DRAW_COLOR_SHADED, s_saved_color_shaded);
    s_color_saved = 0;
}

/* Draw_PrintRawText(text) entry: the color is read while the text is drawn, so
 * a rare drop's name row gets gold here and the next text call restores it. */
/* ASCII letters, digits and spaces to game text, in mod memory. */
static uint32_t pe_ascii_label(uint32_t *slot, const char *ascii) {
    if (!*slot) *slot = psx_mod_alloc_guest_memory(32, 4);
    if (!*slot) return 0;
    uint32_t o = 0;
    for (; *ascii && o < 30u; ++ascii, ++o) {
        const char c = *ascii;
        const uint8_t g = (c >= 'A' && c <= 'Z') ? (uint8_t)(0x10 + c - 'A') :
                          (c >= 'a' && c <= 'z') ? (uint8_t)(0x30 + c - 'a') :
                          (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : 0x0Fu;
        psx_mod_write_byte(*slot + o, g);
    }
    psx_mod_write_byte(*slot + o, 0xFFu);
    return *slot;
}

/* "Junk xN" in game text: the game's own name for Junk, then " x" and N. */
static uint32_t pe_junk_label(uint32_t n) {
    if (!s_junk_label) s_junk_label = psx_mod_alloc_guest_memory(32, 4);
    const uint32_t name = pe_item_name_text(PE_ITEM_JUNK);
    if (!s_junk_label || !name) return 0;
    uint32_t o = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint8_t c = psx_mod_read_byte(name + i);
        if (c == 0xFFu) break;
        psx_mod_write_byte(s_junk_label + o++, c);
    }
    char digits[12];
    snprintf(digits, sizeof digits, "%u", (unsigned)n);
    psx_mod_write_byte(s_junk_label + o++, 0x0Fu);              /* space */
    psx_mod_write_byte(s_junk_label + o++, 0x30u + ('x' - 'a'));
    for (const char *d = digits; *d && o < 30u; ++d) psx_mod_write_byte(s_junk_label + o++, (uint8_t)(*d - '0'));
    psx_mod_write_byte(s_junk_label + o, 0xFFu);
    return s_junk_label;
}

static void pe_on_print_raw_text(struct CPUState *cpu, uint32_t address) {
    (void)address;
    pe_restore_text_color();
    if (!s_found.rare_shown[0] && !s_found.rare_shown[1] && !s_found.junk_shown) return;
    const uint32_t ra = cpu->gpr[31];
    if (ra < PE_FN_SLOT_ROW_START || ra >= PE_FN_SLOT_ROW_END) return;
    const uint32_t override = psx_mod_read_word(PE_INV_OVERRIDE_LIST);
    if (!override || psx_mod_read_word(PE_ACTIVE_LIST_PTR) != override) return;
    const uint32_t text = cpu->gpr[4];
    if (s_found.junk_shown && text == pe_item_name_text(PE_ITEM_JUNK)) {
        const uint32_t label = pe_junk_label(s_found.junk_shown);
        if (label) cpu->gpr[4] = label;
        return;
    }
    if (s_found.econ_shown >= 0 && text == pe_item_name_text(PE_ITEM_MOD_PERMIT)) {
        const uint32_t label = pe_ascii_label(&s_econ_label, s_econ_mods[s_found.econ_shown].name);
        if (label) cpu->gpr[4] = label;      /* and gold, below */
    }
    for (int a = 0; a < 2; ++a) {
        if (!s_found.rare_shown[a] || pe_item_name_text(s_found.rare_shown[a]) != text) continue;   /* text: before any swap */
        s_saved_color = psx_mod_read_word(PE_DRAW_COLOR);
        s_saved_color_shaded = psx_mod_read_word(PE_DRAW_COLOR_SHADED);
        psx_mod_write_word(PE_DRAW_COLOR, PE_RARE_COLOR);
        psx_mod_write_word(PE_DRAW_COLOR_SHADED, (PE_RARE_COLOR >> 1) & 0x7F7F7Fu);
        s_color_saved = 1;
        return;
    }
}

/* ---- pickups and steals ---------------------------------------------------- */
static void pe_on_inv_add_item(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t ra = cpu->gpr[31];
    const int from_steal = ra >= PE_FN_RESOLVE_HIT_START && ra < PE_FN_RESOLVE_HIT_END;
    if (from_steal && cpu->gpr[4]) {
        s_steal_item = (uint16_t)cpu->gpr[4];     /* its message gets the red name */
        s_steal_frame = s_frames;
    }
    /* An empty steal (the enemy's item is gone) banks a little Junk (19, 35). */
    if (from_steal && !cpu->gpr[4] && s_enabled && pe_game_loaded()) {
        const uint32_t bank = (uint32_t)s_st.steal_bank + (uint32_t)s_tune_empty_steal;
        s_st.steal_bank = bank > 0xFFFFu ? 0xFFFFu : (uint16_t)bank;
        return;
    }
    if (!s_enabled || !pe_game_loaded() || cpu->gpr[4] != PE_ITEM_JUNK) return;
    cpu->gpr[4] = 0;   /* Inv_AddItem(0): adds nothing, returns success */
    if (s_found.junk_shown && psx_mod_read_word(PE_INV_OVERRIDE_LIST) != 0) {
        pe_log("junk_take", "n=%u", (unsigned)s_found.junk_shown);
        pe_add_junk(s_found.junk_shown);           /* the "Junk xN" entry taken */
        s_found.junk_shown = 0;
        return;
    }
    if (ra >= PE_FN_RESOLVE_HIT_START && ra < PE_FN_RESOLVE_HIT_END) {
        if (s_st.steal_bank < 0xFFFFu) ++s_st.steal_bank;   /* paid on a win (41) */
    } else {
        pe_add_junk(1);                          /* the game's pickup message names it */
    }
}

/* ---- Wayne's hand-in ------------------------------------------------------- */
static int pe_bag_has_junk(uint32_t limit) {
    for (uint32_t i = 0; i < limit && i < PE_AYA_SLOT_COUNT; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == PE_ITEM_JUNK) return 1;
    return 0;
}

/* Free bag slot inside the bag's limit, or -1. */
static int pe_free_bag_slot(uint32_t limit) {
    for (uint32_t i = 0; i < limit && i < PE_AYA_SLOT_COUNT; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == 0) return (int)i;
    return -1;
}

static void pe_restore_active_list(void) {
    if (!s_list_redirected) return;
    psx_mod_write_word(PE_ACTIVE_LIST_PTR, PE_AYA_SLOTS_ADDR);
    psx_mod_write_word(PE_ACTIVE_LIST_LIMIT, s_saved_limit);
    s_list_redirected = 0;
}

/* The script counts Junk once and then removes that many, one by one. So the
 * count is pointed at a scratch list of N Junk (guest memory the count can
 * read), and each removal gets one real Junk placed just in time. */
static void pe_on_task_count_items(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled || !pe_game_loaded()) return;
    const uint32_t args = cpu->gpr[4];
    if (!pe_ram_pointer(args)) return;
    const uint32_t id_ptr = psx_mod_read_word(args);
    if (!pe_ram_pointer(id_ptr) || psx_mod_read_word(id_ptr) != PE_ITEM_JUNK) return;

    const int new_handin = !s_handin_active ||
                           (!s_handin_left &&
                            s_frames - s_last_count_frame > PE_HANDIN_GUARD_FRAMES);
    s_handin_active = 1;
    s_last_count_frame = s_frames;
    if (!new_handin) return;                 /* one hand-in per visit to the menu */
    s_handin_left = 0;
    s_wayne_prev = psx_mod_read_half(PE_WAYNE_COUNTER);
    s_wayne_dropped = 0;

    if (psx_mod_read_word(PE_ACTIVE_LIST_PTR) != PE_AYA_SLOTS_ADDR) return;
    const uint32_t limit = psx_mod_read_word(PE_ACTIVE_LIST_LIMIT);
    if (s_st.junk == 0 || pe_bag_has_junk(limit)) return;

    /* He crafts when a hand-in takes him to 300+ and then subtracts 300, so
     * below 300 the next gun needs 300 - n; at 300+ (after a refund) one Junk. */
    const unsigned wayne = psx_mod_read_half(PE_WAYNE_COUNTER);
    uint32_t n = s_st.junk;
    if (s_handin_mode == 0) {
        const uint32_t needed = wayne < PE_WAYNE_CRAFT_COST ? PE_WAYNE_CRAFT_COST - wayne : 1u;
        if (n > needed) n = needed;
    }
    if (n > PE_HANDIN_MAX) n = PE_HANDIN_MAX;
    if (pe_free_bag_slot(limit) < 0) return; /* his "Discard junk" row says "bag full" */
    if (!s_scratch_list) {
        s_scratch_list = psx_mod_alloc_guest_memory(PE_HANDIN_MAX * 2u, 4);
        if (!s_scratch_list) return;
    }
    for (uint32_t i = 0; i < n; ++i) psx_mod_write_half(s_scratch_list + i * 2u, PE_ITEM_JUNK);
    s_saved_limit = limit;
    psx_mod_write_word(PE_ACTIVE_LIST_PTR, s_scratch_list);
    psx_mod_write_word(PE_ACTIVE_LIST_LIMIT, n);
    s_list_redirected = 1;
    s_handin_left = n;
}

/* ---- inventory commands: Wayne's removals, new game --------------------------- */
static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const int command = (int)cpu->gpr[4];
    if (command == PE_INV_CMD_START_NEW_GAME) {   /* difficulty and the switch stay */
        const uint8_t preset = s_st.preset < 3 ? s_st.preset : 1;
        memset(&s_st, 0, sizeof s_st);
        s_st.preset = preset;
        s_st.disabled = (uint8_t)!s_enabled;
        return;
    }
    if (command != PE_INV_CMD_REMOVE_ITEM || cpu->gpr[5] != PE_ITEM_JUNK) return;
    pe_restore_active_list();                /* the body re-selects Aya's bag anyway */
    if (!s_handin_left || !s_st.junk) return;
    const uint32_t limit = s_saved_limit ? s_saved_limit : psx_mod_read_word(PE_ACTIVE_LIST_LIMIT);
    const int slot = pe_free_bag_slot(limit);
    if (slot < 0) return;
    psx_mod_write_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u, PE_ITEM_JUNK);
    --s_st.junk;
    --s_handin_left;
    s_last_count_frame = s_frames;           /* keep the sweep away mid-hand-in */
}

/* ---- memory-card save tail ------------------------------------------------- */
static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    psx_mod_write_word(at + 0u, PE_TAIL_MAGIC);
    const uint32_t packed = (uint32_t)s_st.rare_dry | ((uint32_t)s_st.rare_carry << 16);
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, s_st.junk);
    psx_mod_write_word(at + 12u, packed);
    const uint32_t extra = (uint32_t)s_st.charm_left | ((uint32_t)s_st.preset << 16) |
                           ((uint32_t)(s_st.disabled & 1u) << 24);
    psx_mod_write_word(at + 16u, extra);
    psx_mod_write_word(at + 20u, s_st.junk ^ packed ^ extra ^ 0xA5A5A5A5u);
    const uint32_t since = (uint32_t)s_st.econ_since | ((uint32_t)s_st.permit_since << 8);   /* v4 */
    psx_mod_write_word(at + 24u, since);
    psx_mod_write_word(at + 28u, since ^ 0x5A5A5A5Au);
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    const uint8_t preset = s_st.preset < 3 ? s_st.preset : 1;   /* a vanilla save keeps it */
    memset(&s_st, 0, sizeof s_st);   /* a vanilla save: start at 0, the sweep converts */
    s_st.preset = preset;
    s_st.disabled = (uint8_t)!s_enabled;
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC) return;
    const uint32_t version = psx_mod_read_word(at + 4u);
    const uint32_t junk = psx_mod_read_word(at + 8u);
    if (junk > 99999u) return;
    if (version == 1u) {                       /* phase 1 saves: Junk only */
        if ((junk ^ 0xA5A5A5A5u) == psx_mod_read_word(at + 12u)) s_st.junk = junk;
    } else if (version == 2u) {
        const uint32_t packed = psx_mod_read_word(at + 12u);
        if ((junk ^ packed ^ 0xA5A5A5A5u) != psx_mod_read_word(at + 16u)) return;
        s_st.junk = junk;
        s_st.rare_dry = (uint16_t)packed;
        s_st.rare_carry = (uint16_t)(packed >> 16);
    } else if (version == 3u || version == 4u) {
        const uint32_t packed = psx_mod_read_word(at + 12u);
        const uint32_t extra = psx_mod_read_word(at + 16u);
        if ((junk ^ packed ^ extra ^ 0xA5A5A5A5u) != psx_mod_read_word(at + 20u)) return;
        s_st.junk = junk;
        s_st.rare_dry = (uint16_t)packed;
        s_st.rare_carry = (uint16_t)(packed >> 16);
        s_st.charm_left = (uint16_t)extra;
        s_st.preset = (uint8_t)(extra >> 16) < 3 ? (uint8_t)(extra >> 16) : 1;
        s_st.disabled = (uint8_t)((extra >> 24) & 1u);
        const uint32_t since = psx_mod_read_word(at + 24u);
        if (version == 4u && (since ^ 0x5A5A5A5Au) == psx_mod_read_word(at + 28u)) {
            s_st.econ_since = (uint8_t)since;
            s_st.permit_since = (uint8_t)(since >> 8);
        }
    }
    s_enabled = !s_st.disabled;      /* the save's own choice */
}

/* ---- per frame -------------------------------------------------------------- */
static void pe_sweep(void) {
    if (s_found.junk_shown) return;                /* the items-found Junk line is still settling */
    uint32_t moved = 0;
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT + PE_STORAGE_COUNT; ++i) {
        const uint32_t a = i < PE_AYA_SLOT_COUNT ? PE_AYA_SLOTS_ADDR + i * 2u
                                                 : PE_STORAGE_ADDR + (i - PE_AYA_SLOT_COUNT) * 2u;
        const uint16_t v = psx_mod_read_half(a);
        if (v == PE_ITEM_JUNK) { psx_mod_write_half(a, 0); ++moved; continue; }
        const uint32_t bundle = pe_bundle_value(v);
        if (bundle) {
            psx_mod_write_half(a, 0);
            moved += bundle;
            pe_log("bundle", "item=0x%02X junk=%u", (unsigned)v, (unsigned)bundle);
        }
    }
    if (moved) pe_add_junk(moved);           /* silently: in-game messages only */
}

/* Wrap the stolen item's name in the steal message with the textbox's red
 * highlight. The message is built right after the steal (Inv_GetItemEffectData
 * into D_800A1B50: name + suffix, or suffix + name); "bag full" and "nothing"
 * messages hold no name and are left alone. */
static void pe_highlight_steal_message(void) {
    if (!s_steal_item) return;
    if (s_frames - s_steal_frame > 30u) { s_steal_item = 0; return; }
    if (psx_mod_read_word(PE_STEAL_MSG_PTR) != PE_ITEM_MSG_BUF) return;
    uint8_t msg[PE_ITEM_MSG_SIZE], name[PE_ITEM_MSG_SIZE];
    uint32_t msg_len = 0, name_len = 0;
    while (msg_len < PE_ITEM_MSG_SIZE &&
           (msg[msg_len] = psx_mod_read_byte(PE_ITEM_MSG_BUF + msg_len)) != 0xFFu) ++msg_len;
    const uint32_t name_ptr = pe_item_name_text(s_steal_item);
    s_steal_item = 0;
    if (msg_len >= PE_ITEM_MSG_SIZE || !name_ptr) return;
    while (name_len < PE_ITEM_MSG_SIZE &&
           (name[name_len] = psx_mod_read_byte(name_ptr + name_len)) != 0xFFu) ++name_len;
    if (!name_len || name_len > msg_len || msg_len + 5u > PE_ITEM_MSG_SIZE) return;
    for (uint32_t at = 0; at + name_len <= msg_len; ++at) {
        if (memcmp(msg + at, name, name_len) != 0) continue;
        /* rebuild: before, FB 04, name, FB 05, after, FF */
        uint32_t o = PE_ITEM_MSG_BUF;
        for (uint32_t i = 0; i < at; ++i) psx_mod_write_byte(o++, msg[i]);
        psx_mod_write_byte(o++, 0xFBu); psx_mod_write_byte(o++, 0x04u);
        for (uint32_t i = 0; i < name_len; ++i) psx_mod_write_byte(o++, name[i]);
        psx_mod_write_byte(o++, 0xFBu); psx_mod_write_byte(o++, 0x05u);
        for (uint32_t i = at + name_len; i < msg_len; ++i) psx_mod_write_byte(o++, msg[i]);
        psx_mod_write_byte(o, 0xFFu);
        return;
    }
}

/* Turned off: nothing of the overhaul may stay half-done, and the game runs
 * as vanilla (Junk back in the bag, Wayne counts it, Tools as usual). The
 * counter is kept; "Take out Junk" moves it into the bag and trunk. */
static void pe_loot_switched_off(void) {
    pe_restore_active_list();                /* never leave a hand-in redirect behind */
    s_handin_active = 0;                     /* a started hand-in still gets its Junk */
    s_st.steal_bank = 0;
    s_found.rare_shown[0] = s_found.rare_shown[1] = 0;
    s_found.junk_shown = 0;
    s_found.econ_shown = -1;
    pe_restore_text_color();
    s_in_battle = 0;
    if (!pe_game_loaded()) return;
    char msg[96];
    snprintf(msg, sizeof msg, "Junk economy off: %u Junk kept (Workshop > Take out Junk)",
             (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}

/* Turned on: the sweep moves Junk from the bag and trunk into the counter at
 * the next free moment; a battle in progress pays nothing extra. */
static void pe_loot_switched_on(void) {
    s_in_battle = pe_game_loaded() && pe_in_battle();
    s_st.steal_bank = 0;
    if (pe_game_loaded())
        psx_mod_osd_message("Junk economy on: Junk in the bag and trunk joins the counter");
}

/* Junk last on the items-found list (see pe_on_set_total_exp), also for a list
 * built before this rule (a save state on that screen). */
static void pe_junk_entry_last(void) {
    if (psx_mod_read_word(PE_INV_OVERRIDE_LIST) != PE_FOUND_ITEMS) return;
    uint32_t n = psx_mod_read_word(PE_FOUND_COUNT);
    if (n > 10u) n = 10u;
    uint16_t items[10];
    uint32_t last = 0, junk = n;
    for (uint32_t i = 0; i < n; ++i) {
        items[i] = psx_mod_read_half(PE_FOUND_ITEMS + i * 2u);
        if (items[i]) last = i;
        if (items[i] == PE_ITEM_JUNK && junk == n) junk = i;
    }
    if (junk >= last) return;                /* none, or already last */
    for (uint32_t i = junk; i < last; ++i) items[i] = items[i + 1];
    items[last] = PE_ITEM_JUNK;
    for (uint32_t i = junk; i <= last; ++i) psx_mod_write_half(PE_FOUND_ITEMS + i * 2u, items[i]);
}

/* Junk needs no bag slot, but the list only moves an entry into a free one:
 * with the bag full, the "Junk xN" line is paid where it stands and leaves. */
static void pe_junk_without_room(void) {
    if (psx_mod_read_word(PE_INV_OVERRIDE_LIST) != PE_FOUND_ITEMS) return;
    uint32_t limit = psx_mod_read_byte(0x800C0E0Cu) + psx_mod_read_word(0x8009D018u);
    if (limit > PE_AYA_SLOT_COUNT) limit = PE_AYA_SLOT_COUNT;
    if (pe_free_bag_slot(limit) >= 0) return;
    uint32_t n = psx_mod_read_word(PE_FOUND_COUNT);
    if (n > 10u) n = 10u;
    for (uint32_t i = 0; i < n; ++i) {
        if (psx_mod_read_half(PE_FOUND_ITEMS + i * 2u) != PE_ITEM_JUNK) continue;
        psx_mod_write_half(PE_FOUND_ITEMS + i * 2u, 0);
        const uint32_t pay = s_found.junk_shown ? s_found.junk_shown : 1u;   /* 1: a state from before the amount was kept */
        pe_log("junk_take", "n=%u no_room=1", (unsigned)pay);
        pe_add_junk(pay);
        s_found.junk_shown = 0;
        s_found.junk_list_seen = 0;
        return;
    }
}

static void pe_loot_tick(void) {
    static int s_was_enabled = -1;
    ++s_frames;
    if (s_was_enabled != s_enabled) {
        if (s_was_enabled == 1) pe_loot_switched_off();
        else if (s_was_enabled == 0) pe_loot_switched_on();
        s_was_enabled = s_enabled;
        s_st.disabled = (uint8_t)!s_enabled;
    }
    if (pe_game_loaded()) pe_highlight_steal_message();
    if (!s_enabled || !pe_game_loaded()) { s_in_battle = 0; return; }

    pe_restore_text_color();
    pe_junk_entry_last();
    pe_junk_without_room();
    if (s_found.junk_shown) {                      /* the "Junk xN" entry, not taken yet */
        /* Taken into the bag: straight back out, and all N into the counter. */
        uint32_t extra = pe_bag_junk_count();
        extra = extra > s_found.bag_junk_before ? extra - s_found.bag_junk_before : 0u;
        int taken = 0;
        for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT && extra; ++i) {
            const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
            if (psx_mod_read_half(a) == PE_ITEM_JUNK) { psx_mod_write_half(a, 0); --extra; taken = 1; }
        }
        if (taken) {
            pe_log("junk_take", "n=%u", (unsigned)s_found.junk_shown);
            pe_add_junk(s_found.junk_shown);
            s_found.junk_shown = 0;
            s_found.junk_list_seen = 0;
        } else if (psx_mod_read_word(PE_INV_OVERRIDE_LIST) != 0) {
            s_found.junk_list_seen = 1;
        } else if (s_found.junk_list_seen) {
            pe_log("junk_left", "n=%u paid=1", (unsigned)s_found.junk_shown);
            pe_add_junk(s_found.junk_shown);       /* left behind (no slot, or Done): it needs no slot */
            s_found.junk_shown = 0;
            s_found.junk_list_seen = 0;
        }
    }
    if (s_found.econ_shown >= 0) {                 /* the "<mod> mod" entry, not taken yet */
        if (pe_bag_count_of(PE_ITEM_MOD_PERMIT) > s_found.permits_before) {
            for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i) {
                const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
                if (psx_mod_read_half(a) == PE_ITEM_MOD_PERMIT) { psx_mod_write_half(a, 0); break; }
            }
            pe_catalog_unlock_armor(s_econ_mods[s_found.econ_shown].effect);
            pe_log("econ_unlock", "mod=%s source=drop", s_econ_mods[s_found.econ_shown].name);
            s_found.econ_shown = -1;
        } else if (psx_mod_read_word(PE_INV_OVERRIDE_LIST) != 0) {
            s_found.econ_list_seen = 1;
        } else if (s_found.econ_list_seen) {
            pe_log("econ_left", "mod=%s", s_econ_mods[s_found.econ_shown].name);
            s_found.econ_shown = -1;               /* left behind: lost (it can roll again) */
        }
    }
    if (s_found.rare_shown[0] || s_found.rare_shown[1]) {
        const int listed = psx_mod_read_word(PE_INV_OVERRIDE_LIST) != 0;
        if (listed) s_found.rare_list_seen = 1;
        else if (s_found.rare_list_seen) {
            for (int a = 0; a < 2; ++a) {
                if (!s_found.rare_shown[a] || s_found.rare_is_econ[a]) continue;
                const int took = pe_bag_count_of(s_found.rare_shown[a]) > s_found.rare_before[a];
                pe_log(took ? "rare_take" : "rare_left", "item=0x%02X", (unsigned)s_found.rare_shown[a]);
            }
            s_found.rare_shown[0] = s_found.rare_shown[1] = 0;
        }
    }

    const int battle = pe_in_battle();
    if (battle && !s_in_battle) {            /* a battle starts */
        s_found.rare_shown[0] = s_found.rare_shown[1] = 0;
        s_found.junk_shown = 0;
        s_found.econ_shown = -1;
        s_boss_battle = 0;
        s_st.steal_bank = 0;
        memset(s_st.hits, 0, sizeof s_st.hits);
    } else if (!battle && s_in_battle) {     /* it ended; a win already paid out */
        s_st.steal_bank = 0;                 /* fled: stolen Junk is forfeited (41) */
    }
    s_in_battle = battle;

    if (battle) {
        for (int s = 0; s < PE_ENEMY_SLOTS; ++s) {
            const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)s * PE_ENEMY_SLOT_STRIDE;
            if (!psx_mod_read_byte(slot)) continue;
            const uint16_t h = psx_mod_read_half(slot + EN_HITS_ON_AYA);
            if (h > s_st.hits[s]) s_st.hits[s] = h;
            if (psx_mod_read_word(slot + EN_STATUS_FLAGS2) & 0x40000u) s_boss_battle = 1;
        }
        return;
    }

    if (s_wayne_waiting && psx_mod_read_half(PE_WAYNE_COUNTER) < PE_WAYNE_CRAFT_COST) s_wayne_waiting = 0;
    if (s_list_redirected && s_frames - s_last_count_frame > 2u) pe_restore_active_list();
    /* A hand-in stays open until every removal is fed: Wayne's dialogue can
     * take seconds between the count and the removals, and his script adds to
     * his counter even when a removal finds nothing. Only a full second back in
     * free movement ends one early (the menu was left). */
    s_free_frames = pe_free_field() ? s_free_frames + 1u : 0u;
    if (s_handin_left && s_free_frames > 60u) s_handin_left = 0;
    static int s_wayne_check;                /* judge the hand-in once he is done talking */
    if (s_handin_active || s_wayne_check) {  /* a craft takes 300 off his count */
        const uint16_t w = psx_mod_read_half(PE_WAYNE_COUNTER);
        if (w < s_wayne_prev) s_wayne_dropped = 1;
        s_wayne_prev = w;
    }
    if (s_handin_active && !s_handin_left &&
        s_frames - s_last_count_frame > PE_HANDIN_GUARD_FRAMES) {
        s_handin_active = 0;
        s_wayne_check = 1;
    }
    if (s_wayne_check && !s_handin_active && s_free_frames > 60u) {
        s_wayne_check = 0;
        /* At 300+ with no gun made: before Day 4 his menu only stores Junk. */
        /* At 300+ with no gun made: before Day 4; his row says "Day 4". */
        if (!s_wayne_dropped && s_wayne_prev >= PE_WAYNE_CRAFT_COST) s_wayne_waiting = 1;
    }
    if (!s_handin_active && pe_free_field()) pe_sweep();
}

/* ---- save states ------------------------------------------------------------ */
static void pe_loot_state_loaded(int result, uint32_t saved_version,
                                 const void *saved, uint32_t saved_size) {
    if (result == PSX_MOD_STATE_RESTORED) {
        if (s_st.preset > 2) s_st.preset = 1;
        s_enabled = !s_st.disabled;
        return;
    }
    memset(&s_st, 0, sizeof s_st);
    s_st.preset = 1;
    s_st.disabled = (uint8_t)!s_enabled;
    /* Phase 1 states (v1) began with the Junk counter: keep it. v2 is v3
     * without the Lucky Charm and the preset. */
    if (result == PSX_MOD_STATE_MISMATCH && saved_version == 1u && saved && saved_size >= 4u)
        memcpy(&s_st.junk, saved, sizeof s_st.junk);
    if (result == PSX_MOD_STATE_MISMATCH && saved_version == 3u && saved &&
        saved_size == PE_LOOT_STATE_V3_SIZE)
        memcpy(&s_st, saved, PE_LOOT_STATE_V3_SIZE);
    if (result == PSX_MOD_STATE_MISMATCH && saved_version == 2u && saved &&
        saved_size == PE_LOOT_STATE_V2_SIZE)
        memcpy(&s_st, saved, PE_LOOT_STATE_V2_SIZE);
}

static void pe_found_state_loaded(int result, uint32_t saved_version,
                                  const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result == PSX_MOD_STATE_RESTORED) return;
    memset(&s_found, 0, sizeof s_found);
    s_found.econ_shown = -1;
}

static void pe_loot_restored(void) {
    /* A snapshot from the two frames of a hand-in count points Aya's list at
     * our scratch list, in mod memory that snapshots don't hold: back to the bag. */
    if (psx_mod_read_word(PE_ACTIVE_LIST_PTR) >= 0x1F000000u) {
        uint32_t limit = psx_mod_read_byte(0x800C0E0Cu) + psx_mod_read_word(0x8009D018u);
        if (limit > PE_AYA_SLOT_COUNT) limit = PE_AYA_SLOT_COUNT;
        psx_mod_write_word(PE_ACTIVE_LIST_PTR, PE_AYA_SLOTS_ADDR);
        psx_mod_write_word(PE_ACTIVE_LIST_LIMIT, limit);
    }
    s_in_battle = pe_game_loaded() && pe_in_battle();
    s_handin_active = 0;
    s_handin_left = 0;
    s_list_redirected = 0;
    s_color_saved = 0;                       /* the screen state rides in "pe.loot.found" */
}

/* ---- pe_loot.h: Wayne's row (pe_wayne_menu.c) ------------------------------- */
int pe_loot_wayne_waiting(void) {
    return s_enabled && s_wayne_waiting && psx_mod_read_half(PE_WAYNE_COUNTER) >= PE_WAYNE_CRAFT_COST;
}

int pe_loot_handin_blocked(void) {
    if (!s_enabled || !pe_game_loaded() || !s_st.junk) return 0;
    uint32_t limit = psx_mod_read_byte(0x800C0E0Cu) + psx_mod_read_word(0x8009D018u);
    if (limit > PE_AYA_SLOT_COUNT) limit = PE_AYA_SLOT_COUNT;
    return pe_free_bag_slot(limit) < 0;
}

/* ---- menu ----------------------------------------------------------------- */
static int pe_get_junk(void) { return (int)s_st.junk; }
static int pe_set_junk_readonly(int value) { (void)value; return 0; }
static int pe_get_rare_within(void) {
    const int left = (int)pe_rare_guarantee() - (int)s_st.rare_dry;
    const int step = (int)pe_pity_step();
    return left < 1 ? 1 : (left + step - 1) / step;
}
/* With the economy off: the counter's Junk back into the bag, then the trunk,
 * as far as there is room (the game's own Junk again, for Wayne). */
static void pe_take_out_junk(void) {
    char msg[80];
    if (!pe_game_loaded()) { psx_mod_osd_message("Load a game first"); return; }
    if (s_enabled) { psx_mod_osd_message("Turn Junk economy off first (it would go back)"); return; }
    uint32_t moved = 0;
    uint32_t limit = psx_mod_read_byte(0x800C0E0Cu) + psx_mod_read_word(0x8009D018u);
    if (limit > PE_AYA_SLOT_COUNT) limit = PE_AYA_SLOT_COUNT;
    for (uint32_t i = 0; i < limit && s_st.junk; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a)) continue;
        psx_mod_write_half(a, PE_ITEM_JUNK); --s_st.junk; ++moved;
    }
    for (uint32_t i = 0; i < PE_STORAGE_COUNT && s_st.junk; ++i) {
        const uint32_t a = PE_STORAGE_ADDR + i * 2u;
        if (psx_mod_read_half(a)) continue;
        psx_mod_write_half(a, PE_ITEM_JUNK); --s_st.junk; ++moved;
    }
    snprintf(msg, sizeof msg, "%u Junk to the bag and trunk, %u left in the counter",
             (unsigned)moved, (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}
static int pe_get_preset(void) { return s_st.preset < 3 ? s_st.preset : 1; }
static int pe_set_preset(int v) {
    if (v < 0 || v > 2) return 0;
    s_st.preset = (uint8_t)v;
    return 1;
}
static void pe_give_100_junk(void) {
    char msg[48];
    pe_add_junk(100);
    snprintf(msg, sizeof msg, "+100 Junk (total %u)", (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}
static void pe_give_1000_junk(void) {
    char msg[48];
    pe_add_junk(1000);
    snprintf(msg, sizeof msg, "+1000 Junk (total %u)", (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}
static int pe_set_junk_debug(int value) {
    if (value < 0 || value > 99999) return 0;
    s_st.junk = (uint32_t)value;
    return 1;
}

#define PE_WORKSHOP_PAGE "Workshop"
#define PE_TUNING_PAGE   "Debug/Loot tuning"

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_loot_menu_register(void) {
    (void)pe_menu_register_toggle(PE_WORKSHOP_PAGE, "Junk economy",
        "Junk counter, Mod Catalog and shop.", &s_enabled, pe_loot_tick);
    (void)pe_menu_register_live_int(PE_WORKSHOP_PAGE, "Junk",
        "Earned in battles and chests.", 0, 99999, 1,
        pe_get_junk, pe_set_junk_readonly);
    (void)pe_menu_register_action(PE_WORKSHOP_PAGE, "Take out Junk",
        "Economy off: counter Junk to bag/trunk.", pe_take_out_junk);
    (void)pe_menu_register_live_int(PE_WORKSHOP_PAGE, "Rare drop within",
        "Battles until a rare drop is certain.", 1, 999, 1,
        pe_get_rare_within, pe_set_junk_readonly);
    (void)pe_menu_register_choice(PE_WORKSHOP_PAGE, "Wayne hand-in",
        "Junk that Discard junk gives Wayne.", &s_handin_mode, s_handin_names, 2);
    (void)pe_menu_register_live_choice(PE_WORKSHOP_PAGE, "Difficulty",
        "Junk, rare luck and prices. Any time.", s_preset_names, 3,
        pe_get_preset, pe_set_preset);

    (void)pe_menu_register_int(PE_TUNING_PAGE, "Junk floor",
        "Junk for every won battle.", &s_tune_floor, 0, 10, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "EXP per Junk",
        "Battle EXP for each extra Junk.", &s_tune_exp_per_junk, 1, 100, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Battle Junk cap",
        "Most Junk from one battle.", &s_tune_battle_cap, 1, 200, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Boss Junk cap",
        "Most Junk from a boss fight.", &s_tune_boss_cap, 1, 200, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "No-hit bonus %",
        "Extra Junk for a no-hit battle.", &s_tune_nohit_bonus, 0, 100, 5);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Rare base %",
        "Rare drop chance after a drop.", &s_tune_rare_base, 0, 100, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Rare step %",
        "Added for each battle without one.", &s_tune_rare_step, 0, 50, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Rare guarantee",
        "Battles until a rare drop is certain.", &s_tune_rare_guarantee, 1, 200, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Economy mod guarantee",
        "Rares until an economy mod is certain.", &s_tune_econ_guarantee, 1, 50, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Mod Permit guarantee",
        "Rares until a Mod Permit is certain.", &s_tune_permit_guarantee, 1, 50, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Empty steal Junk",
        "Junk for stealing from an empty enemy.", &s_tune_empty_steal, 0, 20, 1);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Give 100 Junk",
        "Debug: add 100 Junk.", pe_give_100_junk);
    (void)pe_menu_register_action(PE_TUNING_PAGE, "Give 1000 Junk",
        "Debug: add 1000 Junk.", pe_give_1000_junk);
    (void)pe_menu_register_live_int(PE_TUNING_PAGE, "Set Junk",
        "Debug: change the Junk counter.", 0, 99999, 10,
        pe_get_junk, pe_set_junk_debug);
}

/* Registered after the loot logger, so the logger's Inv_AddItem hook still
 * sees the original item id before this one rewrites Junk to 0. */
void pe_loot_hooks_register(void) {
    (void)psx_mod_register_function_entry_plugin("pe.loot.battle-end",
        PE_FN_SET_TOTAL_EXP, pe_on_set_total_exp);
    (void)psx_mod_register_function_entry_plugin("pe.loot.rare-color",
        PE_FN_DRAW_PRINT_RAW_TEXT, pe_on_print_raw_text);
    (void)psx_mod_register_function_entry_plugin("pe.loot.add-item",
        PE_FN_INV_ADD_ITEM, pe_on_inv_add_item);
    (void)psx_mod_register_function_entry_plugin("pe.loot.count-items",
        PE_FN_TASK_COUNT_ITEMS, pe_on_task_count_items);
    (void)psx_mod_register_function_entry_plugin("pe.loot.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_function_entry_plugin("pe.loot.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_state("pe.loot", PE_LOOT_STATE_VERSION,
        &s_st, sizeof s_st, pe_loot_state_loaded);
    (void)psx_mod_register_state("pe.loot.found", PE_FOUND_STATE_VERSION,
        &s_found, sizeof s_found, pe_found_state_loaded);
    (void)psx_mod_register_restore_callback(pe_loot_restored);
}
