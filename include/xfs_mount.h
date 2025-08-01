// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#ifndef __XFS_MOUNT_H__
#define __XFS_MOUNT_H__

struct xfs_inode;
struct xfs_buftarg;
struct xfs_da_geometry;
struct libxfs_init;
struct xfs_group;

typedef void (*buf_writeback_fn)(struct xfs_buf *bp);

/* dynamic preallocation free space thresholds, 5% down to 1% */
enum {
	XFS_LOWSP_1_PCNT = 0,
	XFS_LOWSP_2_PCNT,
	XFS_LOWSP_3_PCNT,
	XFS_LOWSP_4_PCNT,
	XFS_LOWSP_5_PCNT,
	XFS_LOWSP_MAX,
};

struct xfs_groups {
	struct xarray		xa;

	/*
	 * Maximum capacity of the group in FSBs.
	 *
	 * Each group is laid out densely in the daddr space.  For the
	 * degenerate case of a pre-rtgroups filesystem, the incore rtgroup
	 * pretends to have a zero-block and zero-blklog rtgroup.
	 */
	uint32_t		blocks;

	/*
	 * Log(2) of the logical size of each group.
	 *
	 * Compared to the blocks field above this is rounded up to the next
	 * power of two, and thus lays out the xfs_fsblock_t/xfs_rtblock_t
	 * space sparsely with a hole from blocks to (1 << blklog) at the end
	 * of each group.
	 */
	uint8_t			blklog;

	/*
	 * Zoned devices can have gaps beyoned the usable capacity of a zone
	 * and the end in the LBA/daddr address space.  In other words, the
	 * hardware equivalent to the RT groups already takes care of the power
	 * of 2 alignment for us.  In this case the sparse FSB/RTB address space
	 * maps 1:1 to the device address space.
	 */
	bool			has_daddr_gaps;

	/*
	 * Mask to extract the group-relative block number from a FSB.
	 * For a pre-rtgroups filesystem we pretend to have one very large
	 * rtgroup, so this mask must be 64-bit.
	 */
	uint64_t		blkmask;

	/*
	 * Start of the first group in the device.  This is used to support a
	 * RT device following the data device on the same block device for
	 * SMR hard drives.
	 */
	xfs_fsblock_t		start_fsb;
};

/*
 * Define a user-level mount structure with all we need
 * in order to make use of the numerous XFS_* macros.
 */
typedef struct xfs_mount {
	xfs_sb_t		m_sb;		/* copy of fs superblock */
#define m_icount	m_sb.sb_icount
#define m_ifree		m_sb.sb_ifree
	spinlock_t		m_sb_lock;

	/*
	 * Bitsets of per-fs metadata that have been checked and/or are sick.
	 * Callers must hold m_sb_lock to access these two fields.
	 */
	uint8_t			m_fs_checked;
	uint8_t			m_fs_sick;

	char			*m_fsname;	/* filesystem name */
	int			m_bsize;	/* fs logical block size */
	spinlock_t		m_agirotor_lock;
	xfs_agnumber_t		m_agfrotor;	/* last ag where space found */
	xfs_agnumber_t		m_agirotor;	/* last ag dir inode alloced */
	xfs_agnumber_t		m_maxagi;	/* highest inode alloc group */
        struct xfs_ino_geometry	m_ino_geo;	/* inode geometry */
	uint			m_rsumlevels;	/* rt summary levels */
	xfs_filblks_t		m_rsumblocks;	/* size of rt summary, FSBs */
	struct xfs_inode	*m_metadirip;	/* ptr to metadata directory */
	struct xfs_inode	*m_rtdirip;	/* ptr to realtime metadir */
	struct xfs_buftarg	*m_ddev_targp;
	struct xfs_buftarg	*m_logdev_targp;
	struct xfs_buftarg	*m_rtdev_targp;
#define m_dev		m_ddev_targp
#define m_logdev	m_logdev_targp
#define m_rtdev		m_rtdev_targp
	uint8_t			m_dircook_elog;	/* log d-cookie entry bits */
	uint8_t			m_blkbit_log;	/* blocklog + NBBY */
	uint8_t			m_blkbb_log;	/* blocklog - BBSHIFT */
	uint8_t			m_sectbb_log;	/* sectorlog - BBSHIFT */
	uint8_t			m_agno_log;	/* log #ag's */
	int8_t			m_rtxblklog;	/* log2 of rextsize, if possible */

	uint			m_blockmask;	/* sb_blocksize-1 */
	uint			m_blockwsize;	/* sb_blocksize in words */
	/* number of rt extents per rt bitmap block if rtgroups enabled */
	unsigned int		m_rtx_per_rbmblock;
	uint			m_alloc_mxr[2];	/* XFS_ALLOC_BLOCK_MAXRECS */
	uint			m_alloc_mnr[2];	/* XFS_ALLOC_BLOCK_MINRECS */
	uint			m_bmap_dmxr[2];	/* XFS_BMAP_BLOCK_DMAXRECS */
	uint			m_bmap_dmnr[2];	/* XFS_BMAP_BLOCK_DMINRECS */
	uint			m_rmap_mxr[2];	/* max rmap btree records */
	uint			m_rmap_mnr[2];	/* min rmap btree records */
	uint			m_rtrmap_mxr[2]; /* max rtrmap btree records */
	uint			m_rtrmap_mnr[2]; /* min rtrmap btree records */
	uint			m_refc_mxr[2];	/* max refc btree records */
	uint			m_refc_mnr[2];	/* min refc btree records */
	unsigned int		m_rtrefc_mxr[2]; /* max rtrefc btree records */
	unsigned int		m_rtrefc_mnr[2]; /* min rtrefc btree records */
	uint			m_alloc_maxlevels; /* max alloc btree levels */
	uint			m_bm_maxlevels[2];  /* max bmap btree levels */
	uint			m_rmap_maxlevels; /* max rmap btree levels */
	uint			m_rtrmap_maxlevels; /* max rtrmap btree level */
	uint			m_refc_maxlevels; /* max refc btree levels */
	unsigned int		m_rtrefc_maxlevels; /* max rtrefc btree level */
	unsigned int		m_agbtree_maxlevels; /* max level of all AG btrees */
	unsigned int		m_rtbtree_maxlevels; /* max level of all rt btrees */
	xfs_extlen_t		m_ag_prealloc_blocks; /* reserved ag blocks */
	uint			m_alloc_set_aside; /* space we can't use */
	uint			m_ag_max_usable; /* max space per AG */
	struct xfs_groups	m_groups[XG_TYPE_MAX];
	uint64_t		m_features;	/* active filesystem features */
	uint64_t		m_low_space[XFS_LOWSP_MAX];
	uint64_t		m_rtxblkmask;	/* rt extent block mask */
	unsigned long		m_opstate;	/* dynamic state flags */
	bool			m_finobt_nores; /* no per-AG finobt resv. */
	uint			m_qflags;	/* quota status flags */
	uint			m_attroffset;	/* inode attribute offset */
	struct xfs_trans_resv	m_resv;		/* precomputed res values */
	int			m_dalign;	/* stripe unit */
	int			m_swidth;	/* stripe width */
	const struct xfs_nameops *m_dirnameops;	/* vector of dir name ops */

	struct xfs_da_geometry	*m_dir_geo;	/* directory block geometry */
	struct xfs_da_geometry	*m_attr_geo;	/* attribute block geometry */

	/*
	 * anonymous struct to allow xfs_dquot_buf.c to compile.
	 * Pointer is always null in userspace, so code does not use it at all
	 */
	struct {
		int	qi_dqperchunk;
	}			*m_quotainfo;

	buf_writeback_fn	m_buf_writeback_fn;

	/*
	 * xlog is defined in libxlog and thus is not intialized by libxfs. This
	 * allows an application to initialize and store a reference to the log
	 * if warranted.
	 */
	struct xlog		*m_log;		/* log specific stuff */

        /*
	 * Global count of allocation btree blocks in use across all AGs. Only
	 * used when perag reservation is enabled. Helps prevent block
	 * reservation from attempting to reserve allocation btree blocks.
	 */
	atomic64_t		m_allocbt_blks;
	spinlock_t		m_perag_lock;	/* lock for m_perag_tree */

	pthread_mutex_t		m_metafile_resv_lock;
	uint64_t		m_metafile_resv_target;
	uint64_t		m_metafile_resv_used;
	uint64_t		m_metafile_resv_avail;
} xfs_mount_t;

#define M_IGEO(mp)		(&(mp)->m_ino_geo)

/*
 * Flags for m_features.
 *
 * These are all the active features in the filesystem, regardless of how
 * they are configured.
 */
#define XFS_FEAT_ATTR		(1ULL << 0)	/* xattrs present in fs */
#define XFS_FEAT_NLINK		(1ULL << 1)	/* 32 bit link counts */
#define XFS_FEAT_QUOTA		(1ULL << 2)	/* quota active */
#define XFS_FEAT_ALIGN		(1ULL << 3)	/* inode alignment */
#define XFS_FEAT_DALIGN		(1ULL << 4)	/* data alignment */
#define XFS_FEAT_LOGV2		(1ULL << 5)	/* version 2 logs */
#define XFS_FEAT_SECTOR		(1ULL << 6)	/* sector size > 512 bytes */
#define XFS_FEAT_EXTFLG		(1ULL << 7)	/* unwritten extents */
#define XFS_FEAT_ASCIICI	(1ULL << 8)	/* ASCII only case-insens. */
#define XFS_FEAT_LAZYSBCOUNT	(1ULL << 9)	/* Superblk counters */
#define XFS_FEAT_ATTR2		(1ULL << 10)	/* dynamic attr fork */
#define XFS_FEAT_PARENT		(1ULL << 11)	/* parent pointers */
#define XFS_FEAT_PROJID32	(1ULL << 12)	/* 32 bit project id */
#define XFS_FEAT_CRC		(1ULL << 13)	/* metadata CRCs */
#define XFS_FEAT_V3INODES	(1ULL << 14)	/* Version 3 inodes */
#define XFS_FEAT_PQUOTINO	(1ULL << 15)	/* non-shared proj/grp quotas */
#define XFS_FEAT_FTYPE		(1ULL << 16)	/* inode type in dir */
#define XFS_FEAT_FINOBT		(1ULL << 17)	/* free inode btree */
#define XFS_FEAT_RMAPBT		(1ULL << 18)	/* reverse map btree */
#define XFS_FEAT_REFLINK	(1ULL << 19)	/* reflinked files */
#define XFS_FEAT_SPINODES	(1ULL << 20)	/* sparse inode chunks */
#define XFS_FEAT_META_UUID	(1ULL << 21)	/* metadata UUID */
#define XFS_FEAT_REALTIME	(1ULL << 22)	/* realtime device present */
#define XFS_FEAT_INOBTCNT	(1ULL << 23)	/* inobt block counts */
#define XFS_FEAT_BIGTIME	(1ULL << 24)	/* large timestamps */
#define XFS_FEAT_NEEDSREPAIR	(1ULL << 25)	/* needs xfs_repair */
#define XFS_FEAT_NREXT64	(1ULL << 26)	/* large extent counters */
#define XFS_FEAT_EXCHANGE_RANGE	(1ULL << 27)	/* exchange range */
#define XFS_FEAT_METADIR	(1ULL << 28)	/* metadata directory tree */
#define XFS_FEAT_ZONED		(1ULL << 29)	/* zoned RT device */

#define __XFS_HAS_FEAT(name, NAME) \
static inline bool xfs_has_ ## name (const struct xfs_mount *mp) \
{ \
	return mp->m_features & XFS_FEAT_ ## NAME; \
}

/* Some features can be added dynamically so they need a set wrapper, too. */
#define __XFS_ADD_FEAT(name, NAME) \
	__XFS_HAS_FEAT(name, NAME); \
static inline void xfs_add_ ## name (struct xfs_mount *mp) \
{ \
	mp->m_features |= XFS_FEAT_ ## NAME; \
	xfs_sb_version_add ## name(&mp->m_sb); \
}

/* Superblock features */
__XFS_ADD_FEAT(attr, ATTR)
__XFS_HAS_FEAT(nlink, NLINK)
__XFS_ADD_FEAT(quota, QUOTA)
__XFS_HAS_FEAT(align, ALIGN)
__XFS_HAS_FEAT(dalign, DALIGN)
__XFS_HAS_FEAT(logv2, LOGV2)
__XFS_HAS_FEAT(sector, SECTOR)
__XFS_HAS_FEAT(extflg, EXTFLG)
__XFS_HAS_FEAT(asciici, ASCIICI)
__XFS_HAS_FEAT(lazysbcount, LAZYSBCOUNT)
__XFS_ADD_FEAT(attr2, ATTR2)
__XFS_HAS_FEAT(parent, PARENT)
__XFS_ADD_FEAT(projid32, PROJID32)
__XFS_HAS_FEAT(crc, CRC)
__XFS_HAS_FEAT(v3inodes, V3INODES)
__XFS_HAS_FEAT(pquotino, PQUOTINO)
__XFS_HAS_FEAT(ftype, FTYPE)
__XFS_HAS_FEAT(finobt, FINOBT)
__XFS_HAS_FEAT(rmapbt, RMAPBT)
__XFS_HAS_FEAT(reflink, REFLINK)
__XFS_HAS_FEAT(sparseinodes, SPINODES)
__XFS_HAS_FEAT(metauuid, META_UUID)
__XFS_HAS_FEAT(realtime, REALTIME)
__XFS_HAS_FEAT(inobtcounts, INOBTCNT)
__XFS_HAS_FEAT(bigtime, BIGTIME)
__XFS_HAS_FEAT(needsrepair, NEEDSREPAIR)
__XFS_HAS_FEAT(large_extent_counts, NREXT64)
__XFS_HAS_FEAT(exchange_range, EXCHANGE_RANGE)
__XFS_HAS_FEAT(metadir, METADIR)
__XFS_HAS_FEAT(zoned, ZONED)

static inline bool xfs_has_rtgroups(const struct xfs_mount *mp)
{
	/* all metadir file systems also allow rtgroups */
	return xfs_has_metadir(mp);
}

static inline bool xfs_has_rtsb(const struct xfs_mount *mp)
{
	/* all rtgroups filesystems with an rt section have an rtsb */
	return xfs_has_rtgroups(mp) &&
		xfs_has_realtime(mp) &&
		!xfs_has_zoned(mp);
}

static inline bool xfs_has_rtrmapbt(const struct xfs_mount *mp)
{
	return xfs_has_rtgroups(mp) && xfs_has_realtime(mp) &&
	       xfs_has_rmapbt(mp);
}

static inline bool xfs_has_rtreflink(const struct xfs_mount *mp)
{
	return xfs_has_metadir(mp) && xfs_has_realtime(mp) &&
	       xfs_has_reflink(mp);
}

static inline bool xfs_has_nonzoned(const struct xfs_mount *mp)
{
	return !xfs_has_zoned(mp);
}

/* Kernel mount features that we don't support */
#define __XFS_UNSUPP_FEAT(name) \
static inline bool xfs_has_ ## name (const struct xfs_mount *mp) \
{ \
	return false; \
}
__XFS_UNSUPP_FEAT(wsync)
__XFS_UNSUPP_FEAT(noattr2)
__XFS_UNSUPP_FEAT(ikeep)
__XFS_UNSUPP_FEAT(swalloc)
__XFS_UNSUPP_FEAT(small_inums)
__XFS_UNSUPP_FEAT(readonly)
__XFS_UNSUPP_FEAT(grpid)

/* Operational mount state flags */
#define XFS_OPSTATE_INODE32		0	/* inode32 allocator active */
#define XFS_OPSTATE_DEBUGGER		1	/* is this the debugger? */
#define XFS_OPSTATE_REPORT_CORRUPTION	2	/* report buffer corruption? */
#define XFS_OPSTATE_PERAG_DATA_LOADED	3	/* per-AG data initialized? */
#define XFS_OPSTATE_RTGROUP_DATA_LOADED	4	/* rtgroup data initialized? */

#define __XFS_IS_OPSTATE(name, NAME) \
static inline bool xfs_is_ ## name (struct xfs_mount *mp) \
{ \
	return (mp)->m_opstate & (1UL << XFS_OPSTATE_ ## NAME); \
} \
static inline bool xfs_clear_ ## name (struct xfs_mount *mp) \
{ \
	bool	ret = xfs_is_ ## name(mp); \
\
	(mp)->m_opstate &= ~(1UL << XFS_OPSTATE_ ## NAME); \
	return ret; \
} \
static inline bool xfs_set_ ## name (struct xfs_mount *mp) \
{ \
	bool	ret = xfs_is_ ## name(mp); \
\
	(mp)->m_opstate |= (1UL << XFS_OPSTATE_ ## NAME); \
	return ret; \
}

__XFS_IS_OPSTATE(inode32, INODE32)
__XFS_IS_OPSTATE(debugger, DEBUGGER)
__XFS_IS_OPSTATE(reporting_corruption, REPORT_CORRUPTION)
__XFS_IS_OPSTATE(perag_data_loaded, PERAG_DATA_LOADED)
__XFS_IS_OPSTATE(rtgroup_data_loaded, RTGROUP_DATA_LOADED)

#define __XFS_UNSUPP_OPSTATE(name) \
static inline bool xfs_is_ ## name (struct xfs_mount *mp) \
{ \
	return false; \
}
__XFS_UNSUPP_OPSTATE(readonly)
__XFS_UNSUPP_OPSTATE(shutdown)

static inline int64_t xfs_sum_freecounter(struct xfs_mount *mp,
		enum xfs_free_counter ctr)
{
	if (ctr == XC_FREE_RTEXTENTS)
		return mp->m_sb.sb_frextents;
	return mp->m_sb.sb_fdblocks;
}

static inline int64_t xfs_estimate_freecounter(struct xfs_mount *mp,
		enum xfs_free_counter ctr)
{
	return xfs_sum_freecounter(mp, ctr);
}

static inline int xfs_compare_freecounter(struct xfs_mount *mp,
		enum xfs_free_counter ctr, int64_t rhs, int32_t batch)
{
	uint64_t count;

	if (ctr == XC_FREE_RTEXTENTS)
		count = mp->m_sb.sb_frextents;
	else
		count = mp->m_sb.sb_fdblocks;
	if (count > rhs)
		return 1;
	else if (count < rhs)
		return -1;
	return 0;
}

/* don't fail on device size or AG count checks */
#define LIBXFS_MOUNT_DEBUGGER		(1U << 0)
/* report metadata corruption to stdout */
#define LIBXFS_MOUNT_REPORT_CORRUPTION	(1U << 1)

#define LIBXFS_BHASHSIZE(sbp) 		(1<<10)

void libxfs_compute_all_maxlevels(struct xfs_mount *mp);
struct xfs_mount *libxfs_mount(struct xfs_mount *mp, struct xfs_sb *sb,
		struct libxfs_init *xi, unsigned int flags);
int libxfs_flush_mount(struct xfs_mount *mp);
int		libxfs_umount(struct xfs_mount *mp);
extern void	libxfs_rtmount_destroy (xfs_mount_t *);

/* Dummy xfs_dquot so that libxfs compiles. */
struct xfs_dquot {
	int		q_type;
};

typedef struct wait_queue_head {
} wait_queue_head_t;

static inline void wake_up(wait_queue_head_t *wq) {}

struct xfs_defer_drain { /* empty */ };

#define xfs_defer_drain_init(dr)		((void)0)
#define xfs_defer_drain_free(dr)		((void)0)

#define xfs_group_intent_get(mp, fsbno, type) \
	xfs_group_get_by_fsb((mp), (fsbno), (type))
#define xfs_group_intent_put(xg)		xfs_group_put(xg)

static inline void xfs_group_intent_hold(struct xfs_group *xg) {}
static inline void xfs_group_intent_rele(struct xfs_group *xg) {}

static inline void libxfs_buftarg_drain(struct xfs_buftarg *btp)
{
	cache_purge(btp->bcache);
}

struct mnt_idmap {
	/* empty */
};

/* bogus idmapping so that mkfs can do directory inheritance correctly */
#define libxfs_nop_idmap	((struct mnt_idmap *)1)

#endif	/* __XFS_MOUNT_H__ */
