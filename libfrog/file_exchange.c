// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "xfs.h"
#include "fsgeom.h"
#include "bulkstat.h"
#include "libfrog/file_exchange.h"

/* Prepare for a file contents exchange. */
void
xfrog_exchangerange_prep(
	struct xfs_exchange_range	*fxr,
	off_t				file2_offset,
	int				file1_fd,
	off_t				file1_offset,
	uint64_t			length)
{
	memset(fxr, 0, sizeof(*fxr));

	fxr->file1_fd			= file1_fd;
	fxr->file1_offset		= file1_offset;
	fxr->length			= length;
	fxr->file2_offset		= file2_offset;
}

/*
 * Execute an exchange-range operation.  Returns 0 for success or a negative
 * errno.
 */
int
xfrog_exchangerange(
	int				file2_fd,
	struct xfs_exchange_range	*fxr,
	uint64_t			flags)
{
	int				ret;

	fxr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_EXCHANGE_RANGE, fxr);
	if (ret)
		return -errno;

	return 0;
}

/*
 * Prepare for committing a file contents exchange if nobody changes file2 in
 * the meantime by asking the kernel to sample file2's change attributes.
 *
 * Returns 0 for success or a negative errno.
 */
int
xfrog_commitrange_prep(
	struct xfs_commit_range		*xcr,
	int				file2_fd,
	off_t				file2_offset,
	int				file1_fd,
	off_t				file1_offset,
	uint64_t			length)
{
	int				ret;

	memset(xcr, 0, sizeof(*xcr));

	xcr->file1_fd			= file1_fd;
	xcr->file1_offset		= file1_offset;
	xcr->length			= length;
	xcr->file2_offset		= file2_offset;

	ret = ioctl(file2_fd, XFS_IOC_START_COMMIT, xcr);
	if (ret)
		return -errno;

	return 0;
}

/*
 * Execute an exchange-commit operation.  Returns 0 for success or a negative
 * errno.
 */
int
xfrog_commitrange(
	int				file2_fd,
	struct xfs_commit_range		*xcr,
	uint64_t			flags)
{
	int				ret;

	xcr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_COMMIT_RANGE, xcr);
	if (ret)
		return -errno;

	return 0;
}

/* Opaque freshness blob for XFS_IOC_COMMIT_RANGE */
struct xfs_commit_range_fresh {
	xfs_fsid_t	fsid;		/* m_fixedfsid */
	__u64		file2_ino;	/* inode number */
	__s64		file2_mtime;	/* modification time */
	__s64		file2_ctime;	/* change time */
	__s32		file2_mtime_nsec; /* mod time, nsec */
	__s32		file2_ctime_nsec; /* change time, nsec */
	__u32		file2_gen;	/* inode generation */
	__u32		magic;		/* zero */
};

/* magic flag to force use of swapext */
#define XCR_SWAPEXT_MAGIC	0x43524150	/* CRAP */

/*
 * Import file2 freshness information for a XFS_IOC_SWAPEXT call from bulkstat
 * information.  We can skip the fsid and file2_gen members because old swapext
 * did not verify those things.
 */
static void
xfrog_swapext_prep(
	struct xfs_commit_range		*xdf,
	const struct xfs_bulkstat	*file2_stat)
{
	struct xfs_commit_range_fresh	*f;

	f = (struct xfs_commit_range_fresh *)&xdf->file2_freshness;
	f->file2_ino			= file2_stat->bs_ino;
	f->file2_mtime			= file2_stat->bs_mtime;
	f->file2_mtime_nsec		= file2_stat->bs_mtime_nsec;
	f->file2_ctime			= file2_stat->bs_ctime;
	f->file2_ctime_nsec		= file2_stat->bs_ctime_nsec;
	f->magic			= XCR_SWAPEXT_MAGIC;
}

/* Invoke the old swapext ioctl. */
static int
xfrog_ioc_swapext(
	int				file2_fd,
	struct xfs_commit_range		*xdf)
{
	struct xfs_swapext		args = {
		.sx_version		= XFS_SX_VERSION,
		.sx_fdtarget		= file2_fd,
		.sx_length		= xdf->length,
		.sx_fdtmp		= xdf->file1_fd,
	};
	struct xfs_commit_range_fresh	*f;
	int				ret;

	BUILD_BUG_ON(sizeof(struct xfs_commit_range_fresh) !=
		     sizeof(xdf->file2_freshness));

	f = (struct xfs_commit_range_fresh *)&xdf->file2_freshness;
	args.sx_stat.bs_ino		= f->file2_ino;
	args.sx_stat.bs_mtime.tv_sec	= f->file2_mtime;
	args.sx_stat.bs_mtime.tv_nsec	= f->file2_mtime_nsec;
	args.sx_stat.bs_ctime.tv_sec	= f->file2_ctime;
	args.sx_stat.bs_ctime.tv_nsec	= f->file2_ctime_nsec;

	ret = ioctl(file2_fd, XFS_IOC_SWAPEXT, &args);
	if (ret) {
		/*
		 * Old swapext returns EFAULT if file1 or file2 length doesn't
		 * match.  The new new COMMIT_RANGE doesn't check the file
		 * length, but the freshness checks will trip and return EBUSY.
		 * If we see EFAULT from the old ioctl, turn that into EBUSY.
		 */
		if (errno == EFAULT)
			return -EBUSY;
		return -errno;
	}

	return 0;
}

/*
 * Prepare for defragmenting a file by committing a file contents exchange if
 * if nobody changes file2 in the meantime by asking the kernel to sample
 * file2's change attributes.
 *
 * If the kernel supports only the old XFS_IOC_SWAPEXT ioctl, the @file2_stat
 * information will be used to sample the change attributes.
 *
 * Returns 0 or a negative errno.
 */
int
xfrog_defragrange_prep(
	struct xfs_commit_range		*xdf,
	int				file2_fd,
	const struct xfs_bulkstat	*file2_stat,
	int				file1_fd)
{
	int				ret;

	memset(xdf, 0, sizeof(*xdf));

	xdf->file1_fd			= file1_fd;
	xdf->length			= file2_stat->bs_size;

	ret = ioctl(file2_fd, XFS_IOC_START_COMMIT, xdf);
	if (ret && (errno == EOPNOTSUPP || errno == ENOTTY)) {
		xfrog_swapext_prep(xdf, file2_stat);
		return 0;
	}
	if (ret)
		return -errno;

	return 0;
}

/* Execute an exchange operation.  Returns 0 for success or a negative errno. */
int
xfrog_defragrange(
	int				file2_fd,
	struct xfs_commit_range		*xdf)
{
	struct xfs_commit_range_fresh	*f;
	int				ret;

	f = (struct xfs_commit_range_fresh *)&xdf->file2_freshness;
	if (f->magic == XCR_SWAPEXT_MAGIC)
		goto legacy_fallback;

	ret = ioctl(file2_fd, XFS_IOC_COMMIT_RANGE, xdf);
	if (ret) {
		if (errno == EOPNOTSUPP || errno != ENOTTY)
			goto legacy_fallback;
		return -errno;
	}

	return 0;

legacy_fallback:
	ret = xfrog_ioc_swapext(file2_fd, xdf);
	if (ret)
		return -errno;

	return 0;
}
