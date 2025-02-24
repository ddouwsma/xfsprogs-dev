// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef _XFS_REPAIR_RT_H_
#define _XFS_REPAIR_RT_H_

void generate_rtinfo(struct xfs_mount *mp);
void check_rtbitmap(struct xfs_mount *mp);
void check_rtsummary(struct xfs_mount *mp);

void fill_rtbitmap(struct xfs_rtgroup *rtg);
void fill_rtsummary(struct xfs_rtgroup *rtg);

void discover_rtgroup_inodes(struct xfs_mount *mp);
void unload_rtgroup_inodes(struct xfs_mount *mp);

void init_rtgroup_inodes(void);
void free_rtgroup_inodes(void);

bool is_rtgroup_inode(xfs_ino_t ino, enum xfs_rtg_inodes type);

static inline bool is_rtbitmap_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTGI_BITMAP);
}
static inline bool is_rtsummary_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTGI_SUMMARY);
}
static inline bool is_rtrmap_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTGI_RMAP);
}
static inline bool is_rtrefcount_inode(xfs_ino_t ino)
{
	return is_rtgroup_inode(ino, XFS_RTGI_REFCOUNT);
}

void mark_rtgroup_inodes_bad(struct xfs_mount *mp, enum xfs_rtg_inodes type);
bool rtgroup_inodes_were_bad(enum xfs_rtg_inodes type);

void check_rtsb(struct xfs_mount *mp);
void rewrite_rtsb(struct xfs_mount *mp);

#endif /* _XFS_REPAIR_RT_H_ */
