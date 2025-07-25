// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc. All Rights Reserved.
 */
#include "platform_defs.h"
#include "xfs.h"
#include "bitops.h"
#include "fsgeom.h"
#include "util.h"

static inline const char *
rtdev_name(
	struct xfs_fsop_geom	*geo,
	const char		*rtname)
{
	if (!geo->rtblocks)
		return _("none");
	if (geo->rtstart)
		return _("internal");
	if (!rtname)
		return _("external");
	return rtname;
}

void
xfs_report_geom(
	struct xfs_fsop_geom	*geo,
	const char		*mntpoint,
	const char		*logname,
	const char		*rtname)
{
	int			isint;
	int			lazycount;
	int			dirversion;
	int			logversion;
	int			attrversion;
	int			projid32bit;
	int			crcs_enabled;
	int			cimode;
	int			ftype_enabled;
	int			finobt_enabled;
	int			spinodes;
	int			rmapbt_enabled;
	int			reflink_enabled;
	int			bigtime_enabled;
	int			inobtcount;
	int			nrext64;
	int			exchangerange;
	int			parent;
	int			metadir;
	int			zoned;

	isint = geo->logstart > 0;
	lazycount = geo->flags & XFS_FSOP_GEOM_FLAGS_LAZYSB ? 1 : 0;
	dirversion = geo->flags & XFS_FSOP_GEOM_FLAGS_DIRV2 ? 2 : 1;
	logversion = geo->flags & XFS_FSOP_GEOM_FLAGS_LOGV2 ? 2 : 1;
	attrversion = geo->flags & XFS_FSOP_GEOM_FLAGS_ATTR2 ? 2 : \
			(geo->flags & XFS_FSOP_GEOM_FLAGS_ATTR ? 1 : 0);
	cimode = geo->flags & XFS_FSOP_GEOM_FLAGS_DIRV2CI ? 1 : 0;
	projid32bit = geo->flags & XFS_FSOP_GEOM_FLAGS_PROJID32 ? 1 : 0;
	crcs_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_V5SB ? 1 : 0;
	ftype_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_FTYPE ? 1 : 0;
	finobt_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_FINOBT ? 1 : 0;
	spinodes = geo->flags & XFS_FSOP_GEOM_FLAGS_SPINODES ? 1 : 0;
	rmapbt_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_RMAPBT ? 1 : 0;
	reflink_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_REFLINK ? 1 : 0;
	bigtime_enabled = geo->flags & XFS_FSOP_GEOM_FLAGS_BIGTIME ? 1 : 0;
	inobtcount = geo->flags & XFS_FSOP_GEOM_FLAGS_INOBTCNT ? 1 : 0;
	nrext64 = geo->flags & XFS_FSOP_GEOM_FLAGS_NREXT64 ? 1 : 0;
	exchangerange = geo->flags & XFS_FSOP_GEOM_FLAGS_EXCHANGE_RANGE ? 1 : 0;
	parent = geo->flags & XFS_FSOP_GEOM_FLAGS_PARENT ? 1 : 0;
	metadir = geo->flags & XFS_FSOP_GEOM_FLAGS_METADIR ? 1 : 0;
	zoned = geo->flags & XFS_FSOP_GEOM_FLAGS_ZONED ? 1 : 0;

	printf(_(
"meta-data=%-22s isize=%-6d agcount=%u, agsize=%u blks\n"
"         =%-22s sectsz=%-5u attr=%u, projid32bit=%u\n"
"         =%-22s crc=%-8u finobt=%u, sparse=%u, rmapbt=%u\n"
"         =%-22s reflink=%-4u bigtime=%u inobtcount=%u nrext64=%u\n"
"         =%-22s exchange=%-3u metadir=%u\n"
"data     =%-22s bsize=%-6u blocks=%llu, imaxpct=%u\n"
"         =%-22s sunit=%-6u swidth=%u blks\n"
"naming   =version %-14u bsize=%-6u ascii-ci=%d, ftype=%d, parent=%d\n"
"log      =%-22s bsize=%-6d blocks=%u, version=%d\n"
"         =%-22s sectsz=%-5u sunit=%d blks, lazy-count=%d\n"
"realtime =%-22s extsz=%-6d blocks=%lld, rtextents=%lld\n"
"         =%-22s rgcount=%-4d rgsize=%u extents\n"
"         =%-22s zoned=%-6d start=%llu reserved=%llu\n"),
		mntpoint, geo->inodesize, geo->agcount, geo->agblocks,
		"", geo->sectsize, attrversion, projid32bit,
		"", crcs_enabled, finobt_enabled, spinodes, rmapbt_enabled,
		"", reflink_enabled, bigtime_enabled, inobtcount, nrext64,
		"", exchangerange, metadir,
		"", geo->blocksize, (unsigned long long)geo->datablocks,
			geo->imaxpct,
		"", geo->sunit, geo->swidth,
		dirversion, geo->dirblocksize, cimode, ftype_enabled, parent,
		isint ? _("internal log") : logname ? logname : _("external"),
			geo->blocksize, geo->logblocks, logversion,
		"", geo->logsectsize, geo->logsunit / geo->blocksize, lazycount,
		rtdev_name(geo, rtname),
		geo->rtextsize * geo->blocksize, (unsigned long long)geo->rtblocks,
			(unsigned long long)geo->rtextents,
		"", geo->rgcount, geo->rgextents,
		"", zoned, geo->rtstart, geo->rtreserved);
}

/* Try to obtain the xfs geometry.  On error returns a negative error code. */
int
xfrog_geometry(
	int			fd,
	struct xfs_fsop_geom	*fsgeo)
{
	int			ret;

	memset(fsgeo, 0, sizeof(*fsgeo));

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY, fsgeo);
	if (!ret)
		return 0;

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY_V4, fsgeo);
	if (!ret)
		return 0;

	ret = ioctl(fd, XFS_IOC_FSGEOMETRY_V1, fsgeo);
	if (!ret)
		return 0;

	return -errno;
}

/* Compute conversion factors of an xfs_fd structure. */
static void
xfd_compute_conversion_factors(
	struct xfs_fd		*xfd)
{
	xfd->agblklog = log2_roundup(xfd->fsgeom.agblocks);
	xfd->blocklog = highbit32(xfd->fsgeom.blocksize);
	xfd->inodelog = highbit32(xfd->fsgeom.inodesize);
	xfd->inopblog = xfd->blocklog - xfd->inodelog;
	xfd->aginolog = xfd->agblklog + xfd->inopblog;
	xfd->blkbb_log = xfd->blocklog - BBSHIFT;
}

/*
 * Prepare xfs_fd structure for future ioctl operations by computing the xfs
 * geometry for @xfd->fd.  Returns zero or a negative error code.
 */
int
xfd_prepare_geometry(
	struct xfs_fd		*xfd)
{
	int			ret;

	ret = xfrog_geometry(xfd->fd, &xfd->fsgeom);
	if (ret)
		return ret;

	xfd_compute_conversion_factors(xfd);
	return 0;
}

/*
 * Prepare xfs_fd structure for future ioctl operations by computing the xfs
 * geometry for @xfd->fd.  Returns zero or a negative error code.
 */
void
xfd_install_geometry(
	struct xfs_fd		*xfd,
	struct xfs_fsop_geom	*fsgeom)
{
	memcpy(&xfd->fsgeom, fsgeom, sizeof(*fsgeom));
	xfd_compute_conversion_factors(xfd);
}

/* Open a file on an XFS filesystem.  Returns zero or a negative error code. */
int
xfd_open(
	struct xfs_fd		*xfd,
	const char		*pathname,
	int			flags)
{
	int			ret;

	xfd->fd = open(pathname, flags);
	if (xfd->fd < 0)
		return -errno;

	ret = xfd_prepare_geometry(xfd);
	if (ret) {
		xfd_close(xfd);
		return ret;
	}

	return 0;
}

/*
 * Release any resources associated with this xfs_fd structure.  Returns zero
 * or a negative error code.
 */
int
xfd_close(
	struct xfs_fd		*xfd)
{
	int			ret = 0;

	if (xfd->fd < 0)
		return 0;

	ret = close(xfd->fd);
	xfd->fd = -1;
	if (ret < 0)
		return -errno;

	return 0;
}

/* Try to obtain an AG's geometry.  Returns zero or a negative error code. */
int
xfrog_ag_geometry(
	int			fd,
	unsigned int		agno,
	struct xfs_ag_geometry	*ageo)
{
	int			ret;

	ageo->ag_number = agno;
	ret = ioctl(fd, XFS_IOC_AG_GEOMETRY, ageo);
	if (ret)
		return -errno;
	return 0;
}

/*
 * Try to obtain a rt group's geometry.  Returns zero or a negative error code.
 */
int
xfrog_rtgroup_geometry(
	int			fd,
	unsigned int		rgno,
	struct xfs_rtgroup_geometry	*rgeo)
{
	int			ret;

	rgeo->rg_number = rgno;
	ret = ioctl(fd, XFS_IOC_RTGROUP_GEOMETRY, rgeo);
	if (ret)
		return -errno;
	return 0;
}
