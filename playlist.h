/* Playlist handling
 *
 * Project : microdlna
 * Website : http://sourceforge.net/projects/minidlna/
 * Author  : Justin Maggard
 *
 * microDLNA media server
 * Copyright (C) 2008-2010  Justin Maggard
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
#ifndef __PLAYLIST_H__
#define __PLAYLIST_H__

int insert_playlist(const char *path, const char *name);
int fill_playlists(void);

#endif // __PLAYLIST_H__
