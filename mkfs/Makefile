# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
#

TOPDIR = ..
include $(TOPDIR)/include/builddefs

LTCOMMAND = mkfs.xfs
XFS_PROTOFILE = xfs_protofile.py

HFILES =
CFILES = proto.c xfs_mkfs.c
CFGFILES = \
	dax_x86_64.conf \
	lts_4.19.conf \
	lts_5.4.conf \
	lts_5.10.conf \
	lts_5.15.conf \
	lts_6.1.conf \
	lts_6.6.conf \
	lts_6.12.conf

LLDLIBS += $(LIBXFS) $(LIBXCMD) $(LIBFROG) $(LIBRT) $(LIBBLKID) \
	$(LIBUUID) $(LIBINIH) $(LIBURCU) $(LIBPTHREAD)
LTDEPENDENCIES += $(LIBXFS) $(LIBXCMD) $(LIBFROG)
LLDFLAGS = -static-libtool-libs
DIRT = $(XFS_PROTOFILE)

default: depend $(LTCOMMAND) $(CFGFILES) $(XFS_PROTOFILE)

include $(BUILDRULES)

$(XFS_PROTOFILE): $(XFS_PROTOFILE).in $(TOPDIR)/include/builddefs $(TOPDIR)/libfrog/gettext.py
	@echo "    [SED]    $@"
	$(Q)$(SED) -e "s|@pkg_version@|$(PKG_VERSION)|g" \
		   -e '/@INIT_GETTEXT@/r $(TOPDIR)/libfrog/gettext.py' \
		   -e '/@INIT_GETTEXT@/d' \
		   < $< > $@
	$(Q)chmod a+x $@

install: default
	$(INSTALL) -m 755 -d $(PKG_SBIN_DIR)
	$(LTINSTALL) -m 755 $(LTCOMMAND) $(PKG_SBIN_DIR)
	$(INSTALL) -m 755 $(XFS_PROTOFILE) $(PKG_SBIN_DIR)/xfs_protofile
	$(INSTALL) -m 755 -d $(MKFS_CFG_DIR)
	$(INSTALL) -m 644 $(CFGFILES) $(MKFS_CFG_DIR)

install-dev:

-include .dep
