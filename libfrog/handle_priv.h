// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_HANDLE_PRIV_H__
#define __LIBFROG_HANDLE_PRIV_H__

/*
 * Private helpers to construct an xfs_handle without publishing those details
 * in the public libhandle header files.
 */

/*
 * Fills out the fsid part of a handle.  This does not initialize the fid part
 * of the handle; use either of the two functions below.
 */
static inline void
handle_from_fshandle(
	struct xfs_handle	*handle,
	const void		*fshandle,
	size_t			fshandle_len)
{
	ASSERT(fshandle_len == sizeof(xfs_fsid_t));

	memcpy(&handle->ha_fsid, fshandle, sizeof(handle->ha_fsid));
	handle->ha_fid.fid_len = sizeof(xfs_fid_t) -
			sizeof(handle->ha_fid.fid_len);
	handle->ha_fid.fid_pad = 0;
	handle->ha_fid.fid_ino = 0;
	handle->ha_fid.fid_gen = 0;
}

/* Fill out the fid part of a handle from raw components. */
static inline void
handle_from_inogen(
	struct xfs_handle	*handle,
	uint64_t		ino,
	uint32_t		gen)
{
	handle->ha_fid.fid_ino = ino;
	handle->ha_fid.fid_gen = gen;
}

/* Fill out the fid part of a handle. */
static inline void
handle_from_bulkstat(
	struct xfs_handle		*handle,
	const struct xfs_bulkstat	*bstat)
{
	handle->ha_fid.fid_ino = bstat->bs_ino;
	handle->ha_fid.fid_gen = bstat->bs_gen;
}

#endif /* __LIBFROG_HANDLE_PRIV_H__ */
