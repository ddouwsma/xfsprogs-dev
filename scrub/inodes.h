// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_INODES_H_
#define XFS_SCRUB_INODES_H_

/*
 * Callback for each inode in a filesystem.  Return 0 to continue iteration
 * or a positive error code to interrupt iteraton.  If ESTALE is returned,
 * iteration will be restarted from the beginning of the inode allocation
 * group.  Any other non zero value will stop iteration.  The special return
 * value ECANCELED can be used to stop iteration, because the inode iteration
 * function never generates that error code on its own.
 */
typedef int (*scrub_inode_iter_fn)(struct scrub_ctx *ctx,
		struct xfs_handle *handle, struct xfs_bulkstat *bs, void *arg);

/* Scan every file in the filesystem, including metadir and corrupt ones. */
int scrub_scan_all_inodes(struct scrub_ctx *ctx, scrub_inode_iter_fn fn,
		void *arg);

/* Scan all user-created files in the filesystem. */
int scrub_scan_user_files(struct scrub_ctx *ctx, scrub_inode_iter_fn fn,
		void *arg);

int scrub_open_handle(struct xfs_handle *handle);

#endif /* XFS_SCRUB_INODES_H_ */
