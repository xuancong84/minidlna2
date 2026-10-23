################################################################################
#
# microdlna
#
################################################################################

MICRODLNA_VERSION = main
MICRODLNA_SITE = https://github.com/xuancong84/minidlna2.git
MICRODLNA_SITE_METHOD = git
MICRODLNA_LICENSE = GPL-2.0, BSD-3-Clause
MICRODLNA_LICENSE_FILES = COPYING LICENCE.miniupnpd

MICRODLNA_DEPENDENCIES = \
	$(TARGET_NLS_DEPENDENCIES) \
	ffmpeg flac libvorbis libogg libid3tag libexif jpeg sqlite icu zlib \
	host-xutil_makedepend

MICRODLNA_CONF_OPTS = \
	--enable-static \
	--enable-tivo \
	--enable-lto

define MICRODLNA_RUN_AUTOGEN
    cd $(@D) && PATH=$(BR_PATH) ./autogen.sh
endef
MICRODLNA_PRE_CONFIGURE_HOOKS = MICRODLNA_RUN_AUTOGEN

$(eval $(autotools-package))
