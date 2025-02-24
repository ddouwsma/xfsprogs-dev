// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023-2025 Oracle.  All Rights Reserved.
 * Author: Catherine Hoang <catherine.hoang@oracle.com>
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef DB_NAMEI_H_
#define DB_NAMEI_H_

int path_walk(xfs_ino_t rootino, const char *path);

typedef int (*dir_emit_t)(struct xfs_trans *tp, struct xfs_inode *dp,
		xfs_dir2_dataptr_t off, char *name, ssize_t namelen,
		xfs_ino_t ino, uint8_t dtype, void *private);

int listdir(struct xfs_trans *tp, struct xfs_inode *dp, dir_emit_t dir_emit,
		void *private);

#endif /* DB_NAMEI_H_ */
