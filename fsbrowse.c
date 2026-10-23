/* microDLNA media server
 *
 * Live folder mode: serve directory listings straight from the filesystem,
 * without a media database.
 *
 * This file is part of microDLNA.
 *
 * microDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * microDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with microDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "upnpglobalvars.h"
#include "fsbrowse.h"
#include "utils.h"
#include "natsort.h"
#include "log.h"

static const struct {
	const char *ext;
	const char *mime;
} video_mimes[] = {
	{ "mpg", "video/mpeg" }, { "mpeg", "video/mpeg" }, { "vob", "video/mpeg" },
	{ "ts", "video/mpeg" }, { "mts", "video/mpeg" }, { "m2ts", "video/mpeg" },
	{ "m2t", "video/mpeg" },
	{ "avi", "video/x-msvideo" }, { "divx", "video/x-msvideo" }, { "xvid", "video/x-msvideo" },
	{ "asf", "video/x-ms-wmv" }, { "wmv", "video/x-ms-wmv" },
	{ "mp4", "video/mp4" }, { "m4v", "video/mp4" },
	{ "mkv", "video/x-matroska" }, { "webm", "video/x-matroska" },
	{ "flv", "video/x-flv" },
	{ "mov", "video/quicktime" },
	{ "3gp", "video/3gpp" },
	{ "rm", "application/vnd.rn-realmedia" }, { "rmvb", "application/vnd.rn-realmedia-vbr" },
	{ "TiVo", "video/x-tivo-mpeg" },
	{ NULL, NULL }
}, audio_mimes[] = {
	{ "mp3", "audio/mpeg" },
	{ "m4a", "audio/mp4" }, { "aac", "audio/mp4" }, { "mp4", "audio/mp4" }, { "m4p", "audio/mp4" },
	{ "3gp", "audio/3gpp" },
	{ "wma", "audio/x-ms-wma" }, { "asf", "audio/x-ms-wma" },
	{ "flac", "audio/x-flac" }, { "fla", "audio/x-flac" }, { "flc", "audio/x-flac" },
	{ "wav", "audio/x-wav" },
	{ "ogg", "audio/ogg" },
	{ "pcm", "audio/L16" },
	{ "dsf", "audio/x-dsd" }, { "dff", "audio/x-dsd" },
	{ "opus", "audio/ogg" }, { "mka", "audio/x-matroska" }, { "m4b", "audio/mp4" },
	{ NULL, NULL }
};

static struct media_dir_s *
media_dir_at(int idx)
{
	struct media_dir_s *media_dir;

	if (idx < 0)
		return NULL;
	for (media_dir = media_dirs; media_dir && idx; media_dir = media_dir->next)
		idx--;

	return media_dir;
}

static int
count_media_dirs(void)
{
	struct media_dir_s *media_dir;
	int n = 0;

	for (media_dir = media_dirs; media_dir; media_dir = media_dir->next)
		n++;

	return n;
}

/* Same rule as the scanner: several media_dirs get their own top-level
 * containers unless merge_media_dirs is set. */
int
fs_root_is_flat(void)
{
	return !media_dirs || !media_dirs->next || GETFLAG(MERGE_MEDIA_DIRS_MASK);
}

media_types
fs_media_types(int idx)
{
	struct media_dir_s *media_dir = media_dir_at(idx);

	return media_dir ? media_dir->types : 0;
}

/* Is this resolved path inside one of the media_dirs? */
static int
inside_media_dirs(const char *real)
{
	struct media_dir_s *media_dir;
	size_t len;

	for (media_dir = media_dirs; media_dir; media_dir = media_dir->next)
	{
		if (strcmp(media_dir->path, "/") == 0)
			return 1;
		len = strlen(media_dir->path);
		if (strncmp(real, media_dir->path, len) == 0 &&
		    (real[len] == '/' || real[len] == '\0'))
			return 1;
	}

	return 0;
}

static int
link_allowed(const char *path)
{
	char real[PATH_MAX];

	if (GETFLAG(WIDE_LINKS_MASK))
		return 1;
	if (!realpath(path, real))
		return 0;
	if (!inside_media_dirs(real))
	{
		DPRINTF(E_DEBUG, L_HTTP, "Rejecting wide link %s -> %s\n", path, real);
		return 0;
	}

	return 1;
}

static int
url_safe(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static size_t
encode_append(char *buf, size_t off, size_t len, const char *str)
{
	static const char hex[] = "0123456789ABCDEF";
	const unsigned char *p;

	for (p = (const unsigned char *)str; *p && off + 4 < len; p++)
	{
		if (url_safe(*p))
			buf[off++] = *p;
		else
		{
			buf[off++] = '%';
			buf[off++] = hex[*p >> 4];
			buf[off++] = hex[*p & 0xF];
		}
	}
	buf[off] = '\0';

	return off;
}

void
fs_make_id(char *buf, size_t len, int idx, const char *rel, const char *name)
{
	size_t off = snprintf(buf, len, "F%d", idx);

	if (rel && *rel)
	{
		buf[off++] = '/';
		off = encode_append(buf, off, len, rel);
	}
	if (name && *name)
	{
		buf[off++] = '/';
		encode_append(buf, off, len, name);
	}
}

void
fs_object_id(char *buf, size_t len, const struct fs_object *obj)
{
	if (obj->idx < 0)
		strncpyt(buf, "0", len);
	else
		fs_make_id(buf, len, obj->idx, obj->rel, NULL);
}

void
fs_parent_id(char *buf, size_t len, const struct fs_object *obj)
{
	char dir[PATH_MAX];
	char *p;

	if (obj->idx < 0)
		strncpyt(buf, "-1", len);
	else if (!obj->rel[0])
		strncpyt(buf, "0", len);
	else if (!(p = strrchr(obj->rel, '/')))
	{
		if (fs_root_is_flat())
			strncpyt(buf, "0", len);
		else
			fs_make_id(buf, len, obj->idx, NULL, NULL);
	}
	else
	{
		memcpy(dir, obj->rel, p - obj->rel);
		dir[p - obj->rel] = '\0';
		fs_make_id(buf, len, obj->idx, dir, NULL);
	}
}

/* Percent-decode a relative path and refuse anything that could step
 * outside the media_dir: empty, "." or ".." components and NUL bytes. */
static int
decode_rel(const char *in, char *out, size_t len)
{
	size_t n = 0;
	const char *comp;
	int c;

	while (*in)
	{
		c = (unsigned char)*in;
		if (c == '%')
		{
			if (hexval(in[1]) < 0 || hexval(in[2]) < 0)
				return -1;
			c = (hexval(in[1]) << 4) | hexval(in[2]);
			in += 3;
		}
		else
			in++;
		if (c == '\0' || n + 1 >= len)
			return -1;
		out[n++] = c;
	}
	out[n] = '\0';

	if (!n || out[0] == '/' || out[n-1] == '/' || strstr(out, "//"))
		return -1;
	for (comp = out; comp; comp = strchr(comp, '/') ? strchr(comp, '/') + 1 : NULL)
	{
		if (strncmp(comp, "./", 2) == 0 || strcmp(comp, ".") == 0 ||
		    strncmp(comp, "../", 3) == 0 || strcmp(comp, "..") == 0)
			return -1;
	}

	return 0;
}

int
fs_resolve(const char *id, struct fs_object *obj)
{
	struct media_dir_s *media_dir;
	char real[PATH_MAX];
	char *end;
	long idx;
	int ret;

	memset(obj, 0, sizeof(*obj));
	obj->idx = -1;
	if (!id)
		return -1;
	if (strcmp(id, "0") == 0)
		return 0;
	if (id[0] != 'F' || id[1] < '0' || id[1] > '9')
		return -1;

	idx = strtol(id + 1, &end, 10);
	media_dir = media_dir_at(idx);
	if (!media_dir)
		return -1;
	if (*end == '/')
	{
		if (decode_rel(end + 1, obj->rel, sizeof(obj->rel)) != 0)
			return -1;
	}
	else if (*end != '\0')
		return -1;

	if (!obj->rel[0])
		ret = snprintf(obj->path, sizeof(obj->path), "%s", media_dir->path);
	else if (strcmp(media_dir->path, "/") == 0)
		ret = snprintf(obj->path, sizeof(obj->path), "/%s", obj->rel);
	else
		ret = snprintf(obj->path, sizeof(obj->path), "%s/%s", media_dir->path, obj->rel);
	if (ret < 0 || (size_t)ret >= sizeof(obj->path))
		return -1;

	if (stat(obj->path, &obj->st) != 0)
		return -1;
	if (obj->rel[0] && !GETFLAG(WIDE_LINKS_MASK))
	{
		if (!realpath(obj->path, real) || !inside_media_dirs(real))
		{
			DPRINTF(E_WARN, L_HTTP, "Rejecting wide link %s\n", obj->path);
			return -1;
		}
	}
	obj->idx = idx;
	obj->types = media_dir->types;

	return 0;
}

/* Turn a URL token "<id>.<ext>[?query]" back into the object ID. */
int
fs_token_to_id(const char *token, char *id, size_t len)
{
	char *p;

	strncpyt(id, token, len);
	if ((p = strchr(id, '?')))
		*p = '\0';
	p = strrchr(id, '.');
	if (!p || strchr(p, '/'))
		return -1;
	*p = '\0';

	return 0;
}

const char *
fs_mime(const char *name, media_types types, const char **class)
{
	media_types mtype = get_media_type(name);
	const char *ext = strrchr(name, '.');
	const char *mime = NULL, *cls = NULL;
	int i;

	if (!ext)
		return NULL;
	ext++;

	if (mtype == TYPE_IMAGE && (types & TYPE_IMAGE))
	{
		static char image_mime[32];

		if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
			mime = "image/jpeg";
		else
		{
			/* image/png, image/webp, or whatever image_extensions adds */
			snprintf(image_mime, sizeof(image_mime), "image/%s", ext);
			for (i = 6; image_mime[i]; i++)
				image_mime[i] = tolower((unsigned char)image_mime[i]);
			mime = image_mime;
		}
		cls = "item.imageItem.photo";
	}
	else if (mtype == TYPE_VIDEO && (types & TYPE_VIDEO))
	{
		mime = "video/mpeg";
		for (i = 0; video_mimes[i].ext; i++)
		{
			if (strcasecmp(ext, video_mimes[i].ext) == 0)
			{
				mime = video_mimes[i].mime;
				break;
			}
		}
		cls = "item.videoItem";
	}
	/* Extensions such as .mp4 can be audio too; audio-only media_dirs
	 * get them as music, like the scanner's fallback. */
	if (!mime && (types & TYPE_AUDIO) && is_audio(name))
	{
		mime = "audio/mpeg";
		for (i = 0; audio_mimes[i].ext; i++)
		{
			if (strcasecmp(ext, audio_mimes[i].ext) == 0)
			{
				mime = audio_mimes[i].mime;
				break;
			}
		}
		cls = "item.audioItem.musicTrack";
	}
	if (class)
		*class = cls;

	return mime;
}

static int
add_entry(struct fs_listing *list, const char *name, int idx, int is_dir, int top)
{
	struct fs_entry *ents;

	if (list->count == list->alloc)
	{
		int alloc = list->alloc ? list->alloc * 2 : 256;
		ents = realloc(list->ents, alloc * sizeof(*ents));
		if (!ents)
			return -1;
		list->ents = ents;
		list->alloc = alloc;
	}
	ents = &list->ents[list->count];
	ents->name = strdup(name);
	ents->key = ents->name ? sort_key(name) : NULL;
	if (!ents->key)
	{
		free(ents->name);
		return -1;
	}
	ents->idx = idx;
	ents->is_dir = is_dir;
	ents->top = top;
	list->count++;

	return 0;
}

/* One readdir() pass. Only names and d_type are used, so no inode is read
 * except for symlinks and filesystems that don't fill in d_type. */
static int
scan_dir(struct fs_listing *list, int idx, const char *path)
{
	media_types types = fs_media_types(idx);
	char full[PATH_MAX];
	struct dirent *e;
	struct stat st;
	DIR *dir;
	int dfd, fd, is_dir;

	dfd = open(path, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	if (dfd < 0)
	{
		DPRINTF(E_WARN, L_HTTP, "Error opening directory %s [%s]\n", path, strerror(errno));
		return -1;
	}
	fd = dup(dfd);
	dir = fd >= 0 ? fdopendir(fd) : NULL;
	if (!dir)
	{
		if (fd >= 0)
			close(fd);
		close(dfd);
		return -1;
	}
	list->dfd[idx] = dfd;

	while ((e = readdir(dir)))
	{
		if (e->d_name[0] == '.')
			continue;
		switch (e->d_type)
		{
		case DT_DIR:
			is_dir = 1;
			break;
		case DT_REG:
			is_dir = 0;
			break;
		case DT_LNK:
		case DT_UNKNOWN:
			if (fstatat(dfd, e->d_name, &st, 0) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
				is_dir = 1;
			else if (S_ISREG(st.st_mode))
				is_dir = 0;
			else
				continue;
			if (e->d_type == DT_LNK)
			{
				snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
				if (!link_allowed(full))
					continue;
			}
			break;
		default:
			continue;
		}
		if (!is_dir)
		{
			if (is_album_art(e->d_name))
			{
				list->has_art = 1;
				if (get_media_type(e->d_name) == TYPE_IMAGE)
					continue;
			}
			if (!fs_mime(e->d_name, types, NULL))
				continue;
		}
		if (add_entry(list, e->d_name, idx, is_dir, 0) != 0)
			break;
	}
	closedir(dir);

	return 0;
}

static int
cmp_entries(const void *a, const void *b)
{
	const struct fs_entry *x = a, *y = b;
	int ret;

	/* Folders first, then the order of Python's
	 * natsorted(names, key=lambda t: unidecode(t).lower()) */
	if (x->is_dir != y->is_dir)
		return y->is_dir - x->is_dir;
	ret = natcmp(x->key, y->key);
	if (ret)
		return ret;
	ret = strcmp(x->name, y->name);
	if (ret)
		return ret;

	return x->idx - y->idx;
}

int
fs_list(const struct fs_object *obj, struct fs_listing *list)
{
	struct media_dir_s *media_dir;
	int i;

	memset(list, 0, sizeof(*list));
	list->ndirs = count_media_dirs();
	list->dfd = malloc((list->ndirs ? list->ndirs : 1) * sizeof(int));
	if (!list->dfd)
		return -1;
	for (i = 0; i < list->ndirs; i++)
		list->dfd[i] = -1;

	if (obj->idx < 0)
	{
		for (media_dir = media_dirs, i = 0; media_dir; media_dir = media_dir->next, i++)
		{
			if (fs_root_is_flat())
				scan_dir(list, i, media_dir->path);
			else
			{
				const char *base = strrchr(media_dir->path, '/');
				add_entry(list, (base && base[1]) ? base + 1 : media_dir->path, i, 1, 1);
			}
		}
		/* Folder art of one media_dir doesn't belong to the others */
		if (list->ndirs > 1)
			list->has_art = 0;
		if (!fs_root_is_flat())
			return 0;
	}
	else if (S_ISDIR(obj->st.st_mode))
	{
		if (scan_dir(list, obj->idx, obj->path) != 0)
			return -1;
	}

	if (list->count > 1)
		qsort(list->ents, list->count, sizeof(*list->ents), cmp_entries);

	return 0;
}

/* Absolute path of a listed entry */
int
fs_entry_path(const struct fs_object *obj, const struct fs_entry *e, char *buf, size_t len)
{
	struct media_dir_s *media_dir = media_dir_at(e->idx);
	const char *dir;
	int ret;

	if (!media_dir)
		return -1;
	if (e->top)
		ret = snprintf(buf, len, "%s", media_dir->path);
	else
	{
		dir = (obj->idx < 0) ? media_dir->path : obj->path;
		ret = snprintf(buf, len, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e->name);
	}

	return (ret < 0 || (size_t)ret >= len) ? -1 : 0;
}

void
fs_free_listing(struct fs_listing *list)
{
	int i;

	for (i = 0; i < list->count; i++)
	{
		free(list->ents[i].name);
		free(list->ents[i].key);
	}
	free(list->ents);
	if (list->dfd)
	{
		for (i = 0; i < list->ndirs; i++)
		{
			if (list->dfd[i] >= 0)
				close(list->dfd[i]);
		}
		free(list->dfd);
	}
	memset(list, 0, sizeof(*list));
}

static void
base_name_noext(char *buf, size_t len, const char *name)
{
	strncpyt(buf, name, len);
	strip_ext(buf);
}

int
fs_has_captions(int dfd, const char *name)
{
	char file[NAME_MAX + 8];

	base_name_noext(file, sizeof(file) - 4, name);
	strcat(file, ".srt");
	if (faccessat(dfd, file, R_OK, 0) == 0)
		return 1;
	strcpy(strrchr(file, '.'), ".smi");

	return faccessat(dfd, file, R_OK, 0) == 0;
}

/* File-specific cover art, as checked by check_for_album_file() */
int
fs_has_file_art(int dfd, const char *name)
{
	char file[NAME_MAX + 16];

	snprintf(file, sizeof(file), "%s.cover.jpg", name);
	if (faccessat(dfd, file, R_OK, 0) == 0)
		return 1;
	file[0] = '.';
	base_name_noext(file + 1, sizeof(file) - 6, name);
	strcat(file, ".jpg");
	if (faccessat(dfd, file + 1, R_OK, 0) == 0)
		return 1;

	return faccessat(dfd, file, R_OK, 0) == 0;
}

int
fs_find_caption(const char *path, char *out, size_t len)
{
	char *p;

	if (strlen(path) + 4 >= len)
		return -1;
	strncpyt(out, path, len);
	p = strrchr(out, '.');
	if (!p || strchr(p, '/'))
		return -1;
	strcpy(p, ".srt");
	if (access(out, R_OK) == 0)
		return 0;
	strcpy(p, ".smi");

	return access(out, R_OK) == 0 ? 0 : -1;
}

/* Find the cover art for a file or folder at request time. */
int
fs_find_art(const char *path, int is_dir, char *out, size_t len)
{
	struct album_art_name_s *art_name;
	char dir[PATH_MAX];
	char *p, *name;
	int wildcards = 0;

	if (strlen(path) + 12 >= len || strlen(path) >= sizeof(dir))
		return -1;
	strncpyt(dir, path, sizeof(dir));
	if (!is_dir)
	{
		p = strrchr(dir, '/');
		if (!p)
			return -1;
		*p = '\0';
		name = p + 1;

		snprintf(out, len, "%s/%s.cover.jpg", dir, name);
		if (access(out, R_OK) == 0)
			return 0;
		snprintf(out, len, "%s/%s", dir, name);
		p = strrchr(out, '/');
		if (strrchr(p, '.'))
			strcpy(strrchr(p, '.'), ".jpg");
		else
			strcat(p, ".jpg");
		if (access(out, R_OK) == 0)
			return 0;
		memmove(p + 2, p + 1, strlen(p + 1) + 1);
		p[1] = '.';
		if (access(out, R_OK) == 0)
			return 0;
	}

	for (art_name = album_art_names; art_name; art_name = art_name->next)
	{
		if (art_name->wildcard)
		{
			wildcards = 1;
			continue;
		}
		snprintf(out, len, "%s/%s", dir, art_name->name);
		if (access(out, R_OK) == 0)
			return 0;
	}
	if (wildcards)
	{
		DIR *d = opendir(dir);
		struct dirent *e;
		int found = 0;

		while (d && !found && (e = readdir(d)))
		{
			for (art_name = album_art_names; art_name; art_name = art_name->next)
			{
				if (art_name->wildcard &&
				    strncmp(art_name->name, e->d_name, strlen(art_name->name)) == 0)
				{
					snprintf(out, len, "%s/%s", dir, e->d_name);
					found = 1;
					break;
				}
			}
		}
		if (d)
			closedir(d);
		if (found)
			return 0;
	}

	return -1;
}
