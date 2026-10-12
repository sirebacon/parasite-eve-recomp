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
 *     Junk is forfeited (decision 41). One toast per battle.
 *   - Inv_AddItem(0x14) (chests, story gifts, steals): the id is rewritten to
 *     0 before the body runs, which adds nothing and reports success
 *     (Item_LookupBaseData(-1) is out of range), and the Junk is counted --
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

#define PE_FN_SET_TOTAL_EXP     0x8004B70Cu  /* game.toml mod_function_entry_funcs */
#define PE_FN_INV_DISPATCH      0x8005D2B4u
#define PE_FN_INV_ADD_ITEM      0x80053D2Cu
#define PE_FN_SAVE_SERIALIZE    0x8003F800u
#define PE_FN_SAVE_DESERIALIZE  0x8003FBD8u
#define PE_FN_TASK_COUNT_ITEMS  0x800194F8u
#define PE_FN_RESOLVE_HIT_START 0x800236E8u  /* Battle_ResolveHitOnTimer (steals) */
#define PE_FN_RESOLVE_HIT_END   0x80023E14u

#define PE_INV_CMD_REMOVE_ITEM    1112
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_ITEM_JUNK          0x14u
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

#define PE_REWARD_SLOTS       10u

/* Save tail: the 8 KB block's CRC sits at 0x1C8C; 0x1C90..0x1FFF is free
 * (zero in every save checked) and still inside the CRC'd range. */
#define PE_CARD_BUFFER        0x8009EED0u  /* g_MemCardFileBuffer */
#define PE_TAIL_OFFSET        0x1C90u
#define PE_TAIL_MAGIC         0x544C4550u  /* "PELT" */
#define PE_TAIL_VERSION       1u

#define PE_HANDIN_GUARD_FRAMES 120u        /* no sweep this soon after a count */
#define PE_HANDIN_MAX          999u        /* Junk per hand-in */

/* ---- state ---------------------------------------------------------------- */
#define PE_LOOT_STATE_VERSION 1u
static struct {
    uint32_t junk;
    uint16_t steal_bank;          /* Junk stolen this battle, paid on a win */
    uint16_t hits[PE_ENEMY_SLOTS];
} s_st;

static int s_enabled = 1;          /* "Junk economy", on by default (28) */
static int s_handin_mode;          /* 0 = up to Wayne's next gun, 1 = all */
static int s_tune_floor = 1;       /* decision 16 */
static int s_tune_exp_per_junk = 8;/* decision 17 (placeholder until phase 7) */
static int s_tune_battle_cap = 30;
static int s_tune_nohit_bonus = 25;/* decision 32, percent */

static int s_in_battle;
static uint32_t s_frames, s_last_count_frame;
static int s_handin_active;
static uint32_t s_handin_left;     /* removals still to feed this hand-in */
static uint32_t s_scratch_list;   /* guest list of Junk the count reads */
static uint32_t s_saved_limit;
static int s_list_redirected;
static uint32_t s_free_frames;     /* consecutive frames of free movement */

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

/* ---- battle end ------------------------------------------------------------ */
static void pe_on_set_total_exp(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled) return;
    const uint32_t exp = cpu->gpr[4];
    const uint32_t list = cpu->gpr[6];

    /* Vanilla Junk leaves the reward list for the counter (keeps it contiguous). */
    uint32_t vanilla = 0;
    if (pe_ram_pointer(list)) {
        uint32_t keep[PE_REWARD_SLOTS];
        uint32_t kept = 0;
        for (uint32_t i = 0; i < PE_REWARD_SLOTS; ++i) {
            const uint32_t entry = psx_mod_read_word(list + i * 4u);
            if ((entry & 0xFFFFu) == 0) break;
            if ((entry & 0xFFFFu) == PE_ITEM_JUNK) ++vanilla;
            else keep[kept++] = entry;
        }
        for (uint32_t i = 0; i < PE_REWARD_SLOTS; ++i)
            psx_mod_write_word(list + i * 4u, i < kept ? keep[i] : 0xFFFF0000u);
    }

    /* Floor + EXP-scaled, capped, with the no-hit bonus (16, 17, 32). */
    const uint32_t per = s_tune_exp_per_junk > 0 ? (uint32_t)s_tune_exp_per_junk : 1u;
    uint32_t earned = (uint32_t)s_tune_floor + exp / per;
    if (earned > (uint32_t)s_tune_battle_cap) earned = (uint32_t)s_tune_battle_cap;
    uint32_t hits = 0;
    for (int s = 0; s < PE_ENEMY_SLOTS; ++s) hits += s_st.hits[s];
    if (hits < 4) earned += earned * (uint32_t)s_tune_nohit_bonus * (4u - hits) / 400u;

    const uint32_t total = earned + vanilla + s_st.steal_bank;
    s_st.steal_bank = 0;
    pe_add_junk(total);

    char msg[64];
    snprintf(msg, sizeof msg, "+%u Junk (total %u)", (unsigned)total, (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}

/* ---- pickups and steals ---------------------------------------------------- */
static void pe_on_inv_add_item(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_enabled || !pe_game_loaded() || cpu->gpr[4] != PE_ITEM_JUNK) return;
    const uint32_t ra = cpu->gpr[31];
    cpu->gpr[4] = 0;   /* Inv_AddItem(0): adds nothing, returns success */
    if (ra >= PE_FN_RESOLVE_HIT_START && ra < PE_FN_RESOLVE_HIT_END) {
        if (s_st.steal_bank < 0xFFFFu) ++s_st.steal_bank;   /* paid on a win (41) */
    } else {
        pe_add_junk(1);
        char msg[48];
        snprintf(msg, sizeof msg, "+1 Junk (total %u)", (unsigned)s_st.junk);
        psx_mod_osd_message(msg);
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
    if (pe_free_bag_slot(limit) < 0) {
        psx_mod_osd_message("Free one bag slot to hand Junk to Wayne");
        return;
    }
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
    if (command == PE_INV_CMD_START_NEW_GAME) { memset(&s_st, 0, sizeof s_st); return; }
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
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, s_st.junk);
    psx_mod_write_word(at + 12u, s_st.junk ^ 0xA5A5A5A5u);
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    memset(&s_st, 0, sizeof s_st);   /* a vanilla save: start at 0, the sweep converts */
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC || psx_mod_read_word(at + 4u) != PE_TAIL_VERSION)
        return;
    const uint32_t junk = psx_mod_read_word(at + 8u);
    if ((junk ^ 0xA5A5A5A5u) == psx_mod_read_word(at + 12u) && junk <= 99999u) s_st.junk = junk;
}

/* ---- per frame -------------------------------------------------------------- */
static void pe_sweep(void) {
    uint32_t moved = 0;
    for (uint32_t i = 0; i < PE_AYA_SLOT_COUNT; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a) == PE_ITEM_JUNK) { psx_mod_write_half(a, 0); ++moved; }
    }
    for (uint32_t i = 0; i < PE_STORAGE_COUNT; ++i) {
        const uint32_t a = PE_STORAGE_ADDR + i * 2u;
        if (psx_mod_read_half(a) == PE_ITEM_JUNK) { psx_mod_write_half(a, 0); ++moved; }
    }
    if (!moved) return;
    pe_add_junk(moved);
    char msg[64];
    snprintf(msg, sizeof msg, "+%u Junk moved to the counter (total %u)",
             (unsigned)moved, (unsigned)s_st.junk);
    psx_mod_osd_message(msg);
}

static void pe_loot_tick(void) {
    ++s_frames;
    if (!s_enabled || !pe_game_loaded()) { s_in_battle = 0; return; }

    const int battle = pe_in_battle();
    if (battle && !s_in_battle) {            /* a battle starts */
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
        }
        return;
    }

    if (s_list_redirected && s_frames - s_last_count_frame > 2u) pe_restore_active_list();
    /* A hand-in stays open until every removal is fed: Wayne's dialogue can
     * take seconds between the count and the removals, and his script adds to
     * his counter even when a removal finds nothing. Only a full second back in
     * free movement ends one early (the menu was left). */
    s_free_frames = pe_free_field() ? s_free_frames + 1u : 0u;
    if (s_handin_left && s_free_frames > 60u) s_handin_left = 0;
    if (s_handin_active && !s_handin_left &&
        s_frames - s_last_count_frame > PE_HANDIN_GUARD_FRAMES)
        s_handin_active = 0;
    if (!s_handin_active && pe_free_field()) pe_sweep();
}

/* ---- save states ------------------------------------------------------------ */
static void pe_loot_state_loaded(int result, uint32_t saved_version,
                                 const void *saved, uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_st, 0, sizeof s_st);
}

static void pe_loot_restored(void) {
    s_in_battle = pe_game_loaded() && pe_in_battle();
    s_handin_active = 0;
    s_handin_left = 0;
    s_list_redirected = 0;
}

/* ---- menu ----------------------------------------------------------------- */
static int pe_get_junk(void) { return (int)s_st.junk; }
static int pe_set_junk_readonly(int value) { (void)value; return 0; }
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
    (void)pe_menu_register_choice(PE_WORKSHOP_PAGE, "Wayne hand-in",
        "Junk that Discard junk gives Wayne.", &s_handin_mode, s_handin_names, 2);

    (void)pe_menu_register_int(PE_TUNING_PAGE, "Junk floor",
        "Junk for every won battle.", &s_tune_floor, 0, 10, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "EXP per Junk",
        "Battle EXP for each extra Junk.", &s_tune_exp_per_junk, 1, 100, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "Battle Junk cap",
        "Most Junk from one battle.", &s_tune_battle_cap, 1, 200, 1);
    (void)pe_menu_register_int(PE_TUNING_PAGE, "No-hit bonus %",
        "Extra Junk for a no-hit battle.", &s_tune_nohit_bonus, 0, 100, 5);
    (void)pe_menu_register_live_int(PE_TUNING_PAGE, "Set Junk",
        "Debug: change the Junk counter.", 0, 99999, 10,
        pe_get_junk, pe_set_junk_debug);
}

/* Registered after the loot logger, so the logger's Inv_AddItem hook still
 * sees the original item id before this one rewrites Junk to 0. */
void pe_loot_hooks_register(void) {
    (void)psx_mod_register_function_entry_plugin("pe.loot.battle-end",
        PE_FN_SET_TOTAL_EXP, pe_on_set_total_exp);
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
    (void)psx_mod_register_restore_callback(pe_loot_restored);
}
