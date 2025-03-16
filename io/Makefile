# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2000-2005 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTCOMMAND = xfs_io
LSRCFILES = xfs_bmap.sh xfs_freeze.sh xfs_mkfile.sh xfs_property
HFILES = init.h io.h
CFILES = \
	aginfo.c \
	attr.c \
	bmap.c \
	bulkstat.c \
	cowextsize.c \
	crc32cselftest.c \
	encrypt.c \
	exchrange.c \
	fadvise.c \
	fiemap.c \
	file.c \
	freeze.c \
	fsproperties.c \
	fsuuid.c \
	fsync.c \
	getrusage.c \
	imap.c \
	init.c \
	inject.c \
	label.c \
	link.c \
	madvise.c \
	mincore.c \
	mmap.c \
	open.c \
	parent.c \
	pread.c \
	prealloc.c \
	pwrite.c \
	readdir.c \
	reflink.c \
	resblks.c \
	scrub.c \
	seek.c \
	sendfile.c \
	shutdown.c \
	stat.c \
	swapext.c \
	sync.c \
	sync_file_range.c \
	truncate.c \
	utimes.c

LLDLIBS = $(LIBXCMD) $(LIBHANDLE) $(LIBFROG) $(LIBPTHREAD) $(LIBUUID)
LTDEPENDENCIES = $(LIBXCMD) $(LIBHANDLE) $(LIBFROG)
LLDFLAGS = -static-libtool-libs

ifeq ($(HAVE_COPY_FILE_RANGE),yes)
CFILES += copy_file_range.c
LCFLAGS += -DHAVE_COPY_FILE_RANGE
endif

ifeq ($(HAVE_CACHESTAT),yes)
CFILES += cachestat.c
LCFLAGS += -DHAVE_CACHESTAT
endif

ifeq ($(ENABLE_EDITLINE),yes)
LLDLIBS += $(LIBEDITLINE) $(LIBTERMCAP)
endif

ifeq ($(HAVE_PWRITEV2),yes)
LCFLAGS += -DHAVE_PWRITEV2 -DHAVE_PREADV2
endif

ifeq ($(HAVE_MAP_SYNC),yes)
LCFLAGS += -DHAVE_MAP_SYNC
endif

ifeq ($(HAVE_DEVMAPPER),yes)
CFILES += log_writes.c
LLDLIBS += $(LIBDEVMAPPER)
LCFLAGS += -DHAVE_DEVMAPPER
endif

ifeq ($(HAVE_GETFSMAP),yes)
CFILES += fsmap.c
endif

default: depend $(LTCOMMAND)

include $(BUILDRULES)

install: default
	$(INSTALL) -m 755 -d $(PKG_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_SBIN_DIR)
	$(LTINSTALL) -m 755 xfs_bmap.sh $(PKG_SBIN_DIR)/xfs_bmap
	$(LTINSTALL) -m 755 xfs_freeze.sh $(PKG_SBIN_DIR)/xfs_freeze
	$(LTINSTALL) -m 755 xfs_mkfile.sh $(PKG_SBIN_DIR)/xfs_mkfile
	$(LTINSTALL) -m 755 xfs_property $(PKG_SBIN_DIR)/xfs_property
install-dev:

-include .dep
