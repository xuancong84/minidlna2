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
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef HAVE_ICU
#include <unicode/utrans.h>
#include <unicode/ustring.h>
#endif

#include "natsort.h"
#include "log.h"

/* Next code point of a UTF-8 string; invalid bytes give 0xFFFFFFFF and
 * are skipped one at a time, like the surrogate escapes Python uses for
 * them (which unidecode drops) */
static uint32_t
next_cp(const unsigned char **p)
{
	const unsigned char *s = *p;
	uint32_t cp;
	int n, i;

	if (s[0] < 0x80)
	{
		*p = s + 1;
		return s[0];
	}
	if ((s[0] & 0xE0) == 0xC0)
	{
		cp = s[0] & 0x1F;
		n = 1;
	}
	else if ((s[0] & 0xF0) == 0xE0)
	{
		cp = s[0] & 0x0F;
		n = 2;
	}
	else if ((s[0] & 0xF8) == 0xF0)
	{
		cp = s[0] & 0x07;
		n = 3;
	}
	else
		goto invalid;
	for (i = 1; i <= n; i++)
	{
		if ((s[i] & 0xC0) != 0x80)
			goto invalid;
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	/* Overlong forms, surrogates and values past U+10FFFF are invalid */
	if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
	    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
		goto invalid;
	*p = s + n + 1;
	return cp;

invalid:
	*p = s + 1;
	return 0xFFFFFFFF;
}

char *
translit_lower(const char *utf8)
{
	const unsigned char *p = (const unsigned char *)utf8;
	size_t len = 0, alloc = strlen(utf8) * 2 + 16;
	char *out = malloc(alloc), *grown;
	const char *t;
	uint32_t cp;

	if (!out)
		return NULL;
	while (*p)
	{
		cp = next_cp(&p);
		if (cp == 0xFFFFFFFF)
			continue;
		for (t = unidecode_char(cp); *t; t++)
		{
			if (len + 2 > alloc)
			{
				alloc *= 2;
				if (!(grown = realloc(out, alloc)))
				{
					free(out);
					return NULL;
				}
				out = grown;
			}
			out[len++] = (*t >= 'A' && *t <= 'Z') ? *t - 'A' + 'a' : *t;
		}
	}
	out[len] = '\0';

	return out;
}

static int
is_digit(char c)
{
	return c >= '0' && c <= '9';
}

int
natcmp(const char *a, const char *b)
{
	for (;;)
	{
		const char *ea = a, *eb = b;
		size_t la, lb;
		int c;

		/* Text: plain comparison, a prefix sorts first. A key that
		 * starts with a digit has an empty text run here, so it sorts
		 * before keys starting with anything else. */
		while (*ea && !is_digit(*ea))
			ea++;
		while (*eb && !is_digit(*eb))
			eb++;
		la = ea - a;
		lb = eb - b;
		c = memcmp(a, b, la < lb ? la : lb);
		if (c)
			return c < 0 ? -1 : 1;
		if (la != lb)
			return la < lb ? -1 : 1;
		a = ea;
		b = eb;
		if (!*a || !*b)
			return (*a != 0) - (*b != 0);

		/* Numbers: by value, any length; leading zeros don't count */
		while (*a == '0' && is_digit(a[1]))
			a++;
		while (*b == '0' && is_digit(b[1]))
			b++;
		for (ea = a; is_digit(*ea); ea++)
			;
		for (eb = b; is_digit(*eb); eb++)
			;
		la = ea - a;
		lb = eb - b;
		if (la != lb)
			return la < lb ? -1 : 1;
		c = memcmp(a, b, la);
		if (c)
			return c < 0 ? -1 : 1;
		a = ea;
		b = eb;
		if (!*a || !*b)
			return (*a != 0) - (*b != 0);
	}
}

/* Growable output string for the transliterators below */
struct out {
	char *s;
	size_t len, alloc;
};

static int
out_add(struct out *o, const char *s, size_t n)
{
	char *grown;
	size_t i;

	if (o->len + n + 1 > o->alloc)
	{
		size_t alloc = (o->len + n + 1) * 2;
		if (!(grown = realloc(o->s, alloc)))
			return -1;
		o->s = grown;
		o->alloc = alloc;
	}
	for (i = 0; i < n; i++)
		o->s[o->len++] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] - 'A' + 'a' : s[i];
	o->s[o->len] = '\0';

	return 0;
}

/* --- sort_transliteration=pinyin ---------------------------------------- */

struct py_char {
	uint32_t cp;
	const char *syllable;		/* "zhong " */
};

struct py_phrase {
	char *text;			/* UTF-8 */
	char *pinyin;			/* "chong qing " */
	size_t text_len;
};

static struct py_char *py_chars;
static size_t n_py_chars;
static struct py_phrase *py_table;	/* open addressing, power of two */
static size_t py_table_size;
static int py_maxlen;			/* characters in the longest phrase */

static unsigned int
fnv(const char *s, size_t n)
{
	unsigned int h = 2166136261u;

	while (n--)
		h = (h ^ (unsigned char)*s++) * 16777619u;

	return h;
}

static int
cmp_py_char(const void *a, const void *b)
{
	uint32_t x = ((const struct py_char *)a)->cp, y = ((const struct py_char *)b)->cp;

	return x < y ? -1 : x > y;
}

static const char *
py_char_syllable(uint32_t cp)
{
	struct py_char key = { cp, NULL }, *c;

	c = bsearch(&key, py_chars, n_py_chars, sizeof(*py_chars), cmp_py_char);

	return c ? c->syllable : NULL;
}

static const char *
py_phrase_lookup(const char *text, size_t len)
{
	size_t i = fnv(text, len) & (py_table_size - 1);

	for (; py_table[i].text; i = (i + 1) & (py_table_size - 1))
	{
		if (py_table[i].text_len == len && memcmp(py_table[i].text, text, len) == 0)
			return py_table[i].pinyin;
	}

	return NULL;
}

#ifdef HAVE_ZLIB
/* Lines are "<text> <syllable>...": one character with its usual reading,
 * or a phrase whose reading differs from its characters' usual ones */
static int
load_pinyin_dict(const char *path)
{
	struct py_phrase *phrases = NULL;
	size_t n_phrases = 0, alloc_chars = 0, alloc_phrases = 0, i;
	char line[1024];
	gzFile f = gzopen(path, "rb");

	if (!f)
		return -1;
	while (gzgets(f, line, sizeof(line)))
	{
		char *sp = strchr(line, ' '), *nl = strchr(line, '\n'), *syl, *p;
		const unsigned char *t;
		struct out pinyin = { NULL, 0, 0 };
		int chars = 0;
		uint32_t cp = 0;

		if (line[0] == '#' || !sp)
			continue;
		if (nl)
			*nl = '\0';
		*sp = '\0';
		for (t = (const unsigned char *)line; *t; chars++)
			cp = next_cp(&t);
		/* "chong qing" -> "chong qing ": a space after each, like unidecode */
		for (syl = strtok_r(sp + 1, " ", &p); syl; syl = strtok_r(NULL, " ", &p))
		{
			out_add(&pinyin, syl, strlen(syl));
			out_add(&pinyin, " ", 1);
		}
		if (!pinyin.s)
			continue;
		if (chars == 1)
		{
			if (n_py_chars == alloc_chars)
			{
				alloc_chars = alloc_chars ? alloc_chars * 2 : 32768;
				py_chars = realloc(py_chars, alloc_chars * sizeof(*py_chars));
			}
			py_chars[n_py_chars].cp = cp;
			py_chars[n_py_chars++].syllable = pinyin.s;
		}
		else
		{
			if (n_phrases == alloc_phrases)
			{
				alloc_phrases = alloc_phrases ? alloc_phrases * 2 : 4096;
				phrases = realloc(phrases, alloc_phrases * sizeof(*phrases));
			}
			phrases[n_phrases].text = strdup(line);
			phrases[n_phrases].text_len = strlen(line);
			phrases[n_phrases++].pinyin = pinyin.s;
			if (chars > py_maxlen)
				py_maxlen = chars;
		}
	}
	gzclose(f);
	if (!n_py_chars)
		return -1;
	qsort(py_chars, n_py_chars, sizeof(*py_chars), cmp_py_char);

	for (py_table_size = 16; py_table_size < n_phrases * 2; py_table_size *= 2)
		;
	py_table = calloc(py_table_size, sizeof(*py_table));
	for (i = 0; i < n_phrases; i++)
	{
		size_t j = fnv(phrases[i].text, phrases[i].text_len) & (py_table_size - 1);
		while (py_table[j].text)
			j = (j + 1) & (py_table_size - 1);
		py_table[j] = phrases[i];
	}
	free(phrases);

	return 0;
}
#endif

/* Chinese by the longest dictionary phrase at each position (or else each
 * character's usual reading), everything else like unidecode */
static char *
translit_pinyin(const char *utf8)
{
	const unsigned char *p = (const unsigned char *)utf8, *q;
	const unsigned char *ends[64];
	struct out o = { NULL, 0, 0 };
	const char *syl, *t;
	uint32_t cp;
	int n, k;

	out_add(&o, "", 0);
	while (*p)
	{
		q = p;
		cp = next_cp(&q);
		if (cp == 0xFFFFFFFF)
		{
			p = q;
			continue;
		}
		if (!(syl = py_char_syllable(cp)))
		{
			t = unidecode_char(cp);
			out_add(&o, t, strlen(t));
			p = q;
			continue;
		}
		/* Ends of the run of dictionary characters starting here */
		ends[n = 1] = q;
		while (n < py_maxlen && n < 63)
		{
			const unsigned char *r = ends[n];
			uint32_t c = next_cp(&r);
			if (c == 0xFFFFFFFF || !py_char_syllable(c))
				break;
			ends[++n] = r;
		}
		for (k = n; k >= 2; k--)
		{
			if ((t = py_phrase_lookup((const char *)p, ends[k] - p)))
				break;
		}
		if (k >= 2)
		{
			out_add(&o, t, strlen(t));
			p = ends[k];
		}
		else
		{
			out_add(&o, syl, strlen(syl));
			p = q;
		}
	}

	return o.s;
}

/* --- sort_transliteration=icu ------------------------------------------- */

#ifdef HAVE_ICU
static UTransliterator *icu;

static char *
translit_icu(const char *utf8)
{
	UErrorCode status = U_ZERO_ERROR;
	int32_t len, limit, cap = (int32_t)strlen(utf8) * 8 + 64, out_len;
	UChar *u = malloc(cap * sizeof(UChar));
	char *out = NULL;

	if (!u)
		return NULL;
	u_strFromUTF8WithSub(u, cap, &len, utf8, -1, 0xFFFD, NULL, &status);
	limit = len;
	if (U_SUCCESS(status))
		utrans_transUChars(icu, u, &len, cap, 0, &limit, &status);
	if (U_SUCCESS(status) && (out = malloc(len * 3 + 1)))
	{
		u_strToUTF8(out, len * 3 + 1, &out_len, u, len, &status);
		if (U_FAILURE(status))
		{
			free(out);
			out = NULL;
		}
		else
		{
			char *c;
			for (c = out; *c; c++)
				if (*c >= 'A' && *c <= 'Z')
					*c += 'a' - 'A';
		}
	}
	free(u);

	/* e.g. a pathological name that grew past the buffer */
	return out ? out : translit_lower(utf8);
}
#endif

/* --- mode selection and the key cache ------------------------------------ */

enum { MODE_UNIDECODE, MODE_ICU, MODE_PINYIN };
static int mode = MODE_UNIDECODE;

const char *
sort_keys_init(const char *name, const char *pinyin_dict)
{
	if (name && strcmp(name, "unidecode") == 0)
		mode = MODE_UNIDECODE;
	else if (name && strcmp(name, "icu") == 0)
	{
#ifdef HAVE_ICU
		UErrorCode status = U_ZERO_ERROR;
		/* Leaving ASCII to the filters (and lowercasing ourselves) is twice
		 * as fast as "Any-Latin; Latin-ASCII; Lower" */
		static const UChar id[] = u"[:^ASCII:] Any-Latin; [:^ASCII:] Latin-ASCII";

		icu = utrans_openU(id, -1, UTRANS_FORWARD, NULL, 0, NULL, &status);
		if (U_SUCCESS(status))
			mode = MODE_ICU;
		else
			DPRINTF(E_WARN, L_GENERAL, "Cannot open the ICU transliterator (%s); sorting with unidecode\n",
				u_errorName(status));
#else
		DPRINTF(E_WARN, L_GENERAL, "Built without ICU; sorting with unidecode\n");
#endif
	}
	else
	{
		/* pinyin, the default */
		if (name && strcmp(name, "pinyin") != 0)
			DPRINTF(E_WARN, L_GENERAL, "Unknown sort_transliteration '%s'; using pinyin\n", name);
#ifdef HAVE_ZLIB
		if (load_pinyin_dict(pinyin_dict) == 0)
		{
			DPRINTF(E_INFO, L_GENERAL, "Pinyin dictionary %s: %zu characters, phrases of up to %d\n",
				pinyin_dict, n_py_chars, py_maxlen);
			mode = MODE_PINYIN;
		}
		else
			DPRINTF(E_WARN, L_GENERAL, "Cannot read pinyin dictionary %s; sorting with unidecode\n",
				pinyin_dict);
#else
		DPRINTF(E_WARN, L_GENERAL, "Built without zlib; sorting with unidecode, not pinyin\n");
#endif
	}

	return mode == MODE_ICU ? "icu" : mode == MODE_PINYIN ? "pinyin" : "unidecode";
}

/* Keys of non-ASCII names, so that a name is transliterated only once; the
 * whole cache is dropped when it fills up */
#define KEY_CACHE_SIZE (1 << 17)

static struct {
	char *name;
	char *key;
} *key_cache;
static size_t key_cache_count;

static char *
make_key(const char *name)
{
	switch (mode)
	{
#ifdef HAVE_ICU
	case MODE_ICU:
		return translit_icu(name);
#endif
	case MODE_PINYIN:
		return translit_pinyin(name);
	default:
		return translit_lower(name);
	}
}

char *
sort_key(const char *name)
{
	const unsigned char *s;
	size_t len = strlen(name), i;
	char *key;

	/* ASCII transliterates to itself in every mode */
	for (s = (const unsigned char *)name; *s && *s < 0x80; s++)
		;
	if (!*s)
	{
		if (!(key = malloc(len + 1)))
			return NULL;
		for (i = 0; i <= len; i++)
			key[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] - 'A' + 'a' : name[i];
		return key;
	}

	if (!key_cache && !(key_cache = calloc(KEY_CACHE_SIZE, sizeof(*key_cache))))
		return make_key(name);
	i = fnv(name, len) & (KEY_CACHE_SIZE - 1);
	for (; key_cache[i].name; i = (i + 1) & (KEY_CACHE_SIZE - 1))
	{
		if (strcmp(key_cache[i].name, name) == 0)
			return strdup(key_cache[i].key);
	}

	if (!(key = make_key(name)))
		return NULL;
	if (key_cache_count >= KEY_CACHE_SIZE / 4 * 3)
	{
		for (i = 0; i < KEY_CACHE_SIZE; i++)
		{
			free(key_cache[i].name);
			free(key_cache[i].key);
		}
		memset(key_cache, 0, KEY_CACHE_SIZE * sizeof(*key_cache));
		key_cache_count = 0;
		i = fnv(name, len) & (KEY_CACHE_SIZE - 1);
	}
	if ((key_cache[i].name = strdup(name)) && (key_cache[i].key = strdup(key)))
		key_cache_count++;
	else
	{
		free(key_cache[i].name);
		key_cache[i].name = NULL;
	}

	return key;
}
