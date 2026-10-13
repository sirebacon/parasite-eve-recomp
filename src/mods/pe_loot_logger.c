/*
 * Loot logger (research tool): while switched on, records what the vanilla
 * game hands out, to pe_loot_log.csv next to the exe, so the loot overhaul
 * can be balanced against a real baseline (bridge findings
 * pe-loot-overhaul-design-2026-10-11.md, section 6.1).
 *
 * Rows:
 *   enemy   one per enemy when its battle slot is freed (killed or gone):
 *           room, enemy identity (+0x04), start HP, defense, EXP (+0x0E, what
 *           Battle_StepEntityAnimState adds), BP base/factor/hits taken
 *           (+0x98/+0x9A/+0x9C), drop item (+0x9E), steal item (+0x9F),
 *           whether it died.
 *   battle  one per battle when it ends: room, length, EXP and BP totals
 *           (D_8009D304 / D_8009D21C), the reward list (0x800A7FF0).
 *   pickup  a change in Aya's count of Junk / Tools / kits / trading cards /
 *           Mod Permit outside battle, with the room.
 *   session one per launch, the local time in the rewards column, so runs on
 *           different days can be told apart (battle numbers restart).
 *   steal   a steal attempt that got past the enemy's coin flip: Inv_AddItem
 *           called from Battle_ResolveHitOnTimer with the enemy's steal item
 *           (item 0x00 = the enemy had nothing left to steal).
 *   add     any other Inv_AddItem call (chests, story gifts, Wayne's crafts,
 *           taking items from the battle results screen), item = the id.
 *   invcmd  a script inventory command (Inv_DispatchCommand: 1102 count item,
 *           1112 remove one, ...), item = its value, delta = the command,
 *           rewards = "other=N". Used to confirm Wayne's "Discard junk" loop.
 *
 * Rows for empty enemy slots (identity 0) and the 1-2 frame "battle" seen
 * right after loading a save are skipped.
 *
 * Enemies are set up by their battle scripts (Entity_WriteFieldByCmd 0x5A..0x5D
 * for BP/drop/steal), so each enemy's fields are re-read every frame while its
 * slot is active and the last values are logged.
 */
#include <stdio.h>
#include <time.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"

#define PE_FN_INV_ADD_ITEM        0x80053D2Cu  /* game.toml mod_function_entry_funcs */
#define PE_FN_INV_DISPATCH        0x8005D2B4u
#define PE_FN_RESOLVE_HIT_START   0x800236E8u  /* Battle_ResolveHitOnTimer (steals) */
#define PE_FN_RESOLVE_HIT_END     0x80023E14u  /* next function, Battle_ApplyDamage */
#define PE_MIN_BATTLE_FRAMES      3u           /* shorter = the blip after a load */

#define PE_GAME_BLOCK_ADDR    0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE  0x2u
#define PE_ROOM_ADDR          0x8009D280u  /* room word (the debug-warp cheat writes it) */
#define PE_ENEMY_SLOT_BASE    0x800A5D58u  /* 7 x { u8 active; pad; EnemyCombatant } */
#define PE_ENEMY_SLOT_STRIDE  0xDCu
#define PE_ENEMY_SLOTS        7
#define PE_BATTLE_EXP_ADDR    0x8009D304u
#define PE_BATTLE_BP_ADDR     0x8009D21Cu
#define PE_REWARD_LIST_ADDR   0x800A7FF0u  /* 10 x { u16 id; s16 amount } */
#define PE_AYA_SLOTS_ADDR     0x800C0E48u
#define PE_AYA_SLOT_COUNT     50
#define PE_MAX_HP_ADDR        0x800C0E06u

#define EN_IDENTITY  0x04u
#define EN_HP_MIRROR 0x0Eu
#define EN_HP        0x10u
#define EN_DEFENSE   0x8Cu
#define EN_BP_BASE   0x98u
#define EN_BP_FACTOR 0x9Au
#define EN_BP_HITS   0x9Cu
#define EN_DROP      0x9Eu
#define EN_STEAL     0x9Fu
#define EN_ENTITY_ID 0x07u

static int s_logging = 1;          /* on by default: playtest data for balancing (2026-10-13) */
static FILE *s_log;
static unsigned s_battle_no, s_battle_frames;
static int s_in_battle;

typedef struct {
    int tracking;
    uint8_t entity_id;
    int start_hp, last_hp;
    unsigned identity, exp, defense, bp_base, bp_factor, bp_hits, drop, steal;
} PeEnemyTrack;
static PeEnemyTrack s_enemy[PE_ENEMY_SLOTS];

/* Item ids whose count is watched for field pickups. */
static const uint16_t s_watch_ids[] = {
    0x0B, 0x0C, 0x14, 0x15, 0x16, 0x17, 0x35, 0x36, 0x37,             /* junk, tools, permit */
    0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, /* trading cards */
    0xE8, 0xE9, 0xEA, 0xEB,
};
#define PE_WATCH_COUNT (sizeof s_watch_ids / sizeof s_watch_ids[0])
static int s_counts[PE_WATCH_COUNT];
static int s_counts_valid;

static unsigned pe_room(void) { return psx_mod_read_half(PE_ROOM_ADDR); }

static int s_session_written;

/* kind,battle,room + 14 empty columns + rewards,item,delta. */
static void pe_log_row(const char *kind, const char *battle, const char *rewards,
                       const char *item, const char *delta) {
    fprintf(s_log, "%s,%s,0x%04X,,,,,,,,,,,,,,,%s,%s,%s\n",
            kind, battle, pe_room(), rewards, item, delta);
    fflush(s_log);
}

static int pe_log_open(void) {
    if (!s_log) {
        s_log = fopen("pe_loot_log.csv", "a");
        if (!s_log) return 0;
        fseek(s_log, 0, SEEK_END);
        if (ftell(s_log) == 0)
            fprintf(s_log, "kind,battle,room,slot,identity,start_hp,defense,exp,bp_base,bp_factor,"
                           "bp_hits,drop,steal,died,frames,exp_total,bp_total,rewards,item,delta\n");
    }
    if (!s_session_written) {
        char stamp[32];
        const time_t now = time(NULL);
        const struct tm *local = localtime(&now);
        if (!local || !strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", local))
            snprintf(stamp, sizeof stamp, "unknown");
        s_session_written = 1;
        pe_log_row("session", "", stamp, "", "");
    }
    return 1;
}

static void pe_log_enemy(int slot, int died) {
    const PeEnemyTrack *e = &s_enemy[slot];
    if (e->identity == 0) return;      /* an empty slot, not a real enemy */
    if (!pe_log_open()) return;
    fprintf(s_log, "enemy,%u,0x%04X,%d,0x%04X,%d,%u,%u,%u,%u,%u,0x%02X,0x%02X,%d,,,,,,\n",
            s_battle_no, pe_room(), slot, e->identity, e->start_hp, e->defense, e->exp,
            e->bp_base, e->bp_factor, e->bp_hits, e->drop, e->steal, died);
    fflush(s_log);
}

static void pe_log_battle_end(void) {
    char rewards[160];
    unsigned n = 0;
    if (s_battle_frames < PE_MIN_BATTLE_FRAMES) return;   /* the blip after a load */
    rewards[0] = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        const unsigned id = psx_mod_read_half(PE_REWARD_LIST_ADDR + i * 4u);
        if (!id) continue;
        const int amount = (int16_t)psx_mod_read_half(PE_REWARD_LIST_ADDR + i * 4u + 2u);
        n += (unsigned)snprintf(rewards + n, sizeof rewards - n, "%s0x%02X", n ? " " : "", id);
        if (amount >= 0 && n < sizeof rewards)
            n += (unsigned)snprintf(rewards + n, sizeof rewards - n, "x%d", amount);
        if (n >= sizeof rewards) break;
    }
    if (!pe_log_open()) return;
    fprintf(s_log, "battle,%u,0x%04X,,,,,,,,,,,,%u,%u,%u,%s,,\n", s_battle_no, pe_room(),
            s_battle_frames, psx_mod_read_word(PE_BATTLE_EXP_ADDR),
            psx_mod_read_word(PE_BATTLE_BP_ADDR), rewards);
    fflush(s_log);
}

static int pe_count_item(uint16_t id) {
    int count = 0;
    for (int i = 0; i < PE_AYA_SLOT_COUNT; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)i * 2u) == id) ++count;
    return count;
}

static void pe_track_enemies(void) {
    for (int s = 0; s < PE_ENEMY_SLOTS; ++s) {
        const uint32_t slot = PE_ENEMY_SLOT_BASE + (uint32_t)s * PE_ENEMY_SLOT_STRIDE;
        const uint32_t core = slot + 4u;
        PeEnemyTrack *e = &s_enemy[s];
        const int active = psx_mod_read_byte(slot) != 0;
        const uint8_t entity_id = psx_mod_read_byte(core + EN_ENTITY_ID);
        if (e->tracking && (!active || entity_id != e->entity_id)) {
            pe_log_enemy(s, e->last_hp <= 0);
            e->tracking = 0;
        }
        if (!active) continue;
        const int hp = (int32_t)psx_mod_read_word(core + EN_HP);
        if (!e->tracking) {
            e->tracking = 1;
            e->entity_id = entity_id;
            e->start_hp = hp;
        }
        if (hp > e->start_hp) e->start_hp = hp;   /* scripts may set HP after spawn */
        e->last_hp = hp;
        e->identity = psx_mod_read_half(core + EN_IDENTITY);
        e->exp = psx_mod_read_half(core + EN_HP_MIRROR);
        e->defense = psx_mod_read_half(core + EN_DEFENSE);
        e->bp_base = psx_mod_read_half(core + EN_BP_BASE);
        e->bp_factor = psx_mod_read_half(core + EN_BP_FACTOR);
        e->bp_hits = psx_mod_read_half(core + EN_BP_HITS);
        e->drop = psx_mod_read_byte(core + EN_DROP);
        e->steal = psx_mod_read_byte(core + EN_STEAL);
    }
}

static void pe_loot_logger_tick(void) {
    if (!s_logging) {
        if (s_log) { fclose(s_log); s_log = 0; }
        s_counts_valid = 0;
        return;
    }
    if (!psx_mod_game_started() || psx_mod_read_half(PE_MAX_HP_ADDR) == 0) return;

    const int battle = (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE) != 0;
    if (battle) {
        if (!s_in_battle) { s_in_battle = 1; ++s_battle_no; s_battle_frames = 0; }
        ++s_battle_frames;
        pe_track_enemies();
        s_counts_valid = 0;          /* battle rewards aren't field pickups */
        return;
    }
    if (s_in_battle) {
        for (int s = 0; s < PE_ENEMY_SLOTS; ++s)
            if (s_enemy[s].tracking) { pe_log_enemy(s, s_enemy[s].last_hp <= 0); s_enemy[s].tracking = 0; }
        pe_log_battle_end();
        s_in_battle = 0;
    }

    /* Field pickups: count changes of watched items. */
    for (unsigned i = 0; i < PE_WATCH_COUNT; ++i) {
        const int now = pe_count_item(s_watch_ids[i]);
        if (s_counts_valid && now != s_counts[i] && pe_log_open()) {
            fprintf(s_log, "pickup,,0x%04X,,,,,,,,,,,,,,,,0x%02X,%d\n",
                    pe_room(), s_watch_ids[i], now - s_counts[i]);
            fflush(s_log);
        }
        s_counts[i] = now;
    }
    s_counts_valid = 1;
}

/* Inv_AddItem(id) entry: a steal when called from the hit resolver, else a
 * chest / gift / craft / results-screen pickup. */
static void pe_on_inv_add_item(struct CPUState *cpu, uint32_t address) {
    char item[8], battle[12];
    (void)address;
    if (!s_logging || !psx_mod_game_started() || !pe_log_open()) return;
    const uint32_t ra = cpu->gpr[31];
    const int steal = ra >= PE_FN_RESOLVE_HIT_START && ra < PE_FN_RESOLVE_HIT_END;
    snprintf(item, sizeof item, "0x%02X", (unsigned)(cpu->gpr[4] & 0xFFFFu));
    if (s_in_battle) snprintf(battle, sizeof battle, "%u", s_battle_no);
    else battle[0] = 0;
    pe_log_row(steal ? "steal" : "add", battle, "", item, "");
}

/* Inv_DispatchCommand(command, value, other) entry: script inventory actions. */
static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    char other[24], item[12], command[12];
    (void)address;
    if (!s_logging || !psx_mod_game_started() || !pe_log_open()) return;
    snprintf(other, sizeof other, "other=%d", (int)cpu->gpr[6]);
    snprintf(item, sizeof item, "0x%X", (unsigned)cpu->gpr[5]);
    snprintf(command, sizeof command, "%d", (int)cpu->gpr[4]);
    pe_log_row("invcmd", "", other, item, command);
}

/* Called from pe_debug_menu.c's page (appended after its rows). */
void pe_loot_logger_menu_register(void) {
    (void)pe_menu_register_toggle("Debug", "Loot logger",
        "Log drops to pe_loot_log.csv.", &s_logging, 0);
    (void)pe_menu_register_tick(pe_loot_logger_tick);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.add-item",
        PE_FN_INV_ADD_ITEM, pe_on_inv_add_item);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
}
