"""Generate src/mods/pe_item_table.h from the decomp's include/pe1/item_ids.h.

    py -3 tools/gen_pe_item_table.py <decomp>/include/pe1/item_ids.h src/mods/pe_item_table.h

Items are grouped into types for the F1 menu's "item type" / "item" rows.
Ranges follow the ItemId enum's own boundaries; guns are split by the
enum comment ("Handgun", "Shotgun", ...).
"""
import re, sys

src, out = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
start = text.index("enum ItemId")
enum = text[start:text.index("};", start)]
rows = re.findall(r"ITEM_([A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+),\s*(?:/\*\s*(.*?)\s*\*/)?", enum)

GUN_TYPES = {
    "Handgun": "HANDGUNS", "Maeda's Gun With Bullets": "HANDGUNS",
    "Machine Gun": "MACHINE GUNS", "Shotgun": "SHOTGUNS", "Rifle": "RIFLES",
    "Grenade Launcher": "GRENADE LAUNCHERS", "Rocket Launcher": "ROCKET LAUNCHERS",
}

def item_type(v, comment):
    if v in (0x01, 0x02, 0x03, 0x04, 0x05, 0x1A, 0x1B, 0x1C): return "AMMO"
    if 0x06 <= v <= 0x0A or 0x0D <= v <= 0x12:            return "MEDICINE & CURES"
    if v == 0x13 or 0x1D <= v <= 0x34:                     return "STAT BOOSTERS"
    if v == 0x15 or 0xDD <= v <= 0xEB:                     return "TRADING CARDS"
    if 0x38 <= v <= 0x3E or 0xC8 <= v <= 0xDC:             return "KEY ITEMS"
    if v < 0x3F:                                           return "TOOLS & JUNK"
    if v < 0x44:                                           return "MELEE"
    if v < 0x95:                                           return GUN_TYPES[comment.strip('"')]
    if v < 0xA8:                                           return "ARMOR: N KV SP"
    if v < 0xB8:                                           return "ARMOR: SV CR"
    return "ARMOR: B CM"

ORDER = ["MEDICINE & CURES", "AMMO", "HANDGUNS", "MACHINE GUNS", "SHOTGUNS",
         "RIFLES", "GRENADE LAUNCHERS", "ROCKET LAUNCHERS", "MELEE",
         "ARMOR: N KV SP", "ARMOR: SV CR", "ARMOR: B CM", "STAT BOOSTERS",
         "TOOLS & JUNK", "KEY ITEMS", "TRADING CARDS"]

groups = {t: [] for t in ORDER}
techs = []
for name, val, comment in rows:
    v = int(val, 16)
    label = name.replace("_", " ")
    if v == 0:
        continue
    if v >= 0xEC:
        if "MITO" in name:          # unused dummy techs 0xF9-0xFD
            continue
        techs.append((v, "FULL RECOVER" if v == 0xFE else label))
        continue
    groups[item_type(v, comment or "")].append((v, label))

items, first, count = [], [], []
for t in ORDER:
    first.append(len(items)); count.append(len(groups[t])); items += groups[t]

def lst(ctype, name, values):
    return "\n".join([f"static const {ctype} {name}[] = {{"] +
                     [f"    {x}," for x in values] + ["};", ""])

with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("/*\n * GENERATED from parasite-eve-decomp include/pe1/item_ids.h (enum ItemId)\n"
            " * by tools/gen_pe_item_table.py -- do not edit by hand.\n *\n"
            " * Items: every non-empty ID below the Parasite Energy range (0x01-0xEB),\n"
            " * grouped by type; type t owns entries [pe_type_first[t], +pe_type_count[t]).\n"
            " * Techs: Parasite Energy abilities 0xEC-0xFF minus the unused dummies\n"
            " * 0xF9-0xFD; ability bit index = id - 0xEC.\n */\n#pragma once\n\n")
    f.write(lst("char *const", "pe_type_names", [f'"{t}"' for t in ORDER]))
    f.write(lst("unsigned short", "pe_type_first", first))
    f.write(lst("unsigned short", "pe_type_count", count))
    f.write(f"#define PE_TYPE_COUNT {len(ORDER)}u\n#define PE_TYPE_MAX_ITEMS {max(count)}u\n\n")
    f.write(lst("unsigned char", "pe_item_ids", [f"0x{v:02X}" for v, _ in items]))
    f.write(lst("char *const", "pe_item_names", [f'"{n}"' for _, n in items]))
    f.write(f"#define PE_ITEM_COUNT {len(items)}u\n\n")
    f.write(lst("unsigned char", "pe_tech_ids", [f"0x{v:02X}" for v, _ in techs]))
    f.write(lst("char *const", "pe_tech_names", [f'"{n}"' for _, n in techs]))
    f.write(f"#define PE_TECH_COUNT {len(techs)}u\n")
for t, c in zip(ORDER, count):
    print(f"{c:3d}  {t}")
print(len(items), "items,", len(techs), "techs")
