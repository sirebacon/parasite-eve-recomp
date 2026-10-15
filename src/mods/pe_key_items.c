/*
 * Key item pocket: key items leave Aya's bag for a pocket of their own, shown
 * on the Key Items screen in her main menu (pe_catalog_menu.c), so they stop
 * taking bag slots.
 *
 * Which items: keys and story items 0xC8-0xDC (Dressing Room Key ... Port
 * Storage Key, Fuses, cardkeys, Weapon Memo, Shiny Medal) and the Chrysler
 * elevator keys 0x38-0x3E, and Maeda's good-luck charms Hamaya / Mayoke / Narita
 * (0xCD / 0xCC / 0xCB: "does nothing" per the RPGClassics item list; no code in the decomp
 * reads them, 2026-10-14). Not the gun trading cards.
 *
 * Why it's safe (research 2026-10-14): the Theater Key's doors opened with the
 * key out of the bag, and nothing called the inventory lookups at those doors
 * (function tracer over Inv_CountByValue / Inv_CountTotal / Inv_FindItemById):
 * doors keep their own "unlocked" story flags. In case a later key is checked
 * after all, two safety nets:
 *   - a script asking how many of a pocketed key Aya has (Script_CountItemsByValue
 *     0x800194F8, or Inv_DispatchCommand COUNT_ITEM 1102) gets the key lent
 *     back into a free bag slot first, so the game's count finds it; the pocket
 *     takes it again a couple of seconds later. With the bag full, the last
 *     non-key item steps aside for it and comes back when the key leaves
 *     (log key_unpark; found 2026-10-15: the Day 5 fuse box stalled on a full bag);
 *   - a key given with the bag full (Inv_AddItem) gets a slot the same way, and a
 *     key on the items-found list goes straight into the pocket (pe_keys_take);
 *   - a script removing one (Inv_DispatchCommand REMOVE_ITEM 1112) removes it
 *     from the pocket too.
 * Both log rows (key_lend / key_remove) so the play log shows any key that is
 * checked this way.
 *
 * Off ("Key item pocket" on the PE page): every pocketed key goes back to the
 * bag as slots allow. Saved as save-state block "pe.key-items" and in the
 * save's free tail at 0x1C90 + 0x200 (after the shop's block), under the
 * game's CRC.
 */
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pe_mod_menu.h"
#include "pe_loot.h"
#include "pe_log.h"

#define PE_FN_SCRIPT_COUNT_ITEMS 0x800194F8u  /* Script_CountItemsByValue(int **args) */
#define PE_FN_INV_DISPATCH       0x8005D2B4u  /* Inv_DispatchCommand(command, value, ...) */
#define PE_FN_INV_ADD_ITEM       0x80053D2Cu  /* Inv_AddItem(item) */
#define PE_FN_SAVE_SERIALIZE     0x8003F800u
#define PE_FN_SAVE_DESERIALIZE   0x8003FBD8u
#define PE_INV_CMD_COUNT_ITEM    1102
#define PE_INV_CMD_REMOVE_ITEM   1112
#define PE_INV_CMD_START_NEW_GAME 1117

#define PE_MAX_HP_ADDR           0x800C0E06u
#define PE_AYA_SLOTS_ADDR        0x800C0E48u
#define PE_SLOT_COUNT_ADDR       0x800C0E0Cu
#define PE_BONUS_SLOTS_ADDR      0x8009D018u
#define PE_GAME_BLOCK_ADDR       0x8009D1A0u
#define PE_FIELD_BUSY_ADDR       0x800B0CD8u
#define PE_CARD_BUFFER           0x8009EED0u
#define PE_TAIL_OFFSET           (0x1C90u + 0x200u)
#define PE_TAIL_MAGIC            0x59454B50u  /* "PKEY" */
#define PE_TAIL_VERSION          1u

#define PE_KEY_MAX               32
#define PE_LEND_FRAMES           120u         /* ~2 s before a lent key is pocketed again */

#define PE_KEY_STATE_VERSION 1u
static struct {
    uint16_t count, pad;
    uint16_t ids[PE_KEY_MAX];
} s_kp;

static int s_enabled = 1;
static uint16_t s_lent;                       /* a key lent back to the bag for a check */
static uint32_t s_lent_until, s_frames;
/* Bag full: items moved aside to make room for a lent key, put back afterwards. */
#define PE_PARK_MAX 4
static uint16_t s_parked[PE_PARK_MAX];
static int s_parked_count;

static int pe_loaded(void) {
    return psx_mod_game_started() && psx_mod_read_half(PE_MAX_HP_ADDR) != 0;
}

static int pe_free_field(void) {
    return !(psx_mod_read_half(PE_FIELD_BUSY_ADDR) & 0xB600u) &&
           !(psx_mod_read_word(PE_GAME_BLOCK_ADDR) & 0x2006u);
}

static uint32_t pe_bag_limit(void) {
    uint32_t n = psx_mod_read_byte(PE_SLOT_COUNT_ADDR) + psx_mod_read_word(PE_BONUS_SLOTS_ADDR);
    return n > 50u ? 50u : n;
}

int pe_key_is_pocket_item(uint16_t v) {
    if (v >= 0x38u && v <= 0x3Eu) return 1;                         /* Chrysler keys */
    return v >= 0xC8u && v <= 0xDCu;                                /* keys, story items, charms */
}

static int pe_pocket_find(uint16_t v) {
    for (int i = 0; i < s_kp.count; ++i) if (s_kp.ids[i] == v) return i;
    return -1;
}

static void pe_pocket_remove_at(int i) {
    for (int k = i; k + 1 < s_kp.count; ++k) s_kp.ids[k] = s_kp.ids[k + 1];
    if (s_kp.count) --s_kp.count;
}

/* pe_loot.h-style API for the Key Items screen. */
int pe_keys_enabled(void) { return s_enabled; }
int pe_keys_count(void) { return s_kp.count; }
uint16_t pe_keys_item(int i) { return i >= 0 && i < s_kp.count ? s_kp.ids[i] : 0; }

/* Bag -> pocket (and, switched off, pocket -> bag). */
static void pe_key_tick(void) {
    ++s_frames;
    if (!pe_loaded() || !pe_free_field()) return;
    const uint32_t limit = pe_bag_limit();
    if (!s_enabled) {
        for (uint32_t i = 0; i < limit && s_kp.count; ++i) {
            const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
            if (psx_mod_read_half(a)) continue;
            psx_mod_write_half(a, s_kp.ids[0]);
            pe_pocket_remove_at(0);
        }
        return;
    }
    if (s_lent && s_frames < s_lent_until) return;   /* let the script finish with it */
    s_lent = 0;
    for (uint32_t i = 0; i < 50u; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        const uint16_t v = psx_mod_read_half(a);
        if (!pe_key_is_pocket_item(v)) continue;
        if (pe_pocket_find(v) < 0) {
            if (s_kp.count >= PE_KEY_MAX) continue;
            s_kp.ids[s_kp.count++] = v;
        }
        psx_mod_write_half(a, 0);                     /* a duplicate just leaves the bag */
    }
    /* Put back items moved aside for a lent key. */
    for (uint32_t i = 0; i < limit && s_parked_count; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a)) continue;
        psx_mod_write_half(a, s_parked[--s_parked_count]);
        pe_log("key_unpark", "item=0x%02X", psx_mod_read_half(a));
    }
}

/* A script is about to count `item` in the active list: lend a pocketed key
 * back into a free bag slot so the game's count finds it. */
static void pe_lend_for_check(unsigned item, const char *how) {
    if (!s_enabled || !pe_loaded() || !pe_key_is_pocket_item((uint16_t)item)) return;
    if (pe_pocket_find((uint16_t)item) < 0) return;
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i)
        if (psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u) == item) return;   /* already there */
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        if (psx_mod_read_half(a)) continue;
        psx_mod_write_half(a, (uint16_t)item);
        s_lent = (uint16_t)item;
        s_lent_until = s_frames + PE_LEND_FRAMES;
        pe_log("key_lend", "item=0x%02X via=%s", item, how);
        return;
    }
    /* Bag full: move the last non-key item aside until the key is pocketed again
     * (otherwise the count finds nothing and the script can stall, e.g. the fuse box). */
    for (uint32_t i = limit; i-- > 0;) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        const uint16_t v = psx_mod_read_half(a);
        if (!v || pe_key_is_pocket_item(v) || s_parked_count >= PE_PARK_MAX) continue;
        s_parked[s_parked_count++] = v;
        psx_mod_write_half(a, (uint16_t)item);
        s_lent = (uint16_t)item;
        s_lent_until = s_frames + PE_LEND_FRAMES;
        pe_log("key_lend", "item=0x%02X via=%s bag_full=1 parked=0x%02X", item, how, v);
        return;
    }
    pe_log("key_lend", "item=0x%02X via=%s bag_full=1", item, how);
}

/* A key is about to be added to the bag (Inv_AddItem: field pickups, script
 * gifts): with the bag full, the last non-key item steps aside so the key gets a
 * slot; the pocket takes the key at the next free moment and the item comes back. */
static void pe_on_add_item(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint16_t item = (uint16_t)cpu->gpr[4];
    if (!s_enabled || !pe_loaded() || !pe_key_is_pocket_item(item)) return;
    const uint32_t limit = pe_bag_limit();
    for (uint32_t i = 0; i < limit; ++i)
        if (!psx_mod_read_half(PE_AYA_SLOTS_ADDR + i * 2u)) return;          /* room already */
    for (uint32_t i = limit; i-- > 0;) {
        const uint32_t a = PE_AYA_SLOTS_ADDR + i * 2u;
        const uint16_t v = psx_mod_read_half(a);
        if (!v || pe_key_is_pocket_item(v) || s_parked_count >= PE_PARK_MAX) continue;
        s_parked[s_parked_count++] = v;
        psx_mod_write_half(a, 0);
        pe_log("key_add", "item=0x%02X bag_full=1 parked=0x%02X", item, v);
        return;
    }
}

/* pe_loot.c: a key on the items-found list with the bag full goes straight into
 * the pocket (the list only moves an entry into a free slot). 1 = taken. */
int pe_keys_take(uint16_t item) {
    if (!s_enabled || !pe_key_is_pocket_item(item)) return 0;
    if (pe_pocket_find(item) < 0) {
        if (s_kp.count >= PE_KEY_MAX) return 0;
        s_kp.ids[s_kp.count++] = item;
    }
    pe_log("key_add", "item=0x%02X bag_full=1 via=found", item);
    return 1;
}

static void pe_on_script_count_items(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const uint32_t args = cpu->gpr[4];
    if (args < 0x80000000u || args >= 0x80200000u) return;
    const uint32_t vp = psx_mod_read_word(args);
    if (vp < 0x80000000u || vp >= 0x80200000u) return;
    pe_lend_for_check(psx_mod_read_word(vp) & 0xFFFFu, "script");
}

static void pe_on_inv_dispatch(struct CPUState *cpu, uint32_t address) {
    (void)address;
    const int command = (int)cpu->gpr[4];
    const unsigned value = cpu->gpr[5] & 0xFFFFu;
    if (command == PE_INV_CMD_START_NEW_GAME) { memset(&s_kp, 0, sizeof s_kp); s_lent = 0; return; }
    if (command == PE_INV_CMD_COUNT_ITEM) { pe_lend_for_check(value, "count"); return; }
    if (command == PE_INV_CMD_REMOVE_ITEM && pe_key_is_pocket_item((uint16_t)value)) {
        const int i = pe_pocket_find((uint16_t)value);
        if (i >= 0) {
            pe_pocket_remove_at(i);
            if (s_lent == value) s_lent = 0;
            pe_log("key_remove", "item=0x%02X", value);
        }
    }
}

/* ---- persistence ------------------------------------------------------------ */
static uint32_t pe_checksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0x6B657973u;
    for (uint32_t i = 0; i < n; ++i) s = (s << 5) + s + p[i];
    return s;
}

static void pe_on_save_serialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    const uint8_t *p = (const uint8_t *)&s_kp;
    psx_mod_write_word(at + 0u, PE_TAIL_MAGIC);
    psx_mod_write_word(at + 4u, PE_TAIL_VERSION);
    psx_mod_write_word(at + 8u, (uint32_t)sizeof s_kp);
    for (uint32_t i = 0; i < sizeof s_kp; ++i) psx_mod_write_byte(at + 16u + i, p[i]);
    psx_mod_write_word(at + 12u, pe_checksum(p, sizeof s_kp));
}

static void pe_on_save_deserialize(struct CPUState *cpu, uint32_t address) {
    (void)cpu; (void)address;
    const uint32_t at = PE_CARD_BUFFER + PE_TAIL_OFFSET;
    memset(&s_kp, 0, sizeof s_kp);
    s_lent = 0;
    s_parked_count = 0;
    if (psx_mod_read_word(at) != PE_TAIL_MAGIC || psx_mod_read_word(at + 4u) != PE_TAIL_VERSION ||
        psx_mod_read_word(at + 8u) != sizeof s_kp)
        return;                                   /* an older save: its keys are in the bag */
    uint8_t buf[sizeof s_kp];
    for (uint32_t i = 0; i < sizeof s_kp; ++i) buf[i] = psx_mod_read_byte(at + 16u + i);
    if (pe_checksum(buf, sizeof s_kp) != psx_mod_read_word(at + 12u)) return;
    memcpy(&s_kp, buf, sizeof s_kp);
    if (s_kp.count > PE_KEY_MAX) memset(&s_kp, 0, sizeof s_kp);
}

static void pe_key_state_loaded(int result, uint32_t saved_version, const void *saved,
                                uint32_t saved_size) {
    (void)saved_version; (void)saved; (void)saved_size;
    if (result != PSX_MOD_STATE_RESTORED) memset(&s_kp, 0, sizeof s_kp);
    s_lent = 0;
    s_parked_count = 0;
}

/* Called from pe_mod_menu.c. */
void pe_key_items_register(void) {
    (void)pe_menu_register_toggle(0, "Key item pocket",
        "Key items leave the bag (Key Items menu).", &s_enabled, pe_key_tick);
    (void)psx_mod_register_function_entry_plugin("pe.key-items.count",
        PE_FN_SCRIPT_COUNT_ITEMS, pe_on_script_count_items);
    (void)psx_mod_register_function_entry_plugin("pe.key-items.inv-command",
        PE_FN_INV_DISPATCH, pe_on_inv_dispatch);
    (void)psx_mod_register_function_entry_plugin("pe.key-items.add",
        PE_FN_INV_ADD_ITEM, pe_on_add_item);
    (void)psx_mod_register_function_entry_plugin("pe.key-items.save",
        PE_FN_SAVE_SERIALIZE, pe_on_save_serialize);
    (void)psx_mod_register_function_entry_plugin("pe.key-items.load",
        PE_FN_SAVE_DESERIALIZE, pe_on_save_deserialize);
    (void)psx_mod_register_state("pe.key-items", PE_KEY_STATE_VERSION,
        &s_kp, sizeof s_kp, pe_key_state_loaded);
}
