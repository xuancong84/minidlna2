# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MiniDLNA (ReadyMedia) 2.1.0 is a UPnP-A/V / DLNA media server daemon (`minidlnad`) written in C: Debian's minidlna 1.3.3+dfsg plus local patches 16–25. The version lives in `debian/changelog` (package `2.1.0-1`), `AC_INIT` in `configure.ac` and `MINIDLNA_VERSION` in `upnpglobalvars.h`; only binary builds (`dpkg-buildpackage -b`) work, as there is no 2.1.0 orig tarball. This tree is the **Debian source package** (`+dfsg`): `debian/` holds the packaging, and the quilt patches in `debian/patches/` (listed in `debian/patches/series`) are **already applied** to the working tree (see `.pc/applied-patches`). Changes to upstream sources should normally be made as a new or updated quilt patch, not as bare edits, or the package build will diverge from the patch stack.

## Build

Autotools build. Requires libexif, libjpeg, libid3tag, libFLAC, libvorbis/libogg, libsqlite3, libavformat (ffmpeg); optional libffmpegthumbnailer (`--enable-thumbnail`) and avahi (TiVo).

```sh
./autogen.sh                     # runs autoreconf -vfi
./configure --enable-thumbnail   # same main flags as debian/rules
make                             # builds ./minidlnad
make check                       # builds testupnpdescgen (the only "test")
./testupnpdescgen                # prints the generated UPnP XML device/service descriptions
```

Debian package build: `dpkg-buildpackage -us -uc` (uses `debian/rules` → `dh`, with `--enable-thumbnail --with-log-path=/var/log/minidlna`).

Notable configure options: `--enable-tivo`, `--enable-netgear`, `--enable-readynas`, `--enable-static`, `--enable-lto`, `--with-db-path`, `--with-log-path`. These become `#ifdef`s (e.g. `TIVO_SUPPORT`, `HAVE_INOTIFY`, `HAVE_KQUEUE`, `USE_FORK`) in `config.h`.

There is no unit test suite. To test manually, run in the foreground with debug output against a custom config (set `db_dir` in it to exercise database mode):

```sh
./minidlnad -d -f ./minidlna.conf -P ./minidlna.pid   # -d = debug, foreground
./minidlnad -R ...    # wipe files.db + art_cache and rebuild;  -r = rescan
```

## Architecture

**Two modes, chosen by `db_dir`** (local patch `16-live-folder-mode-without-database.patch`):
- `db_dir` unset (the default): **live folder mode**. No SQLite DB, scanner or inotify thread exists (`db` stays NULL). `fsbrowse.c` answers Browse from a fresh `readdir()` per request, typing entries from `d_type` + extension and `stat()`ing only the requested page. Object IDs are `0`, `F<media_dir index>` and `F<i>/<percent-encoded rel path>`; the same string is the URL token in `/MediaItems/<id>.<ext>`, `/Captions/<id>.srt` and `/AlbumArt/1-<id>.jpg`, resolved by `fs_resolve()` (rejects `..`, checks realpath against media_dirs unless `wide_links`). Rows are fed through the existing `callback()` in `upnpsoap.c` as synthetic 25-column rows, so DIDL output and client quirks are shared. Search, thumbnails/resizing, bookmarks and TiVo are disabled.
- `db_dir` set: the original database mode described below. Every DB access on a shared code path is guarded with `fs_mode()` (`fsbrowse.h`); keep that invariant when adding SQL to request handlers.

**Defaults and privileges** (local patch `17-defaults-run-as-invoking-user.patch` + packaging): `root_container` defaults to `B`; empty `db_dir=`/`user=` mean live folder mode / no user switch. There is no `minidlna` system user: the systemd unit and sysV script run as root, and `minidlnad` run by an ordinary user falls back to `$XDG_STATE_HOME/minidlna` (log) and `$XDG_RUNTIME_DIR/minidlna` (pid) when the system paths aren't writable.

**Command line options** (local patch `18-cmdline-options-and-cwd-media-dir.patch`): every config option is also `--name=value`/`--name value`, added to `ary_options` by `addoption()` (`options.c`) after the config file is read, so they override it; for list options (media_dir, album_art_names, network_interface) the first cmdline value replaces the file's. A config file is only read with `-f` (the service passes `-f /etc/minidlna.conf`); without it, built-in defaults + cmdline apply and `$PWD` becomes the media_dir. `album_art_names` has a built-in default list.

**HTTP port** (local patch `21-auto-http-port.patch`): `init()` opens the HTTP socket (static `http_socket`, used by `main()`) right after daemonizing, since daemonizing closes all fds. With no port set it steps from 8200 past busy ports; a set port is exact. Without `-P` the pid file is `minidlna-<port>.pid`, and instances off 8200 get a port-based UUID and `[<port>]` name suffix. `wide_links` defaults to yes; yes/no options must both set and clear their flag so overrides work.

**Learned metadata in live mode** (local patch `22-lazy-metadata-for-live-folders.patch`): `fsmeta.c` keeps an in-memory LRU cache (path → `struct media_info` + thumbnail JPEG, or folder → entry count), validated by size+mtime. All probing runs in its single worker thread because the extractors (`Probe*Metadata()` in `metadata.c`, which share code with the DB-mode `Get*Metadata()`) and `image_utils.c` use static state. Browse queues probes for folders with ≤ `probe_limit` media files and waits ≤ `probe_time` ms via tickets; `/MediaItems/` queues probe+thumbnail for played files. Keep probing off the main thread.

**Media extensions** (local patch `23-opus-mka-m4b-png-webp.patch`): `is_video()/is_audio()/is_image()` (`utils.c`) read lists set from `video_extensions`/`audio_extensions`/`image_extensions` via `set_media_extensions()`, called for all three in `init()` before any thread starts. Audio without a tag reader goes through `lav_readtags()` (`metadata.c`, libavformat); non-JPEG images get their size from libavformat and no resized/EXIF-thumbnail URLs.

**Single-process event loop + forked workers.** `minidlna.c:main()` → `init()` parses `minidlna.conf` (`options.c`) and CLI flags, opens/validates the SQLite DB, then registers sockets with the `event_module` abstraction (`event.h`; `select.c` on Linux, `kqueue.c` on BSD): SSDP multicast (`minissdp.c`), HTTP listener (`upnphttp.c`), optional TiVo beacon, and inotify/kqueue monitor. The loop also periodically sends SSDP NOTIFYs and garbage-collects event subscribers (`upnpevents.c`). Global state lives in `upnpglobalvars.c/h` (`runtime_vars`, `db`, `media_dirs`, flags via `GETFLAG/SETFLAG`).

**Media database.** All content metadata is in SQLite (`files.db` in the db dir). Schema is in `scanner_sqlite.h`: an `OBJECTS` table (UPnP object tree: `OBJECT_ID`, `PARENT_ID`, `CLASS`, `DETAIL_ID`) and a `DETAILS` table (per-file metadata), plus album art, captions, bookmarks, playlists. `DB_VERSION` in `upnpglobalvars.h` is stored in `PRAGMA user_version`; `sql.c` and `minidlna.c` force a full rescan/upgrade when it mismatches — **bump `DB_VERSION` when changing the schema**. Fixed container IDs (e.g. `"1"` music, `"64"` browse folders) are defined in `scanner.h`.

**Scanning.** `scanner.c` walks `media_dirs` (normally in a forked child, `start_scanner()`), and for each file calls `metadata.c` (`GetAudioMetadata`/`GetVideoMetadata`/`GetImageMetadata`) which uses `tagutils/` (per-format audio tag parsers: mp3, flac, ogg, aac, asf, wav, dsf/dff, pcm), libavformat for video, and libexif/libjpeg for images. It inserts into `DETAILS` and builds the virtual container tree (artist/album/genre/date/folder views) in `OBJECTS`. `playlist.c` parses playlists; `albumart.c` / `image_utils.c` handle cover art and resizing. `monitor.c` + `monitor_inotify.c`/`monitor_kqueue.c` apply incremental add/remove after the initial scan.

**HTTP / UPnP.** `upnphttp.c` parses requests and dispatches: device/service XML (`upnpdescgen.c`), SOAP actions (`upnpsoap.c` — ContentDirectory `Browse`/`Search` translate into SQL over OBJECTS/DETAILS and render DIDL-Lite; ConnectionManager; X_MS_MediaReceiverRegistrar), GENA SUBSCRIBE/UNSUBSCRIBE (`upnpevents.c`), and media streaming (`SendResp_dlnafile`, thumbnails, resized images, captions, icons). Streaming responses fork a child via `process_fork()` (`process.c`, bounded by `max_connections`). `minixml.c` + `upnpreplyparse.c` are the minimal XML parser used for SOAP bodies.

**Client quirks.** `clients.c` holds the `client_types[]` table that identifies renderers by User-Agent / headers and assigns `FLAG_*` quirk bits; behaviour throughout `upnpsoap.c`/`upnphttp.c` branches on these flags. `containers.c` defines `magic_containers[]`, which remap root/special container IDs per client (e.g. Xbox, Samsung) and per `root_container` config. Add new device workarounds here rather than ad hoc.

**TiVo** (`tivo_*.c`, `avahi.c`, behind `TIVO_SUPPORT`) is a separate HTTP command interface and discovery beacon/Bonjour.

`minidlna.conf` (and `minidlna.conf.5`) document all runtime options; new options need entries in `options.c`/`options.h`, the sample conf, and the man page.
