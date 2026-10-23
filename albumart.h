/* Album art extraction, caching, and scaling
 *
 * Project : microdlna
 * Website : http://sourceforge.net/projects/minidlna/
 * Author  : Justin Maggard
 *
 * microDLNA media server
 * Copyright (C) 2008  Justin Maggard
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
#ifndef __ALBUMART_H__
#define __ALBUMART_H__

void update_if_album_art(const char *path);
int64_t find_album_art(const char *path, uint8_t *image_data, int image_size);

#endif
