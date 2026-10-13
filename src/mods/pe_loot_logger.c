/*
 * Play log (research tool, on by default): records play to pe_play_log.csv
 * next to the exe, for balancing the loot overhaul against real runs (bridge
 * findings pe-loot-overhaul-design-2026-10-11.md section 6.1, and the
 * balance estimate's second pass) and for chasing bugs.
 *
 * Columns: kind, battle, room, play_s, level, exp, hp, max_hp, junk, wayne,
 * detail. Every row carries the context: the battle number while in battle,
 * the room word, play time in seconds (GAME_TIME_COUNTER(2), frames / 60),
 * Aya's level (1-based), total EXP, HP / max HP, the Junk counter and Wayne's
 * count. `detail` is key=value pairs.
 *
 * Rows written here:
 *   session   one per launch: the local date and time.
 *   enemy     one per enemy when its battle slot is freed: slot, identity
 *             (+0x04), start HP, defense, EXP (+0x0E), BP base / factor / hits
 *             (+0x98/+0x9A/+0x9C), drop (+0x9E), steal (+0x9F), boss (status
 *             flags 2 & 0x40000: can't escape), died.
 *   battle    one per battle: frames and seconds, EXP and BP totals
 *             (D_8009D304 / D_8009D21C), outcome (win / escape: battle phase 4
 *             was seen / loss: HP 0), HP before -> after, the equipped gun, boss,
 *             the reward list (0x800A7FF0).
 *   pickup    a change in Aya's count of Junk / Tools / kits / trading cards /
 *             Mod Permit outside battle.
 *   steal     a steal past the enemy's coin flip (Inv_AddItem from
 *             Battle_ResolveHitOnTimer; item 0 = nothing left to steal).
 *   add       any other Inv_AddItem (chests, story gifts, Wayne's crafts).
 *   invcmd    a script inventory command (Inv_DispatchCommand).
 *   wayne     Wayne's count moving (a hand-in, a craft's -300), one row once it
 *             settles: from -> to.
 *   levelup   Aya's level going up.
 *   newgame / cardload / stateload   a new game, a memory card load, a save
 *             state load (written on the next frame, so the context is the
 *             loaded game's).
 *   save      a memory card save: bag use, the equipped gun and armor with
 *             their + bonuses and mods, bonus points.
 * Other modules add their own rows through pe_log() (pe_log.h): the
 * overhaul's battle loot, Junk taken or left, rare drops, purchases, installs,
 * dismantles, rebuilds, Combine, Lucky Charm, Wayne's refund, and every F1
 * setting changed or action run (pe_mod_menu.c).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"

#define PE_FN_INV_ADD_ITEM        0x80053D2Cu  /* game.toml mod_function_entry_funcs */
#define PE_FN_INV_DISPATCH        0x8005D2B4u
#define PE_FN_SAVE_SERIALIZE      0x8003F800u
#define PE_FN_SAVE_DESERIALIZE    0x8003FBD8u
#define PE_FN_RESOLVE_HIT_START   0x800236E8u  /* Battle_ResolveHitOnTimer (steals) */
#define PE_FN_RESOLVE_HIT_END     0x80023E14u  /* next function, Battle_ApplyDamage */
#define PE_INV_CMD_START_NEW_GAME 1117
#define PE_MIN_BATTLE_FRAMES      3u           /* shorter = the blip after a load */
#define PE_WAYNE_SETTLE_FRAMES    90u

#define PE_GAME_BLOCK_ADDR    0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE  0x2u
#define PE_BATTLE_PHASE_ADDR  0x8009D28Cu  /* 4 = escape */
#define PE_ROOM_ADDR          0x8009D280u  /* room word (the debug-warp cheat writes it) */
#define PE_PLAY_FRAMES_ADDR   0x800A76BCu  /* GAME_TIME_COUNTER(2): play time in frames */
#define PE_ENEMY_SLOT_BASE    0x800A5D58u  /* 7 x { u8 active; pad; EnemyCombatant } */
#define PE_ENEMY_SLOT_STRIDE  0xDCu
#define PE_ENEMY_SLOTS        7
#define PE_BATTLE_EXP_ADDR    0x8009D304u
#define PE_BATTLE_BP_ADDR     0x8009D21Cu
#define PE_REWARD_LIST_ADDR   0x800A7FF0u  /* 10 x { u16 id; s16 amount } */
#define PE_AYA_SLOTS_ADDR     0x800C0E48u
#define PE_AYA_SLOT_COUNT     50
#define PE_TOTAL_EXP_ADDR     0x800C0E00u
#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_CUR_HP_ADDR        0x800C0E08u
#define PE_LEVEL_ADDR         0x800C0E0Au
#define PE_SLOT_COUNT_ADDR    0x800C0E0Cu
#define PE_BONUS_POINTS_ADDR  0x800C0E10u
#define PE_BONUS_SLOTS_ADDR   0x8009D018u
#define PE_WAYNE_COUNTER      0x800A78B0u

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
#define EN_STATUS_FLAGS2 0xCCu             /* & 0x40000: can't escape (bosses) */

#define REC_ITEM_ID  0x04u
#define REC_BONUS    0x0Eu

static int s_logging = 1;          /* on by default: playtest data for balancing (2026-10-13) */
static FILE *s_log;
static unsigned s_battle_no, s_battle_frames;
static int s_in_battle;
static int s_battle_escape, s_battle_boss, s_battle_hp_start;
static int s_pending;              /* PE_PENDING_*: a load to report next frame */
enum { PE_PENDING_NONE, PE_PENDING_NEWGAME, PE_PENDING_CARD, PE_PENDING_STATE };
static unsigned s_frame;
static int s_level_seen = -1;
static int s_wayne_valid, s_wayne_moving;
static unsigned s_wayne_prev, s_wayne_from, s_wayne_changed, s_wayne_room;

typedef struct {
    int tracking;
    uint8_t entity_id;
    int start_hp, last_hp, boss;
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

static int pe_game_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int s_session_written;

static int pe_log_open(void) {
    if (!s_log) {
        s_log = fopen("pe_play_log.csv", "a");
        if (!s_log) return 0;
        fseek(s_log, 0, SEEK_END);
        if (ftell(s_log) == 0)
            fprintf(s_log, "kind,battle,room,play_s,level,exp,hp,max_hp,junk,wayne,detail\n");
    }
    if (!s_session_written) {
        char stamp[32];
        const time_t now = time(NULL);
        const struct tm *local = localtime(&now);
        if (!local || !strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", local))
            snprintf(stamp, sizeof stamp, "unknown");
        s_session_written = 1;
        pe_log("session", "start=%s", stamp);
    }
    return 1;
}

void pe_log(const char *kind, const char *fmt, ...) {
    char detail[320];
    va_list ap;
    if (!s_logging || !pe_log_open()) return;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    for (char *c = detail; *c; ++c)            /* one CSV field */
        if (*c == ',') *c = ';';
        else if (*c == '"') *c = '\'';
        else if (*c == '\n' || *c == '\r') *c = ' ';
    char battle[12] = "";
    if (s_in_battle) snprintf(battle, sizeof battle, "%u", s_battle_no);
    fprintf(s_log, "%s,%s,0x%04X,", kind, battle, psx_mod_read_half(PE_ROOM_ADDR));
    if (pe_game_loaded())
        fprintf(s_log, "%u,%u,%u,%u,%u,%u,%u,", psx_mod_read_word(PE_PLAY_FRAMES_ADDR) / 60u,
                psx_mod_read_byte(PE_LEVEL_ADDR) + 1u, psx_mod_read_word(PE_TOTAL_EXP_ADDR),
                psx_mod_read_half(PE_CUR_HP_ADDR), psx_mod_read_half(PE_MAX_HP_ADDR),
                (unsigned)pe_loot_junk(), psx_mod_read_half(PE_WAYNE_COUNTER));
    else
        fprintf(s_log, ",,,,,,,");
    fprintf(s_log, "%s\n", detail);
    fflush(s_log);
}

static void pe_log_enemy(int slot, int died) {
    const PeEnemyTrack *e = &s_enemy[slot];
    if (e->identity == 0) return;      /* an empty slot, not a real enemy */
    pe_log("enemy", "slot=%d id=0x%04X hp=%d def=%u exp=%u bp=%u/%u/%u drop=0x%02X steal=0x%02X boss=%d died=%d",
           slot, e->identity, e->start_hp, e->defense, e->exp, e->bp_base, e->bp_factor,
           e->bp_hits, e->drop, e->steal, e->boss, died);
}

static unsigned pe_equipped_id(int which) {
    const uint32_t rec = pe_cat_record(which);
    return rec ? psx_mod_read_byte(rec + REC_ITEM_ID) : 0u;
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
    const int hp = psx_mod_read_half(PE_CUR_HP_ADDR);
    const char *outcome = hp <= 0 ? "loss" : s_battle_escape ? "escape" : "win";
    pe_log("battle", "frames=%u secs=%u exp=%u bp=%u outcome=%s hp=%d->%d gun=0x%02X armor=0x%02X boss=%d rewards=%s",
           s_battle_frames, (s_battle_frames * 10u / 599u + 5u) / 10u,
           psx_mod_read_word(PE_BATTLE_EXP_ADDR), psx_mod_read_word(PE_BATTLE_BP_ADDR), outcome,
           s_battle_hp_start, hp, pe_equipped_id(0), pe_equipped_id(1), s_battle_boss, rewards);
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
        e->boss = (psx_mod_read_word(core + EN_STATUS_FLAGS2) & 0x40000u) != 0;
        if (e->identity && e->boss) s_battle_boss = 1;
    }
}

/* Wayne's count: one row per hand-in or craft, once it stops moving. */
static void pe_track_wayne(void) {
    const unsigned now = psx_mod_read_half(PE_WAYNE_COUNTER);
    const unsigned room = psx_mod_read_half(PE_ROOM_ADDR);
    if (!s_wayne_valid) { s_wayne_valid = 1; s_wayne_prev = now; s_wayne_room = room; return; }
    if (now != s_wayne_prev) {
        if (!s_wayne_moving) { s_wayne_moving = 1; s_wayne_from = s_wayne_prev; }
        s_wayne_changed = s_frame;
        s_wayne_prev = now;
    }
    if (s_wayne_moving && (s_frame - s_wayne_changed > PE_WAYNE_SETTLE_FRAMES || room != s_wayne_room)) {
        s_wayne_moving = 0;
        if (now != s_wayne_from) pe_log("wayne", "from=%u to=%u", s_wayne_from, now);
    }
    s_wayne_room = room;
}

/* What a memory card save holds that matters for balance. */
static void pe_append_gear(char *out, size_t size, int which) {
    const uint32_t rec = pe_cat_record(which);
    size_t n = strlen(out);
    if (!rec) { snprintf(out + n, size - n, " %s=none", which ? "armor" : "gun"); return; }
    n += (size_t)snprintf(out + n, size - n, " %s=0x%02X+%d/%d/%d mods=", which ? "armor" : "gun",
                          psx_mod_read_byte(rec + REC_ITEM_ID),
                          (int16_t)psx_mod_read_half(rec + REC_BONUS),
                          (int16_t)psx_mod_read_half(rec + REC_BONUS + 2u),
                          (int16_t)psx_mod_read_half(rec + REC_BONUS + 4u));
    const int slots = pe_cat_slots(which);
    for (int s = 0; s < slots && n < size; ++s)
        n += (size_t)snprintf(out + n, size - n, "%s%d", s ? "/" : "", pe_cat_slot_effect(which, s));
    if (slots <= 0 && n < size) snprintf(out + n, size - n, "-");
}

static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    if (!s_logging || !pe_game_loaded()) return;
    unsigned limit = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    if (limit > 50u) limit = 50u;
    unsigned used = 0;
    for (unsigned i = 0; i < limit; ++i) used += psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) != 0;
    char gear[200];
    snprintf(gear, sizeof gear, "bag=%u/%u bp=%u", used, limit, psx_mod_read_word(PE_BONUS_POINTS_ADDR));
    pe_append_gear(gear, sizeof gear, 0);
    pe_append_gear(gear, sizeof gear, 1);
    pe_log("save", "%s", gear);
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    s_pending = PE_PENDING_CARD;
}

static void pe_on_restored(void) { s_pending = PE_PENDING_STATE; }

static void pe_loot_logger_tick(void) {
    ++s_frame;
    if (!s_logging) {
        if (s_log) { fclose(s_log); s_log = 0; }
        s_counts_valid = 0;
        s_pending = PE_PENDING_NONE;
        return;
    }
    if (!pe_game_loaded()) return;

    if (s_pending) {                         /* the loaded game's context, now */
        static const char *const kinds[] = { "", "newgame", "cardload", "stateload" };
        pe_log(kinds[s_pending], "%s", "");
        s_pending = PE_PENDING_NONE;
        s_counts_valid = 0;
        s_wayne_valid = s_wayne_moving = 0;
        s_level_seen = -1;
        s_in_battle = 0;
        memset(s_enemy, 0, sizeof s_enemy);
    }
    const int level = psx_mod_read_byte(PE_LEVEL_ADDR) + 1;
    if (s_level_seen > 0 && level > s_level_seen) pe_log("levelup", "to=%d", level);
    s_level_seen = level;

    const int battle = (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE) != 0;
    if (battle) {
        if (!s_in_battle) {
            s_in_battle = 1; ++s_battle_no; s_battle_frames = 0;
            s_battle_escape = s_battle_boss = 0;
            s_battle_hp_start = psx_mod_read_half(PE_CUR_HP_ADDR);
        }
        ++s_battle_frames;
        if (psx_mod_read_word(PE_BATTLE_PHASE_ADDR) == 4u) s_battle_escape = 1;
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
    pe_track_wayne();

    /* Field pickups: count changes of watched items. */
    for (unsigned i = 0; i < PE_WATCH_COUNT; ++i) {
        const int now = pe_count_item(s_watch_ids[i]);
        if (s_counts_valid && now != s_counts[i])
            pe_log("pickup", "item=0x%02X delta=%d", s_watch_ids[i], now - s_counts[i]);
        s_counts[i] = now;
    }
    s_counts_valid = 1;
}

/* Inv_AddItem(id) entry: a steal when called from the hit resolver, else a
 * chest / gift / craft / results-screen pickup. */
static void pe_on_inv_add_item(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_logging || !psx_mod_game_started()) return;
    const uint32_t ra = cpu->gpr[31];
    const int steal = ra >= PE_FN_RESOLVE_HIT_START && ra < PE_FN_RESOLVE_HIT_END;
    pe_log(steal ? "steal" : "add", "item=0x%02X", (unsigned)(cpu->gpr[4] & 0xFFFFu));
}

/* Inv_DispatchCommand(command, value, other) entry: script inventory actions. */
static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    if (!s_logging || !psx_mod_game_started()) return;
    if ((int)cpu->gpr[4] == PE_INV_CMD_START_NEW_GAME) s_pending = PE_PENDING_NEWGAME;
    pe_log("invcmd", "cmd=%d value=0x%X other=%d", (int)cpu->gpr[4], (unsigned)cpu->gpr[5],
           (int)cpu->gpr[6]);
}

/* Called from pe_debug_menu.c's page (appended after its rows). */
void pe_loot_logger_menu_register(void) {
    (void)pe_menu_register_toggle("Debug", "Play log",
        "Log play to pe_play_log.csv.", &s_logging, 0);
    (void)pe_menu_register_tick(pe_loot_logger_tick);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.add-item",
        PE_FN_INV_ADD_ITEM, pe_on_inv_add_item);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.loot-logger.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_restore_callback(pe_on_restored);
}
