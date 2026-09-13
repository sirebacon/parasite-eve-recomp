#include "mod_plugins.h"
#include <stdio.h>

/*
 * Increased Storage: raise Aya's inventory slot cap 50 -> 80.
 *
 * Two clamp sites in the retail SLUS-00662 executable enforce the 50-slot
 * cap, both confirmed by direct disassembly (not the decomp's guessed
 * addresses -- those pointed at the wrong function; the real addresses
 * below were found by scanning the executable for the exact `slti *,*,0x33`
 * instruction pattern and cross-checking a `jal` target between the two
 * candidates found next to each other in memory):
 *
 *   Inv_SetAyaSlotCount (0x80052F24):
 *     0x80052F28  slti  $v0, $a0, 0x33   -- if (count >= 0x33)
 *     0x80052F34  addiu $a0, $zero, 0x32 --     count = 0x32
 *
 *   Inv_GetAyaSlotLimit (0x80052F70), called by the setter above via a
 *   `jal` at 0x80052F54 whose target lands exactly on this address --
 *   the cross-check that confirmed both addresses are correct:
 *     0x80052F94  slti  $v1, $v1, 0x33   -- if (base+bonus >= 0x33)
 *     0x80052FB4  addiu $v0, $zero, 0x32 --     return 0x32
 *
 * Each pair keeps the same instruction (same opcode/registers), only the
 * 16-bit immediate changes: 0x33 -> 0x51 (new threshold, 81) and
 * 0x32 -> 0x50 (new cap, 80). u8 storage for the raw slot-count byte
 * (g_AyaInventorySlotCount) has headroom to 255, so 80 is not a
 * representation-size risk.
 *
 * NOT verified: whether the backing item array (g_AyaInventoryItems) is
 * itself allocated for more than 50 entries, or exactly 50. No linker map
 * was available to confirm this statically. This ships behind an
 * off-by-default feature flag specifically so it can be play-tested by
 * actually filling storage past the old cap before being trusted --
 * see the manifest description.
 */

#define PE_SLOT_PATCH_COUNT 4

typedef struct {
    uint32_t address;
    uint32_t expected_original;
    uint32_t patched_value;
    const char* description;
} PeSlotPatch;

static const PeSlotPatch kSlotPatches[PE_SLOT_PATCH_COUNT] = {
    {0x80052F28u, 0x28820033u, 0x28820051u,
     "Inv_SetAyaSlotCount: slti $v0,$a0,0x33 -> 0x51"},
    {0x80052F34u, 0x24040032u, 0x24040050u,
     "Inv_SetAyaSlotCount: addiu $a0,$zero,0x32 -> 0x50"},
    {0x80052F94u, 0x28630033u, 0x28630051u,
     "Inv_GetAyaSlotLimit: slti $v1,$v1,0x33 -> 0x51"},
    {0x80052FB4u, 0x24020032u, 0x24020050u,
     "Inv_GetAyaSlotLimit: addiu $v0,$zero,0x32 -> 0x50"},
};

static void pe_increased_storage_activate(void) {
    int i;
    for (i = 0; i < PE_SLOT_PATCH_COUNT; ++i) {
        const PeSlotPatch* p = &kSlotPatches[i];
        uint32_t live = psx_mod_read_word(p->address);
        if (live != p->expected_original) {
            /* Fail loud and skip this one write rather than patch bytes we
             * don't recognize -- a mismatch here means either a different
             * game revision or a wrong address, and silently writing over
             * unknown code is worse than leaving the cap at 50. */
            fprintf(stderr,
                    "pe.increased-storage: SKIPPED 0x%08X (%s) -- expected "
                    "0x%08X, found 0x%08X\n",
                    p->address, p->description, p->expected_original, live);
            continue;
        }
        psx_mod_write_code_word(p->address, p->patched_value);
    }
}

PSX_MOD_CONSTRUCTOR(pe_register_increased_storage_plugin) {
    (void)psx_mod_register_activation_plugin(
        "pe.increased-storage", pe_increased_storage_activate);
}
