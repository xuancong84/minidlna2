/* Metadata extraction
 *
 * Project : microdlna
 * Website : http://sourceforge.net/projects/minidlna/
 * Author  : Justin Maggard
 *
 * microDLNA media server
 * Copyright (C) 2008-2009  Justin Maggard
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
#ifndef __METADATA_H__
#define __METADATA_H__

typedef struct metadata_s {
	char *       title;
	char *       artist;
	char *       creator;
	char *       album;
	char *       genre;
	char *       comment;
	unsigned int disc;
	unsigned int track;
	unsigned int channels;
	unsigned int bitrate;
	unsigned int frequency;
	unsigned int rotation;
	char *       resolution;
	char *       duration;
	char *       date;
	char *       mime;
	char *       dlna_pn;
	int          thumb_size;
	uint8_t *    thumb_data;
} metadata_t;

typedef enum {
  AAC_INVALID   =  0,
  AAC_MAIN      =  1, /* AAC Main */
  AAC_LC        =  2, /* AAC Low complexity */
  AAC_SSR       =  3, /* AAC SSR */
  AAC_LTP       =  4, /* AAC Long term prediction */
  AAC_HE        =  5, /* AAC High efficiency (SBR) */
  AAC_SCALE     =  6, /* Scalable */
  AAC_TWINVQ    =  7, /* TwinVQ */
  AAC_CELP      =  8, /* CELP */
  AAC_HVXC      =  9, /* HVXC */
  AAC_TTSI      = 12, /* TTSI */
  AAC_MS        = 13, /* Main synthetic */
  AAC_WAVE      = 14, /* Wavetable synthesis */
  AAC_MIDI      = 15, /* General MIDI */
  AAC_FX        = 16, /* Algorithmic Synthesis and Audio FX */
  AAC_LC_ER     = 17, /* AAC Low complexity with error recovery */
  AAC_LTP_ER    = 19, /* AAC Long term prediction with error recovery */
  AAC_SCALE_ER  = 20, /* AAC scalable with error recovery */
  AAC_TWINVQ_ER = 21, /* TwinVQ with error recovery */
  AAC_BSAC_ER   = 22, /* BSAC with error recovery */
  AAC_LD_ER     = 23, /* AAC LD with error recovery */
  AAC_CELP_ER   = 24, /* CELP with error recovery */
  AAC_HXVC_ER   = 25, /* HXVC with error recovery */
  AAC_HILN_ER   = 26, /* HILN with error recovery */
  AAC_PARAM_ER  = 27, /* Parametric with error recovery */
  AAC_SSC       = 28, /* AAC SSC */
  AAC_HE_L3     = 31, /* Reserved : seems to be HeAAC L3 */
} aac_object_type_t;

typedef enum {
	NONE,
	EMPTY,
	VALID
} ts_timestamp_t;

int
ends_with(const char *haystack, const char *needle);

void
check_for_captions(const char *path, int64_t detailID);

int64_t
GetFolderMetadata(const char *name, const char *path, const char *artist, const char *genre, int64_t album_art);

int64_t
GetAudioMetadata(const char *path, const char *name);

int64_t
GetImageMetadata(const char *path, const char *name);

int64_t
GetVideoMetadata(const char *path, const char *name);

/* What a probe learns about one file, for live folder mode. Text fields
 * are escaped the same way as in the database (ready for DIDL output). */
struct media_info {
	char *title;
	char *creator;
	char *artist;
	char *album;
	char *genre;
	char *comment;
	char *date;
	char *duration;
	char *resolution;
	char *dlna_pn;
	char *mime;
	unsigned int channels;
	unsigned int bitrate;
	unsigned int frequency;
	unsigned int disc;
	unsigned int track;
	unsigned int rotation;
	int exif_thumb;		/* image has a usable EXIF thumbnail */
	uint8_t *art;		/* embedded cover / attached picture (JPEG) */
	int art_size;
};

/* Probe*Metadata(): fill *info without touching the database.
 * They share static state, so call them from one thread only.
 * returns: 1 success, 0 failure */
int
ProbeAudioMetadata(const char *path, const char *name, struct media_info *info);

int
ProbeImageMetadata(const char *path, const char *name, struct media_info *info);

int
ProbeVideoMetadata(const char *path, const char *name, struct media_info *info);

void
free_media_info(struct media_info *info);

#endif
