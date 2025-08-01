// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_REPAIR_AGHEADER_H__
#define __XFS_REPAIR_AGHEADER_H__

typedef struct fs_geometry  {
	/*
	 * these types should match the superblock types
	 */
	uint32_t	sb_blocksize;	/* blocksize (bytes) */
	xfs_rfsblock_t	sb_dblocks;	/* # data blocks */
	xfs_rfsblock_t	sb_rblocks;	/* # realtime blocks */
	xfs_rtxnum_t	sb_rextents;	/* # realtime extents */
	xfs_fsblock_t	sb_logstart;	/* starting log block # */
	xfs_agblock_t	sb_rextsize;	/* realtime extent size (blocks )*/
	xfs_agblock_t	sb_agblocks;	/* # of blocks per ag */
	xfs_agnumber_t	sb_agcount;	/* # of ags */
	xfs_extlen_t	sb_rbmblocks;	/* # of rt bitmap blocks */
	xfs_extlen_t	sb_logblocks;	/* # of log blocks */
	uint16_t	sb_sectsize;	/* volume sector size (bytes) */
	uint16_t	sb_inodesize;	/* inode size (bytes) */
	uint8_t		sb_imax_pct;	/* max % of fs for inode space */

	/*
	 * these don't have to match the superblock types but are placed
	 * before sb_shared_vn because these values don't have to be
	 * checked manually.  These variables will be set only on
	 * filesystems with dependably good (fully initialized)
	 * secondary superblock sectors, will be stamped in all
	 * superblocks at mkfs time, and are features that cannot
	 * be downgraded unless all superblocks in the filesystem
	 * are rewritten.
	 */
	int		sb_extflgbit;	/* extent flag feature bit set */

	/*
	 * fields after this point have to be checked manually in compare_sb()
	 */
	uint8_t		sb_shared_vn;	/* shared version number */
	xfs_extlen_t	sb_inoalignmt;	/* inode chunk alignment, fsblocks */
	uint32_t	sb_unit;	/* stripe or raid unit */
	uint32_t	sb_width;	/* stripe or width unit */

	/*
	 * these don't have to match, they track superblock properties
	 * that could have been upgraded and/or downgraded during
	 * run-time so that the primary superblock has them but the
	 * secondaries do not.
	 * Plus, they have associated data fields whose data fields may
	 * be corrupt in cases where the filesystem was made on a
	 * pre-6.5 campus alpha mkfs and the feature was enabled on
	 * the filesystem later.
	 */
	int		sb_ialignbit;	/* sb has inode alignment bit set */
	int		sb_salignbit;	/* sb has stripe alignment bit set */
	int		sb_sharedbit;	/* sb has inode alignment bit set */

	int		sb_fully_zeroed; /* has zeroed secondary sb sectors */
} fs_geometry_t;

typedef struct fs_geo_list  {
	struct fs_geo_list	*next;
	int			refs;
	int			index;
	fs_geometry_t		geo;
} fs_geo_list_t;

/*
 * fields for sb_last_nonzero
 */

#define XR_SB_COUNTERS		0x0001
#define XR_SB_INOALIGN		0x0002
#define XR_SB_SALIGN		0x0004

/*
 * what got modified by verify_set_* routines
 */

#define XR_AG_SB	0x1
#define XR_AG_AGF	0x2
#define XR_AG_AGI	0x4
#define XR_AG_SB_SEC	0x8

static inline bool xfs_sb_version_hasmetadir(const struct xfs_sb *sbp)
{
	return xfs_sb_is_v5(sbp) &&
		(sbp->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_METADIR);
}

#endif /* __XFS_REPAIR_AGHEADER_H__ */
