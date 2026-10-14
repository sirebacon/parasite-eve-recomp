/*
 * Wayne's services from Warner, in a section with no way back to the station
 * (map 374, Day 4). Store equipment, Store item and the Workshop (Requisition
 * with the Mod Permits, Rebuild); not Discard junk -- making guns stays Wayne's.
 *
 * Warner's talk is an ordinary actor script (research 2026-10-14): Cross next
 * to him runs his contact task (actor +0x1A0 = script base + 0x154), which
 * locks the field (PlayFieldSfx FE), shows his two lines (op 0x0D message 0x1E,
 * wait op 0x22; then 0x1F), unlocks (PlayFieldSfx FF) and ends. No story
 * flags. The script VM (Script_RunQueue): a header word (opcode & 0x1FFF,
 * argument count >> 13 & 15, 3-bit argument modes >> 17), a tail word (modes
 * of arguments 6+), then the arguments; a handler returning 0 yields. Jumps
 * are script base + 2 * value.
 *
 * So, once his script is checked word for word (s_orig), it is patched:
 *   - his unlock (base + 0x350) jumps to our menu script, in mod memory;
 *   - after the first talk this visit, his first line (base + 0x2B0) does too,
 *     so later talks go straight to the menu.
 * Our script: show message 0x1F (pe_wayne_menu.c points the box at our text:
 * "Torres sent supplies." and Store equipment / Store item / Cancel, adding
 * its Workshop above Cancel as at Wayne's), read the choice (op 0x43), and for
 * 0 / 1 open the game's storage screen (op 0xEE, Script_BeginSceneTransition
 * -> Menu_OpenEquipScreen(0 equipment / 1 items)), yield, and show the menu
 * again; anything else runs his own unlock and ends. A new room load brings
 * his script back from disc, unpatched: it is checked and patched again.
 */
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"
#include "pe_gametext.h"

#define PE_MAX_HP_ADDR       0x800C0E06u
#define PE_ROOM_ADDR         0x8009D280u   /* g_SceneDispatchToken */
#define PE_BASE32_TABLE      0x800930B4u   /* g_Base32CharTable */
#define PE_ACTOR_LIST        0x8009D20Cu   /* g_FieldActorListHead */
#define A_NEXT        0x04u
#define A_TYPE        0x0Cu
#define A_SCRIPT_BASE 0x9Cu
#define A_NODE_LISTS  0xA0u                /* 3 ScriptNode lists */
#define A_CONTACT     0x1A0u               /* contact task entry */
#define N_SCRIPT      0x00u                /* ScriptNode.current.script */
#define N_NEXT        0x24u

#define PE_WARNER_MAP     374
#define PE_WARNER_TYPE    0x04u
#define PE_CONTACT_OFF    0x154u           /* contact entry, from the script base */
#define PE_FIRST_LINE_IDX 87               /* op 0x0D #1E: base + 0x2B0 */
#define PE_UNLOCK_IDX     127              /* PlayFieldSfx #FF: base + 0x350 */
#define PE_MENU_MESSAGE   0x1Fu

/* His contact task, base + 0x154 .. 0x36C, as read 2026-10-14. */
static const uint32_t s_orig[] = {
    0x01308009,0x00000000,0x00000003,0x00000000,0x00000036,0x00010000,0x01B08009,0x00000000,0x00000007,0x00000001,
    0x00000000,0x00000000,0x00064005,0x00000000,0x00000001,0x000001B2,0x01308009,0x00000000,0x0000000C,0x00000002,
    0x0000004A,0x000000E8,0x00064005,0x00000000,0x00000002,0x000001B2,0x00124024,0x00000000,0x00000000,0x00000001,
    0x00B08009,0x00000000,0x0000000B,0x00000000,0x00000000,0x00000000,0x00064005,0x00000000,0x00000000,0x000001B2,
    0x02308009,0x00000000,0x0000000B,0x00000001,0x00000012,0x00000000,0x00064005,0x00000000,0x00000001,0x000001B2,
    0x00806011,0x00000000,0x00000001,0x00000100,0x00000002,0x00B08009,0x00000000,0x0000000E,0x00000000,0x00000002,
    0x00000000,0x00064005,0x00000000,0x00000000,0x000001B2,0x000000AA,0x00000000,0x0000601C,0x00000000,0x00000000,
    0x00000000,0x000000FE,0x00000079,0x00000000,0x0000202E,0x00000000,0x00000001,0x0000604B,0x00000000,0x00000000,
    0x00000000,0x0000002D,0x0000202E,0x00000000,0x00000000,0x00000078,0x00000000,0x0000200D,0x00000000,0x0000001E,
    0x0000202E,0x00000000,0x00000005,0x0000204E,0x00000000,0x00000000,0x0000202F,0x00000000,0x00000077,0x00002022,
    0x00000000,0x0000001E,0x00002002,0x00000000,0x00000077,0x0000200D,0x00000000,0x0000001F,0x0000202E,0x00000000,
    0x00000003,0x0000204E,0x00000000,0x00000000,0x0000202F,0x00000000,0x00000050,0x00002022,0x00000000,0x0000001F,
    0x00000079,0x00000000,0x0000202E,0x00000000,0x00000000,0x00000078,0x00000000,0x0000601C,0x00000000,0x00000000,
    0x00000000,0x000000FF,0x00000020,0x00000000,
};
#define PE_ORIG_WORDS ((int)(sizeof s_orig / sizeof s_orig[0]))

/* Our menu script, word indices (see the top). */
enum { M_MENU = 0, M_ITEMS = 23, M_END = 37, M_WORDS = 44 };

static int s_enabled = 1;
static uint32_t s_script;                  /* mod memory: our script */
static uint32_t s_text;                    /* mod memory: the menu's text */
static uint32_t s_actor, s_base;           /* Warner, as patched */
static uint32_t s_room_token;
static int s_talked;                       /* this visit: his lines are said */

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_map_number(void) {
    const uint32_t token = psx_mod_read_word(PE_ROOM_ADDR);
    int n = 0;
    for (int i = 2; i < 5; ++i) {
        const char c = (char)psx_mod_read_byte(PE_BASE32_TABLE + ((token >> ((5 - i) * 5 + 2)) & 0x1Fu));
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
    }
    return n;
}

static uint32_t pe_jump_value_for(uint32_t base, uint32_t target) { return (target - base) >> 1; }
static uint32_t pe_jump_value(uint32_t target) { return pe_jump_value_for(s_base, target); }

/* The jump our patches write: op 0 (Script_JumpToEntityOffset) to our menu. */
static void pe_write_jump(uint32_t at) {
    psx_mod_write_word(at + 0u, 0x00002000u);
    psx_mod_write_word(at + 4u, 0);
    psx_mod_write_word(at + 8u, pe_jump_value(s_script + M_MENU * 4u));
}

static int pe_is_jump_for(uint32_t base, uint32_t at) {
    return s_script && psx_mod_read_word(at) == 0x00002000u && psx_mod_read_word(at + 4u) == 0 &&
           psx_mod_read_word(at + 8u) == pe_jump_value_for(base, s_script + M_MENU * 4u);
}
static int pe_is_jump(uint32_t at) { return pe_is_jump_for(s_base, at); }

/* His contact task, unpatched or patched by us? */
static int pe_script_matches(uint32_t base) {
    const uint32_t at = base + PE_CONTACT_OFF;
    for (int i = 0; i < PE_ORIG_WORDS; ++i) {
        if ((i >= PE_FIRST_LINE_IDX && i < PE_FIRST_LINE_IDX + 3) ||
            (i >= PE_UNLOCK_IDX && i < PE_UNLOCK_IDX + 3))
            continue;                          /* checked below */
        if (psx_mod_read_word(at + (uint32_t)i * 4u) != s_orig[i]) return 0;
    }
    for (int k = 0; k < 2; ++k) {
        const int idx = k ? PE_UNLOCK_IDX : PE_FIRST_LINE_IDX;
        const uint32_t a = at + (uint32_t)idx * 4u;
        int orig = 1;
        for (int i = 0; i < 3; ++i) orig &= psx_mod_read_word(a + (uint32_t)i * 4u) == s_orig[idx + i];
        if (!orig && !pe_is_jump_for(base, a)) return 0;
    }
    return 1;
}

static void pe_build_script(void) {
    uint32_t w[M_WORDS];
    const uint32_t menu = pe_jump_value(s_script + M_MENU * 4u);
    const uint32_t items = pe_jump_value(s_script + M_ITEMS * 4u);
    const uint32_t end = pe_jump_value(s_script + M_END * 4u);
    int i = 0;
    /* M_MENU */
    w[i++] = 0x0000200D; w[i++] = 0; w[i++] = PE_MENU_MESSAGE;        /* show the box */
    w[i++] = 0x00002022; w[i++] = 0; w[i++] = PE_MENU_MESSAGE;        /* until it closes */
    w[i++] = 0x00062043; w[i++] = 0; w[i++] = 0;                      /* V0 = choice */
    w[i++] = 0x0006601D; w[i++] = 0; w[i++] = 0; w[i++] = 0; w[i++] = items;   /* V0 != 0 -> items */
    w[i++] = 0x000020EE; w[i++] = 0; w[i++] = 0;                      /* storage: equipment */
    w[i++] = 0x00002002; w[i++] = 0; w[i++] = 1;                      /* yield */
    w[i++] = 0x00002000; w[i++] = 0; w[i++] = menu;
    /* M_ITEMS (23) */
    w[i++] = 0x0006601D; w[i++] = 0; w[i++] = 0; w[i++] = 1; w[i++] = end;     /* V0 != 1 -> end */
    w[i++] = 0x000020EE; w[i++] = 0; w[i++] = 1;                      /* storage: items */
    w[i++] = 0x00002002; w[i++] = 0; w[i++] = 1;
    w[i++] = 0x00002000; w[i++] = 0; w[i++] = menu;
    /* M_END (37): his own unlock and end */
    w[i++] = 0x0000601C; w[i++] = 0; w[i++] = 0; w[i++] = 0; w[i++] = 0xFF;
    w[i++] = 0x00000020; w[i++] = 0;
    for (int k = 0; k < i && k < M_WORDS; ++k) psx_mod_write_word(s_script + (uint32_t)k * 4u, w[k]);
}

/* Is Warner's talk task running our script now? */
static int pe_in_our_script(void) {
    if (!s_actor || !s_script) return 0;
    for (uint32_t l = 0; l < 3u; ++l) {
        uint32_t n = psx_mod_read_word(s_actor + A_NODE_LISTS + l * 4u);
        for (int guard = 0; pe_ram_pointer(n) && guard < 16; ++guard) {
            const uint32_t cur = psx_mod_read_word(n + N_SCRIPT);
            if (cur >= s_script && cur < s_script + M_WORDS * 4u) return 1;
            n = psx_mod_read_word(n + N_NEXT);
        }
    }
    return 0;
}

/* pe_wayne_menu.c: the text box our script opened shows this instead. */
uint32_t pe_field_wayne_text(void) { return s_text; }
int pe_field_wayne_box_ours(void) { return s_enabled && pe_in_our_script(); }

static void pe_write_text(void) {
    PeText t;
    memset(&t, 0, sizeof t);
    pe_t_str(&t, "Torres sent supplies.");
    pe_t_byte(&t, G_NEWLINE);
    pe_t_choice(&t, 3);
    pe_t_option_start(&t, 1); pe_t_str(&t, "Store equipment");
    pe_t_option_start(&t, 0); pe_t_str(&t, "Store item");
    pe_t_option_start(&t, 0); pe_t_str(&t, "Cancel");
    pe_t_byte(&t, G_END);
    for (unsigned i = 0; i < t.n; ++i) psx_mod_write_byte(s_text + i, t.b[i]);
}

static uint32_t pe_find_warner(void) {
    uint32_t a = psx_mod_read_word(PE_ACTOR_LIST);
    for (int guard = 0; pe_ram_pointer(a) && guard < 40; ++guard) {
        if (psx_mod_read_byte(a + A_TYPE) == PE_WARNER_TYPE) {
            const uint32_t base = psx_mod_read_word(a + A_SCRIPT_BASE);
            if (pe_ram_pointer(base) && psx_mod_read_word(a + A_CONTACT) == base + PE_CONTACT_OFF &&
                pe_script_matches(base))
                return a;
        }
        a = psx_mod_read_word(a + A_NEXT);
    }
    return 0;
}

static void pe_unpatch(void) {
    if (s_actor && s_base && psx_mod_read_word(s_actor + A_SCRIPT_BASE) == s_base && pe_script_matches(s_base)) {
        const uint32_t at = s_base + PE_CONTACT_OFF;
        for (int k = 0; k < 2; ++k) {
            const int idx = k ? PE_UNLOCK_IDX : PE_FIRST_LINE_IDX;
            for (int i = 0; i < 3; ++i)
                psx_mod_write_word(at + (uint32_t)(idx + i) * 4u, s_orig[idx + i]);
        }
    }
    s_actor = s_base = 0;
}

static void pe_field_wayne_tick(void) {
    if (!pe_loaded()) return;
    const uint32_t token = psx_mod_read_word(PE_ROOM_ADDR);
    if (token != s_room_token) { s_room_token = token; s_talked = 0; s_actor = s_base = 0; }
    if (!s_enabled || !pe_loot_enabled()) { pe_unpatch(); return; }
    if (pe_map_number() != PE_WARNER_MAP) { s_actor = s_base = 0; return; }
    if (!s_script) s_script = psx_mod_alloc_guest_memory(M_WORDS * 4u, 4);
    if (!s_text) s_text = psx_mod_alloc_guest_memory(128, 4);
    if (!s_script || !s_text) return;

    /* Still the actor we patched? (A room reload brings a fresh script.) */
    if (s_actor && (psx_mod_read_word(s_actor + A_SCRIPT_BASE) != s_base ||
                    psx_mod_read_byte(s_actor + A_TYPE) != PE_WARNER_TYPE || !pe_script_matches(s_base)))
        s_actor = s_base = 0;
    if (!s_actor) {
        const uint32_t a = pe_find_warner();
        if (!a) return;
        s_actor = a;
        s_base = psx_mod_read_word(a + A_SCRIPT_BASE);
        pe_log("field_wayne", "warner=0x%08X base=0x%08X", s_actor, s_base);
    }
    pe_build_script();                         /* mod memory: rewritten every frame (save states) */
    pe_write_text();
    const uint32_t at = s_base + PE_CONTACT_OFF;
    if (!pe_is_jump(at + PE_UNLOCK_IDX * 4u)) pe_write_jump(at + PE_UNLOCK_IDX * 4u);
    if (pe_in_our_script()) s_talked = 1;
    if (s_talked && !pe_is_jump(at + PE_FIRST_LINE_IDX * 4u)) pe_write_jump(at + PE_FIRST_LINE_IDX * 4u);
}

static void pe_field_wayne_restored(void) {
    s_actor = s_base = 0;                      /* re-found and re-checked next frame */
    s_room_token = psx_mod_read_word(PE_ROOM_ADDR);
}

/* Called from pe_mod_menu.c. */
void pe_field_wayne_register(void) {
    (void)pe_menu_register_toggle(0, "Warner helps out",
        "Warner (Day 4 park) offers storage and the Workshop.", &s_enabled, pe_field_wayne_tick);
    (void)psx_mod_register_restore_callback(pe_field_wayne_restored);
}
