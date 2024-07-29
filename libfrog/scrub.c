// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019 Oracle, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "fsgeom.h"
#include "scrub.h"

/* These must correspond to XFS_SCRUB_TYPE_ */
const struct xfrog_scrub_descr xfrog_scrubbers[XFS_SCRUB_TYPE_NR] = {
	[XFS_SCRUB_TYPE_PROBE] = {
		.name	= "probe",
		.descr	= "metadata",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
	[XFS_SCRUB_TYPE_SB] = {
		.name	= "sb",
		.descr	= "superblock",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGF] = {
		.name	= "agf",
		.descr	= "free space header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGFL] = {
		.name	= "agfl",
		.descr	= "free list",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_AGI] = {
		.name	= "agi",
		.descr	= "inode header",
		.group	= XFROG_SCRUB_GROUP_AGHEADER,
	},
	[XFS_SCRUB_TYPE_BNOBT] = {
		.name	= "bnobt",
		.descr	= "freesp by block btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_CNTBT] = {
		.name	= "cntbt",
		.descr	= "freesp by length btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INOBT] = {
		.name	= "inobt",
		.descr	= "inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_FINOBT] = {
		.name	= "finobt",
		.descr	= "free inode btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_RMAPBT] = {
		.name	= "rmapbt",
		.descr	= "reverse mapping btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_REFCNTBT] = {
		.name	= "refcountbt",
		.descr	= "reference count btree",
		.group	= XFROG_SCRUB_GROUP_PERAG,
	},
	[XFS_SCRUB_TYPE_INODE] = {
		.name	= "inode",
		.descr	= "inode record",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTD] = {
		.name	= "bmapbtd",
		.descr	= "data block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTA] = {
		.name	= "bmapbta",
		.descr	= "attr block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_BMBTC] = {
		.name	= "bmapbtc",
		.descr	= "CoW block map",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_DIR] = {
		.name	= "directory",
		.descr	= "directory entries",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_XATTR] = {
		.name	= "xattr",
		.descr	= "extended attributes",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_SYMLINK] = {
		.name	= "symlink",
		.descr	= "symbolic link",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_PARENT] = {
		.name	= "parent",
		.descr	= "parent pointer",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
	[XFS_SCRUB_TYPE_RTBITMAP] = {
		.name	= "rtbitmap",
		.descr	= "realtime bitmap",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_RTSUM] = {
		.name	= "rtsummary",
		.descr	= "realtime summary",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_UQUOTA] = {
		.name	= "usrquota",
		.descr	= "user quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_GQUOTA] = {
		.name	= "grpquota",
		.descr	= "group quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_PQUOTA] = {
		.name	= "prjquota",
		.descr	= "project quotas",
		.group	= XFROG_SCRUB_GROUP_FS,
	},
	[XFS_SCRUB_TYPE_FSCOUNTERS] = {
		.name	= "fscounters",
		.descr	= "filesystem summary counters",
		.group	= XFROG_SCRUB_GROUP_SUMMARY,
	},
	[XFS_SCRUB_TYPE_QUOTACHECK] = {
		.name	= "quotacheck",
		.descr	= "quota counters",
		.group	= XFROG_SCRUB_GROUP_ISCAN,
	},
	[XFS_SCRUB_TYPE_NLINKS] = {
		.name	= "nlinks",
		.descr	= "inode link counts",
		.group	= XFROG_SCRUB_GROUP_ISCAN,
	},
	[XFS_SCRUB_TYPE_HEALTHY] = {
		.name	= "healthy",
		.descr	= "retained health records",
		.group	= XFROG_SCRUB_GROUP_NONE,
	},
	[XFS_SCRUB_TYPE_DIRTREE] = {
		.name	= "dirtree",
		.descr	= "directory tree structure",
		.group	= XFROG_SCRUB_GROUP_INODE,
	},
};
#undef DEP

/* Invoke the scrub ioctl.  Returns zero or negative error code. */
int
xfrog_scrub_metadata(
	struct xfs_fd			*xfd,
	struct xfs_scrub_metadata	*meta)
{
	int				ret;

	ret = ioctl(xfd->fd, XFS_IOC_SCRUB_METADATA, meta);
	if (ret)
		return -errno;

	return 0;
}

/* Decide if there have been any scrub failures up to this point. */
static inline int
xfrog_scrubv_check_barrier(
	const struct xfs_scrub_vec	*vectors,
	const struct xfs_scrub_vec	*stop_vec)
{
	const struct xfs_scrub_vec	*v;
	__u32				failmask;

	failmask = stop_vec->sv_flags & XFS_SCRUB_FLAGS_OUT;

	for (v = vectors; v < stop_vec; v++) {
		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER)
			continue;

		/*
		 * Runtime errors count as a previous failure, except the ones
		 * used to ask userspace to retry.
		 */
		switch (v->sv_ret) {
		case -EBUSY:
		case -ENOENT:
		case -EUSERS:
		case 0:
			break;
		default:
			return -ECANCELED;
		}

		/*
		 * If any of the out-flags on the scrub vector match the mask
		 * that was set on the barrier vector, that's a previous fail.
		 */
		if (v->sv_flags & failmask)
			return -ECANCELED;
	}

	return 0;
}

static int
xfrog_scrubv_fallback(
	struct xfs_fd			*xfd,
	struct xfrog_scrubv		*scrubv)
{
	struct xfs_scrub_vec		*vectors = scrubv->vectors;
	struct xfs_scrub_vec		*v;
	unsigned int			i;

	if (scrubv->head.svh_flags & ~XFS_SCRUB_VEC_FLAGS_ALL)
		return -EINVAL;

	foreach_xfrog_scrubv_vec(scrubv, i, v) {
		if (v->sv_reserved)
			return -EINVAL;

		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER &&
		    (v->sv_flags & ~XFS_SCRUB_FLAGS_OUT))
			return -EINVAL;
	}

	/* Run all the scrubbers. */
	foreach_xfrog_scrubv_vec(scrubv, i, v) {
		struct xfs_scrub_metadata	sm = {
			.sm_type		= v->sv_type,
			.sm_flags		= v->sv_flags,
			.sm_ino			= scrubv->head.svh_ino,
			.sm_gen			= scrubv->head.svh_gen,
			.sm_agno		= scrubv->head.svh_agno,
		};
		struct timespec	tv;

		if (v->sv_type == XFS_SCRUB_TYPE_BARRIER) {
			v->sv_ret = xfrog_scrubv_check_barrier(vectors, v);
			if (v->sv_ret)
				break;
			continue;
		}

		v->sv_ret = xfrog_scrub_metadata(xfd, &sm);
		v->sv_flags = sm.sm_flags;

		if (scrubv->head.svh_rest_us) {
			tv.tv_sec = 0;
			tv.tv_nsec = scrubv->head.svh_rest_us * 1000;
			nanosleep(&tv, NULL);
		}
	}

	return 0;
}

/* Invoke the vectored scrub ioctl. */
static int
xfrog_scrubv_call(
	struct xfs_fd			*xfd,
	struct xfs_scrub_vec_head	*vhead)
{
	int				ret;

	ret = ioctl(xfd->fd, XFS_IOC_SCRUBV_METADATA, vhead);
	if (ret)
		return -errno;

	return 0;
}

/* Invoke the vectored scrub ioctl.  Returns zero or negative error code. */
int
xfrog_scrubv_metadata(
	struct xfs_fd			*xfd,
	struct xfrog_scrubv		*scrubv)
{
	int				error = 0;

	if (scrubv->head.svh_nr > XFROG_SCRUBV_MAX_VECTORS)
		return -EINVAL;

	if (xfd->flags & XFROG_FLAG_SCRUB_FORCE_SINGLE)
		goto try_single;

	error = xfrog_scrubv_call(xfd, &scrubv->head);
	if (error == 0 || (xfd->flags & XFROG_FLAG_SCRUB_FORCE_VECTOR))
		return error;

	/* If the vectored scrub ioctl wasn't found, force single mode. */
	switch (error) {
	case -EOPNOTSUPP:
	case -ENOTTY:
		xfd->flags |= XFROG_FLAG_SCRUB_FORCE_SINGLE;
		break;
	}

try_single:
	return xfrog_scrubv_fallback(xfd, scrubv);
}
