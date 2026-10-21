/* MiniDLNA media server
 *
 * Live folder mode: metadata learned on demand (probed files, played files,
 * opened folders), kept in memory only.
 *
 * All probing runs in one worker thread: the metadata extractors keep
 * static state and are not thread safe. A browse that wants results now
 * queues probes and waits for them, at most probe_time milliseconds.
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
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef THUMBNAIL_CREATION
#include <libffmpegthumbnailer/videothumbnailerc.h>
#endif

#include "libav.h"
#include "upnpglobalvars.h"
#include "image_utils.h"
#include "utils.h"
#include "fsmeta.h"
#include "log.h"

#define HASH_SIZE	65536
#define MAX_ENTRIES	50000		/* files and folders remembered */
#define MAX_THUMB_BYTES	(64 << 20)	/* memory for thumbnails */
#define MAX_QUEUE	2000		/* pending jobs per queue */
#define THUMB_DIM	160

struct entry {
	struct entry *hnext;		/* hash chain */
	struct entry *prev, *next;	/* LRU list, most recent first */
	char *path;
	off_t size;
	time_t mtime;
	int is_dir;
	int count;			/* folders: number of entries, -1 unknown */
	int has_art;			/* folders: contain a cover file */
	int probed;			/* files: 1 probed, -1 failed, 0 not yet */
	int thumb_tried;
	struct media_info info;		/* never holds art; see thumb */
	uint8_t *thumb;
	int thumb_size;
};

struct job {
	struct job *next;
	char *path;
	media_types types;
	unsigned long ticket;
};

struct queue {
	struct job *head, *tail;
	int len;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static struct entry *table[HASH_SIZE];
static struct entry *lru_head, *lru_tail;
static int n_entries;
static long thumb_bytes;
static struct queue probe_q, thumb_q;
static unsigned long next_ticket, done_ticket;
static int running, stopping;
static pthread_t worker_id;

static unsigned int
hash_path(const char *s)
{
	unsigned int h = 2166136261u;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;

	return h % HASH_SIZE;
}

static void
lru_unlink(struct entry *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		lru_head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		lru_tail = e->prev;
	e->prev = e->next = NULL;
}

static void
lru_push(struct entry *e)
{
	e->next = lru_head;
	if (lru_head)
		lru_head->prev = e;
	lru_head = e;
	if (!lru_tail)
		lru_tail = e;
}

static void
drop_file_data(struct entry *e)
{
	free_media_info(&e->info);
	e->probed = 0;
	free(e->thumb);
	thumb_bytes -= e->thumb_size;
	e->thumb = NULL;
	e->thumb_size = 0;
	e->thumb_tried = 0;
}

static void
remove_entry(struct entry *e)
{
	struct entry **p = &table[hash_path(e->path)];

	while (*p && *p != e)
		p = &(*p)->hnext;
	if (*p)
		*p = e->hnext;
	lru_unlink(e);
	drop_file_data(e);
	free(e->path);
	free(e);
	n_entries--;
}

/* Forget the least recently used entries, but never keep */
static void
evict(struct entry *keep)
{
	while ((n_entries > MAX_ENTRIES || thumb_bytes > MAX_THUMB_BYTES) &&
	       lru_tail && lru_tail != keep)
		remove_entry(lru_tail);
}

static struct entry *
find(const char *path, int create)
{
	unsigned int h = hash_path(path);
	struct entry *e;

	for (e = table[h]; e; e = e->hnext)
	{
		if (strcmp(e->path, path) == 0)
		{
			lru_unlink(e);
			lru_push(e);
			return e;
		}
	}
	if (!create)
		return NULL;

	e = calloc(1, sizeof(*e));
	if (!e || !(e->path = strdup(path)))
	{
		free(e);
		return NULL;
	}
	e->count = -1;
	e->hnext = table[h];
	table[h] = e;
	lru_push(e);
	n_entries++;
	evict(e);

	return e;
}

static int
fresh(const struct entry *e, const struct stat *st)
{
	return !e->is_dir && e->size == st->st_size && e->mtime == st->st_mtime;
}

static char *
dupstr(const char *s)
{
	return s ? strdup(s) : NULL;
}

static void
copy_info(struct media_info *dst, const struct media_info *src)
{
	*dst = *src;
	dst->title = dupstr(src->title);
	dst->creator = dupstr(src->creator);
	dst->artist = dupstr(src->artist);
	dst->album = dupstr(src->album);
	dst->genre = dupstr(src->genre);
	dst->comment = dupstr(src->comment);
	dst->date = dupstr(src->date);
	dst->duration = dupstr(src->duration);
	dst->resolution = dupstr(src->resolution);
	dst->dlna_pn = dupstr(src->dlna_pn);
	dst->mime = dupstr(src->mime);
	dst->art = NULL;
	dst->art_size = 0;
}

/* Video frames need ffmpegthumbnailer and enable_thumbnail */
static int
can_make_video_thumb(const char *path, media_types types)
{
#ifdef THUMBNAIL_CREATION
	return GETFLAG(THUMB_MASK) && (types & TYPE_VIDEO) && is_video(path);
#else
	(void)path;
	(void)types;
	return 0;
#endif
}

static struct job *
queued(struct queue *q, const char *path)
{
	struct job *j;

	for (j = q->head; j; j = j->next)
	{
		if (strcmp(j->path, path) == 0)
			return j;
	}

	return NULL;
}

static struct job *
push(struct queue *q, const char *path, media_types types, unsigned long ticket)
{
	struct job *j;

	if (q->len >= MAX_QUEUE || !(j = calloc(1, sizeof(*j))))
		return NULL;
	if (!(j->path = strdup(path)))
	{
		free(j);
		return NULL;
	}
	j->types = types;
	j->ticket = ticket;
	if (q->tail)
		q->tail->next = j;
	else
		q->head = j;
	q->tail = j;
	q->len++;

	return j;
}

/* Put a job first in its queue, queued already or not */
static void
move_to_front(struct queue *q, struct job *j)
{
	struct job **p = &q->head, *prev = NULL;

	while (*p && *p != j)
	{
		prev = *p;
		p = &(*p)->next;
	}
	if (!*p || q->head == j)
		return;
	*p = j->next;
	if (q->tail == j)
		q->tail = prev;
	j->next = q->head;
	q->head = j;
}

static struct job *
pop(struct queue *q)
{
	struct job *j = q->head;

	if (j)
	{
		q->head = j->next;
		if (!q->head)
			q->tail = NULL;
		q->len--;
	}

	return j;
}

unsigned long
fsmeta_request(const char *path, const struct stat *st, media_types types, int what)
{
	unsigned long ticket = 0;
	struct entry *e;
	struct job *j;
	int ok;

	if (!running)
		return 0;

	pthread_mutex_lock(&lock);
	e = find(path, 0);
	ok = e && fresh(e, st);
	if ((what & FSMETA_PROBE) && !(ok && e->probed))
	{
		if ((j = queued(&probe_q, path)))
			ticket = j->ticket;
		else if ((j = push(&probe_q, path, types, next_ticket + 1)))
			ticket = ++next_ticket;
		/* A played or selected file shouldn't wait behind a folder's worth */
		if (j && (what & FSMETA_URGENT))
			move_to_front(&probe_q, j);
	}
	if ((what & FSMETA_THUMB) && can_make_video_thumb(path, types) &&
	    !(ok && (e->thumb || e->thumb_tried)) && !queued(&thumb_q, path))
		push(&thumb_q, path, types, 0);
	pthread_cond_signal(&work_cv);
	pthread_mutex_unlock(&lock);

	return ticket;
}

long long
fsmeta_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void
fsmeta_wait(unsigned long ticket, long long deadline_ms)
{
	struct timespec ts;

	ts.tv_sec = deadline_ms / 1000;
	ts.tv_nsec = (deadline_ms % 1000) * 1000000;
	pthread_mutex_lock(&lock);
	while (running && done_ticket < ticket)
	{
		if (pthread_cond_timedwait(&done_cv, &lock, &ts) == ETIMEDOUT)
			break;
	}
	pthread_mutex_unlock(&lock);
}

int
fsmeta_get(const char *path, const struct stat *st, struct media_info *info, int *thumb)
{
	struct entry *e;
	int ret = 0;

	if (thumb)
		*thumb = 0;
	pthread_mutex_lock(&lock);
	e = find(path, 0);
	if (e && fresh(e, st))
	{
		ret = e->probed;
		if (ret == 1 && info)
			copy_info(info, &e->info);
		if (thumb)
			*thumb = (e->thumb != NULL);
	}
	pthread_mutex_unlock(&lock);

	return ret;
}

int
fsmeta_get_thumb(const char *path, const struct stat *st, uint8_t **data, int *size)
{
	struct entry *e;
	int ret = 0;

	pthread_mutex_lock(&lock);
	e = find(path, 0);
	if (e && fresh(e, st) && e->thumb && (*data = malloc(e->thumb_size)))
	{
		memcpy(*data, e->thumb, e->thumb_size);
		*size = e->thumb_size;
		ret = 1;
	}
	pthread_mutex_unlock(&lock);

	return ret;
}

void
fsmeta_set_count(const char *path, time_t mtime, int count, int has_art)
{
	struct entry *e;

	if (!running)
		return;
	pthread_mutex_lock(&lock);
	if ((e = find(path, 1)))
	{
		if (!e->is_dir)
			drop_file_data(e);
		e->is_dir = 1;
		e->mtime = mtime;
		e->count = count;
		e->has_art = has_art;
	}
	pthread_mutex_unlock(&lock);
}

int
fsmeta_get_count(const char *path, time_t mtime, int *has_art)
{
	struct entry *e;
	int count = -1;

	if (has_art)
		*has_art = 0;
	pthread_mutex_lock(&lock);
	e = find(path, 0);
	if (e && e->is_dir && e->mtime == mtime)
	{
		count = e->count;
		if (has_art)
			*has_art = e->has_art;
	}
	pthread_mutex_unlock(&lock);

	return count;
}

int
fsmeta_has_count(const char *path)
{
	struct entry *e;
	int ret;

	pthread_mutex_lock(&lock);
	e = find(path, 0);
	ret = e && e->is_dir && e->count >= 0;
	pthread_mutex_unlock(&lock);

	return ret;
}

/* A JPEG no larger than THUMB_DIM, from embedded cover art */
static uint8_t *
shrink_jpeg(const uint8_t *data, int size, int *out_size)
{
	image_s *src, *dst;
	uint8_t *out = NULL;
	int w, h;

	if (size < 3 || data[0] != 0xFF || data[1] != 0xD8)
		return NULL;
	src = image_new_from_jpeg(NULL, 0, data, size, 1, ROTATE_NONE);
	if (!src)
		return NULL;
	if (src->width <= THUMB_DIM && src->height <= THUMB_DIM)
	{
		if ((out = malloc(size)))
		{
			memcpy(out, data, size);
			*out_size = size;
		}
	}
	else
	{
		if (src->width >= src->height)
		{
			w = THUMB_DIM;
			h = (int)((long long)src->height * THUMB_DIM / src->width);
		}
		else
		{
			h = THUMB_DIM;
			w = (int)((long long)src->width * THUMB_DIM / src->height);
		}
		dst = image_resize(src, w ? w : 1, h ? h : 1);
		if (dst)
		{
			out = image_save_to_jpeg_buf(dst, out_size);
			image_free(dst);
		}
	}
	image_free(src);

	return out;
}

#ifdef THUMBNAIL_CREATION
static uint8_t *
video_thumb(const char *path, int *out_size)
{
	video_thumbnailer *vt = video_thumbnailer_create();
	image_data *img = video_thumbnailer_create_image_data();
	uint8_t *out = NULL;

	if (vt && img)
	{
		vt->thumbnail_image_type = Jpeg;
		vt->thumbnail_image_quality = runtime_vars.thumb_quality;
		vt->seek_percentage = 20;
		vt->overlay_film_strip = GETFLAG(THUMB_FILMSTRIP) ? 1 : 0;
		video_thumbnailer_set_size(vt, runtime_vars.thumb_width, 0);
		DPRINTF(E_DEBUG, L_METADATA, "generating thumbnail: %s\n", path);
		if (video_thumbnailer_generate_thumbnail_to_buffer(vt, path, img) == 0 &&
		    img->image_data_size > 2 && img->image_data_ptr[0] == 0xFF &&
		    img->image_data_ptr[1] == 0xD8 && (out = malloc(img->image_data_size)))
		{
			memcpy(out, img->image_data_ptr, img->image_data_size);
			*out_size = img->image_data_size;
		}
	}
	if (img)
		video_thumbnailer_destroy_image_data(img);
	if (vt)
		video_thumbnailer_destroy(vt);

	return out;
}
#endif

static int
probe(const char *path, media_types types, struct media_info *info)
{
	const char *name = strrchr(path, '/') + 1;
	media_types mtype = get_media_type(name);
	char *esc = escape_tag(name, 1);
	int ok = 0;

	if (!esc)
		return 0;
	if (mtype == TYPE_IMAGE && (types & TYPE_IMAGE))
		ok = ProbeImageMetadata(path, esc, info);
	else
	{
		if (mtype == TYPE_VIDEO && (types & TYPE_VIDEO))
			ok = ProbeVideoMetadata(path, esc, info);
		/* Same fallback as the scanner, e.g. an audio-only .mp4 */
		if (!ok && (types & TYPE_AUDIO) && is_audio(name))
		{
			free_media_info(info);
			ok = ProbeAudioMetadata(path, esc, info);
		}
	}
	if (!ok)
		free_media_info(info);
	free(esc);

	return ok;
}

/* Store a thumbnail, unless the file changed meanwhile */
static void
store_thumb(const char *path, const struct stat *st, uint8_t *data, int size)
{
	struct entry *e;

	pthread_mutex_lock(&lock);
	e = find(path, 0);
	if (e && fresh(e, st))
	{
		e->thumb_tried = 1;
		if (data && !e->thumb)
		{
			e->thumb = data;
			e->thumb_size = size;
			thumb_bytes += size;
			data = NULL;
			evict(e);
		}
	}
	pthread_mutex_unlock(&lock);
	free(data);
}

static void
process(const struct job *j, int thumb_job)
{
	struct media_info info;
	struct entry *e;
	struct stat st;
	uint8_t *art = NULL, *thumb = NULL;
	int art_size = 0, size = 0, need_probe, have_thumb;

	if (stat(j->path, &st) != 0)
		return;

	pthread_mutex_lock(&lock);
	e = find(j->path, 0);
	need_probe = !(e && fresh(e, &st) && e->probed);
	have_thumb = e && fresh(e, &st) && (e->thumb || e->thumb_tried);
	pthread_mutex_unlock(&lock);

	if (need_probe)
	{
		int ok;

		memset(&info, 0, sizeof(info));
		ok = probe(j->path, j->types, &info);
		/* Embedded cover art becomes the thumbnail */
		art = info.art;
		art_size = info.art_size;
		info.art = NULL;
		info.art_size = 0;

		pthread_mutex_lock(&lock);
		if ((e = find(j->path, 1)))
		{
			if (!fresh(e, &st))
			{
				drop_file_data(e);
				e->is_dir = 0;
				e->count = -1;
				e->size = st.st_size;
				e->mtime = st.st_mtime;
			}
			free_media_info(&e->info);
			e->info = info;
			e->probed = ok ? 1 : -1;
		}
		else
			free_media_info(&info);
		pthread_mutex_unlock(&lock);

		if (art)
		{
			thumb = shrink_jpeg(art, art_size, &size);
			free(art);
			if (thumb)
			{
				store_thumb(j->path, &st, thumb, size);
				have_thumb = 1;
			}
		}
	}

#ifdef THUMBNAIL_CREATION
	if (thumb_job && !have_thumb && can_make_video_thumb(j->path, j->types))
	{
		thumb = video_thumb(j->path, &size);
		store_thumb(j->path, &st, thumb, size);
	}
#else
	(void)thumb_job;
	(void)have_thumb;
#endif
}

static void *
worker(void *arg)
{
	sigset_t set;
	struct job *j;
	int thumb_job;

	(void)arg;
	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
#ifdef __linux__
	/* Don't compete with streaming for the CPU */
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10);
#endif

	pthread_mutex_lock(&lock);
	while (!stopping)
	{
		thumb_job = 0;
		if (!(j = pop(&probe_q)))
		{
			j = pop(&thumb_q);
			thumb_job = 1;
		}
		if (!j)
		{
			pthread_cond_wait(&work_cv, &lock);
			continue;
		}
		pthread_mutex_unlock(&lock);
		process(j, thumb_job);
		pthread_mutex_lock(&lock);
		/* Urgent jobs run out of order, so a waiter may return a little
		 * early; it only means fewer details in that answer */
		if (!thumb_job)
		{
			if (j->ticket > done_ticket)
				done_ticket = j->ticket;
			pthread_cond_broadcast(&done_cv);
		}
		free(j->path);
		free(j);
	}
	pthread_mutex_unlock(&lock);

	return NULL;
}

void
fsmeta_start(void)
{
	lav_register_all();
	av_log_set_level(AV_LOG_PANIC);
	if (pthread_create(&worker_id, NULL, worker, NULL) != 0)
	{
		DPRINTF(E_ERROR, L_GENERAL, "Cannot start the probing thread [%s]; "
			"metadata will not be learned\n", strerror(errno));
		return;
	}
	running = 1;
}

void
fsmeta_stop(void)
{
	/* Not joined: the worker may be stuck reading a slow disk, and the
	 * process is about to exit anyway */
	pthread_mutex_lock(&lock);
	stopping = 1;
	running = 0;
	pthread_cond_broadcast(&work_cv);
	pthread_cond_broadcast(&done_cv);
	pthread_mutex_unlock(&lock);
}
