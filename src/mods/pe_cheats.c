/*
 * PE Enhancements: "No random battles" and "Infinite ammo".
 *
 * NO RANDOM BATTLES: the game's own encounter switch, bit 0x8000 of the half
 * at 0x800A781A (g_BattleSkip in the decomp's symbol list; the old Action
 * Replay code 800A781A 8000 and the PE randomizer's battle-skip option set the
 * same bit). It sits at offset 0x2A of the script-variable area
 * g_EntityWorkBuffer (0x800A77F0), which room scripts read and which the game
 * SAVES WITH THE GAME (Save_TailBlobFlow.c copies all 0x800 bytes). So the row
 * is a live view of the real bit: it shows what a loaded save has, turning it
 * on sets the bit (and keeps it set for this session, in case a script clears
 * it), turning it off clears it. A save made while it is on keeps battles off
 * until the row is turned off again.
 *
 * EVENT SKIP (Debug page, experimental): bit 0x4 of the half at 0x800A77F0,
 * script variable 0 (g_EventSkip in the decomp's symbol catalogue, from the
 * old Action Replay code 800A77F0 0004, "Event Skip"). Room event scripts
 * (overlay bytecode, not decompiled C) read it; what they do with it, skip
 * outright or on a button, is what the toggle is for finding out (2026-10-14).
 * Same live-view handling as No random battles, and also saved with the game.
 *
 * MOVIE WATCHDOG (main page, on by default): some story movies stop getting
 * frames partway (\FMV004.STR stalls at frame 439 of 561, every time,
 * 2026-10-14): the stream library stops taking sectors, StGetNext never
 * returns a frame and MenuCardFirst_UpdateVideoFrame (0x80121270, menu_memcard
 * overlay, decomp video_frame.inc) spins forever while the game re-reads the
 * same stretch of disc. Root cause still open (bridge findings
 * fmv-freeze-overlay-cache-2026-10-14.md). The watchdog uses the game's own
 * exit: while that player is loaded (its first two instructions match) and a
 * movie is running (g_MenuCardVideoFinished 0x801223F5 is 0, the entry pointer
 * g_MenuCardVideoEntry 0x801227E4 is set), a last frame
 * (g_MenuCardVideoLastTime 0x801227E8) that hasn't moved for 5 seconds sets
 * Finished, as reaching the movie's last frame would. The play log gets a
 * fmv_watchdog row.
 *
 * INFINITE AMMO (decomp: Inv_DrawSlotItemIcon, BattleCmd_CommitAmmoAndUpdate,
 * Battle_StatusPrimitiveFlow's ammo counter):
 *   - Spare rounds live in one of three pools (ItemDataRecord[3] at
 *     0x800A1E64; kind 8 uses its own record at 0x800A1E44), picked by the
 *     equipped gun's kind. "Refill spares" keeps that pool at its capacity
 *     (pool base[2] + bonus[2], max 999, as the give-item code does).
 *   - The gun's loaded rounds live in its equipment record (+0x0A) in the
 *     field, and in Aya's battle action during a battle (player -> core ->
 *     action (+0x68) -> attackWord (+0x0C), low 10 bits), which the game copies
 *     back to the record afterwards. "Never reload" also keeps those at the
 *     gun's capacity (base[2] + bonus[2], max 999).
 */
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_log.h"

#define PE_BATTLE_SKIP_ADDR   0x800A781Au
#define PE_BATTLE_SKIP_BIT    0x8000u
#define PE_EVENT_SKIP_ADDR    0x800A77F0u   /* script variable 0 */
#define PE_EVENT_SKIP_BIT     0x0004u
#define PE_FMV_PLAYER_FN      0x80121270u   /* MenuCardFirst_UpdateVideoFrame */
#define PE_FMV_PLAYER_SIG0    0x27BDFFD0u   /* its first two instructions */
#define PE_FMV_PLAYER_SIG1    0xAFB10024u
#define PE_FMV_FINISHED       0x801223F5u   /* u8 g_MenuCardVideoFinished */
#define PE_FMV_ENTRY          0x801227E4u   /* VideoEntry *g_MenuCardVideoEntry */
#define PE_FMV_LAST_TIME      0x801227E8u   /* last frame shown */
#define PE_FMV_STUCK_TICKS    300u          /* 5 s of 60 Hz ticks */

#define PE_MAX_HP_ADDR        0x800C0E06u
#define PE_WEAPON_SLOT_ADDR   0x800C0E20u
#define PE_AYA_SLOTS_ADDR     0x800C0E48u
#define PE_EQUIP_RECORDS_ADDR 0x800C0EACu
#define PE_AMMO_POOLS_ADDR    0x800A1E64u /* ItemDataRecord[3] */
#define PE_AMMO_POOL_KIND8    0x800A1E44u
#define PE_PLAYER_PTR_ADDR    0x8009D254u
#define PE_GAME_BLOCK_ADDR    0x8009D1A0u
#define PE_GAME_BLOCK_BATTLE  0x2u

#define REC_KIND   0x06u
#define REC_BASE2  0x09u /* baseStats[2]: bullet capacity */
#define REC_AMMO   0x0Au
#define REC_BONUS2 0x12u /* bonusStats[2] */
#define COMBATANT_ACTION    0x68u
#define ACTION_ATTACK_WORD  0x0Cu
#define ATTACK_WORD_AMMO    0x3FFu

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_ram_pointer(uint32_t p) {
    return p >= 0x80000000u && p < 0x80200000u;
}

/* ---- no random battles ---------------------------------------------------- */
static int s_force_no_battles;   /* chosen ON from the menu this session */

static int pe_get_no_battles(void) {
    if (!pe_loaded()) return 0;
    return (psx_mod_read_half(PE_BATTLE_SKIP_ADDR) & PE_BATTLE_SKIP_BIT) ? 1 : 0;
}

static int pe_set_no_battles(int value) {
    if (!pe_loaded()) return 0;
    const uint16_t v = psx_mod_read_half(PE_BATTLE_SKIP_ADDR);
    psx_mod_write_half(PE_BATTLE_SKIP_ADDR,
        (uint16_t)(value ? (v | PE_BATTLE_SKIP_BIT) : (v & ~PE_BATTLE_SKIP_BIT)));
    s_force_no_battles = value ? 1 : 0;
    return 1;
}

/* ---- event skip (experimental) ----------------------------------------------- */
static int s_force_event_skip;

static int pe_get_event_skip(void) {
    if (!pe_loaded()) return 0;
    return (psx_mod_read_half(PE_EVENT_SKIP_ADDR) & PE_EVENT_SKIP_BIT) ? 1 : 0;
}

static int pe_set_event_skip(int value) {
    if (!pe_loaded()) return 0;
    const uint16_t v = psx_mod_read_half(PE_EVENT_SKIP_ADDR);
    psx_mod_write_half(PE_EVENT_SKIP_ADDR,
        (uint16_t)(value ? (v | PE_EVENT_SKIP_BIT) : (v & ~PE_EVENT_SKIP_BIT)));
    s_force_event_skip = value ? 1 : 0;
    return 1;
}

/* ---- movie watchdog ---------------------------------------------------------- */
static int s_fmv_watchdog = 1;

static void pe_fmv_watchdog_tick(void) {
    static uint32_t s_last = 0xFFFFFFFFu, s_entry, s_stuck;
    if (!s_fmv_watchdog || psx_mod_read_word(PE_FMV_PLAYER_FN) != PE_FMV_PLAYER_SIG0 ||
        psx_mod_read_word(PE_FMV_PLAYER_FN + 4u) != PE_FMV_PLAYER_SIG1) {
        s_stuck = 0; s_last = 0xFFFFFFFFu; return;
    }
    const uint32_t entry = psx_mod_read_word(PE_FMV_ENTRY);
    if (entry < 0x80120000u || entry >= 0x80130000u || psx_mod_read_byte(PE_FMV_FINISHED)) {
        s_stuck = 0; s_last = 0xFFFFFFFFu; return;
    }
    const uint32_t last = psx_mod_read_word(PE_FMV_LAST_TIME);
    if (entry != s_entry || last != s_last) { s_entry = entry; s_last = last; s_stuck = 0; return; }
    if (++s_stuck < PE_FMV_STUCK_TICKS) return;
    char name[20];
    const uint32_t np = psx_mod_read_word(entry);
    unsigned i = 0;
    for (; i < sizeof name - 1 && np >= 0x80000000u && np < 0x80200000u; ++i) {
        const char c = (char)psx_mod_read_byte(np + i);
        if (!c) break;
        name[i] = (c > 32 && c < 127 && c != ',') ? c : '_';
    }
    name[i] = 0;
    pe_log("fmv_watchdog", "movie=%s frame=%u end=%d", name, (unsigned)last,
           (int)(int16_t)psx_mod_read_half(entry + 8u));
    psx_mod_write_byte(PE_FMV_FINISHED, 1);       /* the game's own end of movie */
    s_stuck = 0;
}

/* Both switches: keep a bit a script cleared set, for this session. */
static void pe_no_battles_tick(void) {
    pe_fmv_watchdog_tick();                       /* runs from the boot movies on */
    if (!pe_loaded()) return;
    if (s_force_no_battles) {
        const uint16_t v = psx_mod_read_half(PE_BATTLE_SKIP_ADDR);
        if (!(v & PE_BATTLE_SKIP_BIT))
            psx_mod_write_half(PE_BATTLE_SKIP_ADDR, (uint16_t)(v | PE_BATTLE_SKIP_BIT));
    }
    if (s_force_event_skip) {
        const uint16_t v = psx_mod_read_half(PE_EVENT_SKIP_ADDR);
        if (!(v & PE_EVENT_SKIP_BIT))
            psx_mod_write_half(PE_EVENT_SKIP_ADDR, (uint16_t)(v | PE_EVENT_SKIP_BIT));
    }
}

/* ---- battle frequency --------------------------------------------------------
 * Room scripts (bytecode, loaded around 0x801A0000) roll for a random battle as
 *   Script_Random $r #0 #N        (op 0x1A: $r = roll in [0, N), N = 0x100)
 *   Script_EvalExpr #A V0 $r $t   (op 0x09: V0 = roll against the room's encounter value $t)
 *   Script_JumpIfZero V0 ...      (op 0x05: no battle)
 *   ... Boot_MemCardStepInit #n   (op 0x55: start battle n), a few instructions on
 * (traced 2026-10-15: the [0,256) roll comes right before every random battle).
 * Shrinking N in the loaded script raises the odds: N/2 doubles them, N = 1 makes
 * every check a battle (a room with no encounter value still has none). Patched
 * after each room load; the originals are put back when the row goes to Normal. */
#define PE_SCENE_TOKEN        0x8009D280u
#define PE_SCRIPT_LO          0x80198000u   /* room scripts seen at 0x801A0000-0x801B2000 */
#define PE_SCRIPT_HI          0x801C8000u
#define PE_ENC_MAX            16
enum { PE_ENC_NORMAL, PE_ENC_MORE, PE_ENC_LOTS, PE_ENC_ALWAYS, PE_ENC_CHOICES };
static const char *const s_enc_names[PE_ENC_CHOICES] = { "NORMAL", "MORE (x2)", "LOTS (x4)", "EVERY CHECK" };
static int s_enc_mode, s_enc_applied = -1;
static struct { uint32_t at; uint32_t orig; } s_enc[PE_ENC_MAX];
static int s_enc_count;
static uint32_t s_enc_token, s_enc_scan_until, s_enc_frames;

static unsigned pe_ins_argc(uint32_t h) { return (h >> 13) & 15u; }
static unsigned pe_ins_mode(uint32_t h, unsigned i) { return (h >> (17u + 3u * i)) & 7u; }   /* args 0-4 */

/* Script_Random at `a` that feeds an encounter check? */
static int pe_is_encounter_roll(uint32_t a) {
    const uint32_t h = psx_mod_read_word(a);
    if ((h & 0x1FFFu) != 0x1Au || pe_ins_argc(h) != 3u) return 0;
    if (pe_ins_mode(h, 1) != 0u || pe_ins_mode(h, 2) != 0u) return 0;      /* #start #end */
    if (psx_mod_read_word(a + 12u) != 0u) return 0;                        /* start 0 */
    const uint32_t e = a + 20u, h2 = psx_mod_read_word(e);
    if ((h2 & 0x1FFFu) != 0x09u || pe_ins_argc(h2) != 4u) return 0;
    if (psx_mod_read_word(e + 8u) != 0xAu || psx_mod_read_word(e + 16u) != psx_mod_read_word(a + 8u)) return 0;
    const uint32_t j = e + 24u;
    if ((psx_mod_read_word(j) & 0x1FFFu) != 0x05u) return 0;
    for (uint32_t p = j, n = 0; n < 16u && p < PE_SCRIPT_HI; ++n) {        /* a battle start soon after */
        const uint32_t hp = psx_mod_read_word(p);
        if ((hp & 0x1FFFu) == 0x55u && pe_ins_argc(hp) == 1u) return 1;
        p += 8u + 4u * pe_ins_argc(hp);
    }
    return 0;
}

static uint32_t pe_enc_end(uint32_t orig) {
    switch (s_enc_mode) {
    case PE_ENC_MORE: return orig / 2u ? orig / 2u : 1u;
    case PE_ENC_LOTS: return orig / 4u ? orig / 4u : 1u;
    case PE_ENC_ALWAYS: return 1u;
    default: return orig;
    }
}

static void pe_enc_restore(void) {
    for (int i = 0; i < s_enc_count; ++i)
        if (psx_mod_read_word(s_enc[i].at) == pe_enc_end(s_enc[i].orig) || s_enc_mode == PE_ENC_NORMAL)
            psx_mod_write_word(s_enc[i].at, s_enc[i].orig);
    s_enc_count = 0;
}

/* Adds rolls not seen yet in this room (its script arrives in pieces) and patches
 * them; returns how many were new. */
static int pe_enc_scan(void) {
    int added = 0;
    for (uint32_t a = PE_SCRIPT_LO; a < PE_SCRIPT_HI - 64u && s_enc_count < PE_ENC_MAX; a += 4u) {
        if ((psx_mod_read_word(a) & 0x1FFFu) != 0x1Au || !pe_is_encounter_roll(a)) continue;
        int known = 0;
        for (int i = 0; i < s_enc_count; ++i) if (s_enc[i].at == a + 16u) known = 1;
        if (known) continue;
        const uint32_t orig = psx_mod_read_word(a + 16u);
        if (orig < 2u || orig > 0x1000u) continue;
        s_enc[s_enc_count].at = a + 16u;
        s_enc[s_enc_count].orig = orig;
        psx_mod_write_word(a + 16u, pe_enc_end(orig));
        ++s_enc_count;
        ++added;
    }
    return added;
}

static void pe_enc_tick(void) {
    ++s_enc_frames;
    if (!pe_loaded()) { s_enc_count = 0; return; }
    const uint32_t token = psx_mod_read_word(PE_SCENE_TOKEN);
    if (token != s_enc_token) {              /* a new room: its script loads over the next moments */
        s_enc_token = token;
        s_enc_count = 0;
        s_enc_scan_until = s_enc_frames + 240u;
    }
    if (s_enc_mode != s_enc_applied) {       /* the row changed: undo, then patch again */
        const int mode = s_enc_mode;
        s_enc_mode = s_enc_applied < 0 ? PE_ENC_NORMAL : s_enc_applied;
        pe_enc_restore();
        s_enc_mode = mode;
        s_enc_applied = mode;
        s_enc_scan_until = s_enc_frames + 30u;
    }
    if (s_enc_mode == PE_ENC_NORMAL) return;
    if (psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE) return;
    /* Shared encounter code (same addresses in every room, 0x801B1264 / 0x801B18A8 on
     * 2026-10-15) is copied in again after a room settles, undoing a patch: put any
     * known roll that is back to its original value right again, every frame. */
    for (int i = 0; i < s_enc_count; ++i)
        if (psx_mod_read_word(s_enc[i].at) == s_enc[i].orig)
            psx_mod_write_word(s_enc[i].at, pe_enc_end(s_enc[i].orig));
    /* The room rolls once, soon after it loads (then sets a "checked" bit), so scan
     * every other frame for its first seconds, then twice a second for late pieces. */
    if (s_enc_frames <= s_enc_scan_until ? (s_enc_frames & 1u) : (s_enc_frames % 30u)) return;
    if (pe_enc_scan())
        pe_log("encounters", "mode=%s rolls=%d end=%u->%u", s_enc_names[s_enc_mode], s_enc_count,
               (unsigned)s_enc[0].orig, (unsigned)pe_enc_end(s_enc[0].orig));
}

/* ---- infinite ammo -------------------------------------------------------- */
enum { PE_AMMO_OFF, PE_AMMO_SPARES, PE_AMMO_NO_RELOAD, PE_AMMO_CHOICES };
static const char *const s_ammo_names[PE_AMMO_CHOICES] = {
    "OFF", "REFILL SPARES", "NEVER RELOAD",
};
static int s_ammo_mode;

/* Equipped gun's equipment record, or 0. */
static uint32_t pe_gun_record(void) {
    const int8_t slot = (int8_t)psx_mod_read_byte(PE_WEAPON_SLOT_ADDR);
    if (slot < 0 || slot >= 50) return 0;
    const uint16_t v = psx_mod_read_half(PE_AYA_SLOTS_ADDR + (uint32_t)slot * 2u);
    if (v < 256 || v >= 384) return 0;
    return PE_EQUIP_RECORDS_ADDR + (uint32_t)(v - 256) * 0x20u;
}

/* base[2] + bonus[2], clamped to 0..999 (the game's ClampAmmo rule). */
static uint16_t pe_capacity(uint32_t rec) {
    int cap = psx_mod_read_byte(rec + REC_BASE2) +
              (int16_t)psx_mod_read_half(rec + REC_BONUS2);
    if (cap > 999) cap = 999;
    if (cap < 0) cap = 0;
    return (uint16_t)cap;
}

/* The spare-round pool the game reloads this gun from (Inv_DrawSlotItemIcon). */
static uint32_t pe_pool_for(uint32_t gun) {
    const unsigned kind = psx_mod_read_byte(gun + REC_KIND);
    if (kind >= 1 && kind < 8)
        return PE_AMMO_POOLS_ADDR + (kind > 4 ? kind - 5u : 0u) * 0x20u;
    if (kind >= 19 && kind <= 21) return PE_AMMO_POOLS_ADDR + (kind - 19u) * 0x20u;
    if (kind == 8) return PE_AMMO_POOL_KIND8;
    return 0;
}

static void pe_fill(uint32_t rec) {
    const uint16_t cap = pe_capacity(rec);
    if (psx_mod_read_half(rec + REC_AMMO) < cap) psx_mod_write_half(rec + REC_AMMO, cap);
}

static void pe_ammo_tick(void) {
    if (s_ammo_mode == PE_AMMO_OFF || !pe_loaded()) return;
    const uint32_t gun = pe_gun_record();
    if (!gun) return;
    const uint32_t pool = pe_pool_for(gun);
    if (pool) pe_fill(pool);
    if (s_ammo_mode != PE_AMMO_NO_RELOAD) return;

    pe_fill(gun);
    if (!(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & PE_GAME_BLOCK_BATTLE)) return;
    const uint32_t player = psx_mod_read_word(PE_PLAYER_PTR_ADDR);
    if (!pe_ram_pointer(player)) return;
    const uint32_t core = psx_mod_read_word(player);
    if (!pe_ram_pointer(core)) return;
    const uint32_t action = psx_mod_read_word(core + COMBATANT_ACTION);
    if (!pe_ram_pointer(action)) return;
    const uint32_t word = psx_mod_read_word(action + ACTION_ATTACK_WORD);
    const uint32_t cap = pe_capacity(gun);
    if ((word & ATTACK_WORD_AMMO) < cap)
        psx_mod_write_word(action + ACTION_ATTACK_WORD, (word & ~ATTACK_WORD_AMMO) | cap);
}

/* Called from pe_mod_menu.c, which fixes the order of the PE page. */
void pe_cheats_menu_register(void) {
    static const char *const off_on[2] = { "OFF", "ON" };
    (void)pe_menu_register_live_choice(0, "No random battles",
        "Stops random battles. Saved with game.", off_on, 2,
        pe_get_no_battles, pe_set_no_battles);
    (void)pe_menu_register_tick(pe_no_battles_tick);
    (void)pe_menu_register_choice(0, "Battle frequency",
        "More random battles (grinding).",
        &s_enc_mode, s_enc_names, PE_ENC_CHOICES);
    (void)pe_menu_register_tick(pe_enc_tick);
    (void)pe_menu_register_toggle(0, "Unstick movies",
        "Ends a movie stuck for 5 s.", &s_fmv_watchdog, 0);
    (void)pe_menu_register_live_choice("Debug", "Event skip (test)",
        "Game's own debug event skip. Saved.", off_on, 2,
        pe_get_event_skip, pe_set_event_skip);
    (void)pe_menu_register_choice(0, "Infinite ammo",
        "Refill spare rounds, or never reload.",
        &s_ammo_mode, s_ammo_names, PE_AMMO_CHOICES);
    (void)pe_menu_register_tick(pe_ammo_tick);
}
