// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_DB_BLOCK_H
#define __XFS_DB_BLOCK_H

struct field;

extern void	block_init(void);
extern void	print_block(const struct field *fields, int argc, char **argv);

static inline xfs_daddr_t
xfs_rtb_to_daddr(
	struct xfs_mount	*mp,
	xfs_rtblock_t		rtb)
{
	return rtb << mp->m_blkbb_log;
}

static inline xfs_rtblock_t
xfs_daddr_to_rtb(
	struct xfs_mount	*mp,
	xfs_daddr_t		daddr)
{
	return daddr >> mp->m_blkbb_log;
}

#endif /* __XFS_DB_BLOCK_H */
