/* MiniDLNA media server
 *
 * Transliterated natural sort, the same order as Python's
 * natsort.natsorted(names, key=lambda t: unidecode(t).lower()).
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __NATSORT_H__
#define __NATSORT_H__

#include <stdint.h>

/* ASCII transliteration of one code point, as unidecode() gives it
 * ("" for code points it drops); in unidecode_table.c */
const char *unidecode_char(uint32_t cp);

/* unidecode(utf8).lower(): a malloc'ed ASCII string, invalid UTF-8
 * bytes dropped. NULL only if out of memory. */
char *translit_lower(const char *utf8);

/* Choose how sort keys transliterate (sort_transliteration): "pinyin" (the
 * default: Chinese by its context-aware pinyin from the dictionary at
 * pinyin_dict, the rest like unidecode), "icu" or "unidecode". Falls
 * back to unidecode when ICU or the dictionary is unavailable. Returns the
 * mode in use. */
const char *sort_keys_init(const char *mode, const char *pinyin_dict);

/* The sort key of a file name: transliterated per sort_keys_init(), in
 * lowercase. A malloc'ed copy; keys of non-ASCII names are cached, so each
 * is computed once. Main thread only. */
char *sort_key(const char *name);

/* Compare two such keys the way natsort does by default: runs of digits
 * compare as (arbitrarily large) unsigned integers, everything else as
 * ASCII text, and a key starting with a digit sorts before letters. */
int natcmp(const char *a, const char *b);

#endif
