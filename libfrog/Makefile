# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2017 Oracle.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTLIBRARY = libfrog.la
LT_CURRENT = 0
LT_REVISION = 0
LT_AGE = 0
# we need a static build even if --disable-static is specified
LTLDFLAGS += -static

CFILES = \
avl64.c \
bitmap.c \
bulkstat.c \
convert.c \
crc32.c \
file_exchange.c \
fsgeom.c \
fsproperties.c \
fsprops.c \
getparents.c \
histogram.c \
list_sort.c \
linux.c \
logging.c \
paths.c \
projects.c \
ptvar.c \
radix-tree.c \
randbytes.c \
scrub.c \
util.c \
workqueue.c

HFILES = \
avl64.h \
bulkstat.h \
bitmap.h \
convert.h \
crc32c.h \
crc32cselftest.h \
crc32defs.h \
crc32table.h \
dahashselftest.h \
div64.h \
fakelibattr.h \
file_exchange.h \
fsgeom.h \
fsproperties.h \
fsprops.h \
getparents.h \
handle_priv.h \
histogram.h \
logging.h \
paths.h \
projects.h \
ptvar.h \
radix-tree.h \
randbytes.h \
scrub.h \
workqueue.h

GETTEXT_PY = \
	gettext.py

LSRCFILES += gen_crc32table.c

LDIRT = gen_crc32table crc32table.h

ifeq ($(ENABLE_GETTEXT),yes)
HAVE_GETTEXT = True
else
HAVE_GETTEXT = False
endif

default: ltdepend $(LTLIBRARY) $(GETTEXT_PY)

crc32table.h: gen_crc32table.c crc32defs.h
	@echo "    [CC]     gen_crc32table"
	$(Q) $(BUILD_CC) $(BUILD_CFLAGS) -o gen_crc32table $<
	@echo "    [GENERATE] $@"
	$(Q) ./gen_crc32table > crc32table.h

$(GETTEXT_PY): $(GETTEXT_PY).in $(TOPDIR)/include/builddefs
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@HAVE_GETTEXT@|$(HAVE_GETTEXT)|g" \
		   -e "s|@PACKAGE@|$(PKG_NAME)|g" \
		   -e "s|@LOCALEDIR@|$(PKG_LOCALE_DIR)|g" \
		   < $< > $@

include $(BUILDRULES)

install install-dev: default

-include .ltdep
