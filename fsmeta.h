/* microDLNA media server
 *
 * Live folder mode: metadata learned on demand (probed files, played files,
 * opened folders), kept in memory only.
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
#ifndef __FSMETA_H__
#define __FSMETA_H__

#include <stdint.h>
#include <sys/stat.h>
#include "microdlnatypes.h"
#include "metadata.h"

/* What fsmeta_request() should make sure of */
#define FSMETA_PROBE	0x01	/* duration, resolution, DLNA profile, tags... */
#define FSMETA_THUMB	0x02	/* a thumbnail (video frame or embedded cover) */
#define FSMETA_URGENT	0x04	/* probe before anything already queued */

/* Start/stop the worker thread that does all probing */
void fsmeta_start(void);
void fsmeta_stop(void);

/* Queue work for one file (types: the media types allowed in its media_dir).
 * Returns a ticket for fsmeta_wait() if a probe was queued, else 0. */
unsigned long fsmeta_request(const char *path, const struct stat *st, media_types types, int what);

/* Wait until the probe with this ticket (and all before it) is done,
 * or until deadline_ms (from fsmeta_now_ms()) has passed */
void fsmeta_wait(unsigned long ticket, long long deadline_ms);
long long fsmeta_now_ms(void);

/* A copy of what is known about a file that hasn't changed since (size and
 * mtime match st). info->art is never filled; *thumb tells if a thumbnail
 * can be fetched. Returns 1 if probed, -1 if probing failed, 0 if unknown. */
int fsmeta_get(const char *path, const struct stat *st, struct media_info *info, int *thumb);

/* A malloc'ed copy of the thumbnail JPEG, if there is an up-to-date one */
int fsmeta_get_thumb(const char *path, const struct stat *st, uint8_t **data, int *size);

/* Number of entries in an opened folder and whether it holds a cover file,
 * valid while its mtime is unchanged */
void fsmeta_set_count(const char *path, time_t mtime, int count, int has_art);
int fsmeta_get_count(const char *path, time_t mtime, int *has_art);	/* -1 if unknown */
int fsmeta_has_count(const char *path);	/* a count is stored, maybe stale */

#endif
