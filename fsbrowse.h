/* MiniDLNA media server
 *
 * Live folder mode: serve directory listings straight from the filesystem,
 * without a media database.
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
#ifndef __FSBROWSE_H__
#define __FSBROWSE_H__

#include <limits.h>
#include <sys/stat.h>
#include "upnpglobalvars.h"

/* An empty db_dir selects live folder mode: no database is built and
 * every Browse request is answered from a fresh readdir(). */
static inline int
fs_mode(void)
{
	return db_path[0] == '\0';
}

/* Object IDs in live folder mode:
 *   "0"               root container
 *   "F<i>"            top of the i'th media_dir (config order)
 *   "F<i>/<rel/path>" a folder or file below it, percent-encoded
 * The same string is used as the token in /MediaItems/, /Captions/ and
 * /AlbumArt/ URLs, so it only ever contains URL- and XML-safe characters. */
#define FS_ID_MAX (PATH_MAX * 3 + 16)

struct fs_object {
	int idx;                /* media_dir index, -1 for the root container */
	media_types types;      /* media types allowed in that media_dir */
	char path[PATH_MAX];    /* absolute filesystem path */
	char rel[PATH_MAX];     /* path below the media_dir, "" for its top */
	struct stat st;
};

struct fs_entry {
	char *name;             /* file name, or media_dir basename for tops */
	char *key;              /* sort key: the name transliterated to lowercase ASCII */
	short idx;              /* media_dir index */
	unsigned char is_dir;
	unsigned char top;      /* entry is a media_dir itself ("F<i>") */
};

struct fs_listing {
	struct fs_entry *ents;
	int count;
	int alloc;
	int *dfd;               /* open directory fd per media_dir index, or -1 */
	int ndirs;
	int has_art;            /* the listed directory has folder album art */
};

int fs_root_is_flat(void);
int fs_resolve(const char *id, struct fs_object *obj);
int fs_list(const struct fs_object *obj, struct fs_listing *list);
int fs_entry_path(const struct fs_object *obj, const struct fs_entry *e, char *buf, size_t len);
void fs_free_listing(struct fs_listing *list);

void fs_make_id(char *buf, size_t len, int idx, const char *rel, const char *name);
void fs_object_id(char *buf, size_t len, const struct fs_object *obj);
void fs_parent_id(char *buf, size_t len, const struct fs_object *obj);
int fs_token_to_id(const char *token, char *id, size_t len);

const char *fs_mime(const char *name, media_types types, const char **class);
media_types fs_media_types(int idx);
int fs_has_captions(int dfd, const char *name);
int fs_has_file_art(int dfd, const char *name);
int fs_find_caption(const char *path, char *out, size_t len);
int fs_find_art(const char *path, int is_dir, char *out, size_t len);

#endif
