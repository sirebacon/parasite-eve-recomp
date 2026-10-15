# Play log (`pe_play_log.csv`)

The PE Enhancements mods write a play log while you play. It records battles,
loot, the economy, combat, movement between maps, and every change made in
the F1 menu. It exists to balance the loot overhaul against real playthroughs,
to chase bugs, and to hand real combat data to anyone building a combat mod.

- **Where:** `pe_play_log.csv` next to the game exe (`build-release/`).
- **On/off:** F1 > PE Enhancements > Debug > **Play log** (on by default).
  **Combat detail** on the same page adds one row per combat event (on by
  default for the data-collection run; turn it off for a smaller file).
- **Code:** `src/mods/pe_loot_logger.c` (most rows), `pe_log()` in
  `src/mods/pe_log.h` (rows other modules add), the menu wrappers in
  `src/mods/pe_mod_menu.c` (F1 changes).
- Rows are flushed as they are written, so a crash loses nothing.
- If the file's header doesn't match the current version, it is renamed to
  `pe_play_log.<unix time>.csv` and a new file is started.

## Columns

| Column | Meaning |
|---|---|
| `kind` | the event (tables below) |
| `battle` | battle number this launch, while in a battle; empty in the field |
| `day` | the engine's story day, `g_GameState.current_story_day` (0x800B0CE4). 1-8; -1 before the first scene. Which value is which in-game day is still to be confirmed from logs |
| `map` | map number decoded from the scene token (`g_SceneDispatchToken` 0x8009D280, as `Save_GetCurrentMapNumber`). The decomp's `docs/ROOMS.md` lists maps in story order (1-34 theater/sewer Day 1, 57-89 Central Park, ..., 288+ Chrysler) |
| `scene` | the 6-character scene name the map number comes from |
| `play_s` | play time in seconds (the save clock, `GAME_TIME_COUNTER(2)` / 60). Goes backwards after a load, which marks rewound play |
| `level` | Aya's level (1-based) |
| `exp` | total EXP |
| `hp`, `max_hp` | Aya's HP from the save-side mirror (in battle, see `battle` rows for live values) |
| `junk` | the loot overhaul's Junk counter |
| `wayne` | Wayne's hand-in count (0x800A78B0) |
| `detail` | `key=value` pairs, space separated; lists inside a value are `/` separated. Commas never appear (they become `;`) |

Item ids are the game's (`include/pe1/item_ids.h` in the decomp): e.g. 0x06-0x09
Medicine 1-4, 0x0D-0x10 Cures, 0x0B/0x0C Super/Duper Junk, 0x14 Junk,
0x16/0x17 Tool/Super Tool, 0x18 Smoke Grenade and 0x19 Lucky Charm (mod
items), 0x37 Mod Permit, 0xEC-0xFF PE techniques. Gear in the bag is stored as
0x100 + equipment record; rows print the item id of the record and say `gear`.

## Rows

### Session, loads and progress

| kind | when | detail |
|---|---|---|
| `session` | first row of each launch | `start` local date and time |
| `settings` | first frame of play, and after every load | every F1 setting not at its default as `Page/Label=value` (or `all defaults`). Check this before trusting balance numbers: God mode, No random battles, speeds, tuning sliders |
| `newgame` / `cardload` / `stateload` | the frame after a new game, a memory card load, a save state load | (context columns are the loaded game's) |
| `save` | a memory card save | `bag=used/limit bp=` bonus points, `gun=` / `armor=` id `+a/b/c` (the three + bonuses) and `mods=` effect numbers per slot |
| `levelup` | Aya's level rises | `to` |
| `room` | the map changes | `from=map/scene`, then the snapshot (below). The row's own map/scene columns are the new map |

### Battles

| kind | when | detail |
|---|---|---|
| `pe_turn` | a turn that cast a Parasite Energy power ends (half-bar turns on; pe_atb_items.c) | `power cost gauge=before->after` (cost: half = quick power, full = full bar, full_debt = full power from a half bar, the rest owed from the next refill) |
| `modfx` | a battle ends with the Mod Effects rules on (pe_mod_effects.c) | `rules hits elem=n(neutral/weak/resist) rof_boost status_on status_off crit_up quickdraw poison_scaled hp_refunded` (what the rebalance changed in that battle) |
| `enemy` | an enemy's slot is freed | `slot id hp def exp bp=base/factor/hits drop steal boss died heat frost cyanide acid tranq steal_r poison charge` (boss = status flags 2 & 0x40000, can't escape; resist values 0 neutral / 50%, 1 resists / immune, 2 weak / always; poison = HP lost per poison tick; charge = ATB charge per tick, full at 9000) |
| `battle` | a battle ends | see below |
| `loot` | a won battle pays out (loot overhaul) | `exp junk` (earned this battle) `hits` (hits Aya took) `vanilla` (game's own Junk) `steal` (empty-steal Junk) `shown` (the "Junk xN" line) `boss preset scav` (Scavenger) `rare_dry` (rare meter before the roll) `rolled` (0 none; 0x8000+n = economy mod n) `carried` (rare held over) `charm` (Lucky Charm battles left) `path` (the cascade: `-name` skipped, `+name` the drop, `!` = by guarantee; `-` = no rare) |
| `junk_take` / `junk_left` | the items-found "Junk xN" line taken / left behind (still paid, `paid=1`) | `n`, `paid` |
| `rare_take` / `rare_left` | a rare drop taken / left | `item` |
| `econ_unlock` / `econ_left` | an economy mod unlocked / left | `mod source=drop|bought` |
| `charm_on` | a Lucky Charm starts | `battles` |
| `steal` | a steal got past the coin flip | `item` (0 = nothing left) |

The `battle` row:

- `frames secs exp bp` and `outcome` (win / escape / loss), `hp=before->after`,
  `gun armor` (equipped ids), `boss`, `rewards` (the game's items-found list,
  id x amount).
- **Combat:** `turns` (Aya's turn gauge emptying; full is 9000), `acts=id:count`
  (the queued action id per turn, raw: the decomp names 6 fire-weapon and 8
  item/special, others unconfirmed), `pe=start->end spent regained` (PE, the
  Combatant's +0x08 >> 16; units as stored), `casts=Name:count` (PE
  techniques, from `Battle_ApplySpellEffect`), `dealt` (enemy HP lost),
  `taken` / `healed` (Aya's live HP), `hp_min`, `kills`, `first_kill`
  (seconds), `shots` (loaded rounds spent), `reloads`, `wswaps` (weapon
  changes), `used=id:count` (bag items that went down during the fight).
- Then the snapshot.

### Combat detail (only with the Debug toggle on)

| kind | detail |
|---|---|
| `turn` | `n act t pe loaded` |
| `hit` | `slot dmg left t` (an enemy losing HP) |
| `kill` | `slot t` |
| `hurt` / `heal` | `dmg` / `hp`, `t` |
| `spell` | `tech t` |
| `reload` | `loaded t` |
| `weapon` | `gun t` |

`t` is seconds since the battle started.

### Economy and items

| kind | when | detail |
|---|---|---|
| `buy` | Requisition purchase | `item price` |
| `spare_club` | the soft-lock rescue Club | |
| `install` / `remove` | Mod Catalog | `gun|armor slot effect was price tool` / `slot effect` |
| `dismantle` | gear dismantled (Discard, F1, or a Tool's donor) | `item gun|armor plus=a/b/c mods new payout` |
| `rebuild` | Rebuild bought | `item price` |
| `combine` | two Medicines combined | `from into` |
| `wayne` | Wayne's count moved (a hand-in, a craft's -300) | `from to` |
| `bundle` | a Super Junk / Duper Junk turned into Junk (75 / 150) | `item junk` |
| `wayne_refund` | a Super/Duper Junk craft refunded 300 (only with the Junk economy off) | `back` |
| `discard` | the game's Discard confirmed | `item list=bag|storage` |
| `store` / `withdraw` | an item moved bag -> Wayne's storage / back | `item` |
| `pickup` | field count change of Junk / Tools / kits / cards / Mod Permit | `item delta` |
| `add` | the game adding an item (chests, gifts, crafts) | `item bagfree` (free bag slots at that moment; 0 = it may not have fit) |
| `invcmd` | a script inventory command | `cmd value other` |
| `itemcheck` | a script asking how many of an item Aya has (door / event checks) | `item count list=bag|storage|other` |

### Snapshot (on `battle` and `room` rows)

`bag=id:count/...` and `st=id:count/...` (Wayne's storage) for items below
0x100 (consumables, Junk, Tools, keys), `ammo=a/b/c` (bullets, rockets,
Maeda's pools) and `loaded` (rounds in the equipped gun). Consumption is the
difference between two snapshots.

### F1 menu

| kind | detail |
|---|---|
| `tweak` | `Page/Label old->new`: any toggle, number or choice changed, a live row's value set (Item Editor), a Stats edit applied. Debug selectors (Item type, Item) show up here too, which tells what a following `Give item` gave |
| `action` | `Page/Label`: a button pressed; `(accept)` for a grid's accept (Buy, Install on the F1 pages) |

Settings the game itself changes (a save turning the Junk economy off on
load) also show as `tweak`.

## Reading it

- Split by `day` (or by `map` with ROOMS.md) for per-day numbers; use
  `play_s` for per-hour rates.
- After a `stateload` / `cardload`, rows whose `play_s` is later than the
  loaded value were rewound: drop them for balance numbers.
- `settings` rows say whether a stretch of play was vanilla-like or had
  helpers on.
- Battle numbers restart each launch; `session` rows separate launches.

## Known limits

- `day` values aren't yet tied to in-game days; `acts` ids beyond 6 and 8 are
  unconfirmed; PE is logged in its stored units.
- Escape attempts that fail aren't seen (only successful escapes).
- Enemy attacks aren't broken down by attack; `taken` is the total.
