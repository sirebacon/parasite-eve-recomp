/* The play log (pe_loot_logger.c): one CSV row per event, with the game's
 * context (room, play time, level, EXP, HP, Junk, Wayne's count) filled in.
 * `kind` names the event; the printf-style detail goes in the last column as
 * key=value pairs (commas become semicolons). Does nothing while the logger
 * is switched off. */
#pragma once

void pe_log(const char *kind, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* pe_mod_menu.c: one "settings" row listing every F1 row not at its default. */
void pe_menu_log_settings(void);
