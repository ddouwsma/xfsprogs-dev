// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_dir2.h"
#include "xfs_inode.h"
#include "xfs_btree.h"
#include "xfs_trans.h"
#include "xfs_alloc.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_errortag.h"
#include "xfs_trans_space.h"
#include "xfs_trace.h"
#include "xfs_attr_leaf.h"
#include "xfs_quota_defs.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_refcount.h"
#include "xfs_rtbitmap.h"
#include "xfs_health.h"
#include "defer_item.h"
#include "xfs_symlink_remote.h"
#include "xfs_inode_util.h"
#include "xfs_rtgroup.h"

struct kmem_cache		*xfs_bmap_intent_cache;

/*
 * Miscellaneous helper functions
 */

/*
 * Compute and fill in the value of the maximum depth of a bmap btree
 * in this filesystem.  Done once, during mount.
 */
void
xfs_bmap_compute_maxlevels(
	xfs_mount_t	*mp,		/* file system mount structure */
	int		whichfork)	/* data or attr fork */
{
	uint64_t	maxblocks;	/* max blocks at this level */
	xfs_extnum_t	maxleafents;	/* max leaf entries possible */
	int		level;		/* btree level */
	int		maxrootrecs;	/* max records in root block */
	int		minleafrecs;	/* min records in leaf block */
	int		minnoderecs;	/* min records in node block */
	int		sz;		/* root block size */

	/*
	 * The maximum number of extents in a fork, hence the maximum number of
	 * leaf entries, is controlled by the size of the on-disk extent count.
	 *
	 * Note that we can no longer assume that if we are in ATTR1 that the
	 * fork offset of all the inodes will be
	 * (xfs_default_attroffset(ip) >> 3) because we could have mounted with
	 * ATTR2 and then mounted back with ATTR1, keeping the i_forkoff's fixed
	 * but probably at various positions. Therefore, for both ATTR1 and
	 * ATTR2 we have to assume the worst case scenario of a minimum size
	 * available.
	 */
	maxleafents = xfs_iext_max_nextents(xfs_has_large_extent_counts(mp),
				whichfork);
	if (whichfork == XFS_DATA_FORK)
		sz = xfs_bmdr_space_calc(MINDBTPTRS);
	else
		sz = xfs_bmdr_space_calc(MINABTPTRS);

	maxrootrecs = xfs_bmdr_maxrecs(sz, 0);
	minleafrecs = mp->m_bmap_dmnr[0];
	minnoderecs = mp->m_bmap_dmnr[1];
	maxblocks = howmany_64(maxleafents, minleafrecs);
	for (level = 1; maxblocks > 1; level++) {
		if (maxblocks <= maxrootrecs)
			maxblocks = 1;
		else
			maxblocks = howmany_64(maxblocks, minnoderecs);
	}
	mp->m_bm_maxlevels[whichfork] = level;
	ASSERT(mp->m_bm_maxlevels[whichfork] <= xfs_bmbt_maxlevels_ondisk());
}

unsigned int
xfs_bmap_compute_attr_offset(
	struct xfs_mount	*mp)
{
	if (mp->m_sb.sb_inodesize == 256)
		return XFS_LITINO(mp) - xfs_bmdr_space_calc(MINABTPTRS);
	return xfs_bmdr_space_calc(6 * MINABTPTRS);
}

STATIC int				/* error */
xfs_bmbt_lookup_eq(
	struct xfs_btree_cur	*cur,
	struct xfs_bmbt_irec	*irec,
	int			*stat)	/* success/failure */
{
	cur->bc_rec.b = *irec;
	return xfs_btree_lookup(cur, XFS_LOOKUP_EQ, stat);
}

STATIC int				/* error */
xfs_bmbt_lookup_first(
	struct xfs_btree_cur	*cur,
	int			*stat)	/* success/failure */
{
	cur->bc_rec.b.br_startoff = 0;
	cur->bc_rec.b.br_startblock = 0;
	cur->bc_rec.b.br_blockcount = 0;
	return xfs_btree_lookup(cur, XFS_LOOKUP_GE, stat);
}

/*
 * Check if the inode needs to be converted to btree format.
 */
static inline bool xfs_bmap_needs_btree(struct xfs_inode *ip, int whichfork)
{
	struct xfs_ifork *ifp = xfs_ifork_ptr(ip, whichfork);

	return whichfork != XFS_COW_FORK &&
		ifp->if_format == XFS_DINODE_FMT_EXTENTS &&
		ifp->if_nextents > XFS_IFORK_MAXEXT(ip, whichfork);
}

/*
 * Check if the inode should be converted to extent format.
 */
static inline bool xfs_bmap_wants_extents(struct xfs_inode *ip, int whichfork)
{
	struct xfs_ifork *ifp = xfs_ifork_ptr(ip, whichfork);

	return whichfork != XFS_COW_FORK &&
		ifp->if_format == XFS_DINODE_FMT_BTREE &&
		ifp->if_nextents <= XFS_IFORK_MAXEXT(ip, whichfork);
}

/*
 * Update the record referred to by cur to the value given by irec
 * This either works (return 0) or gets an EFSCORRUPTED error.
 */
STATIC int
xfs_bmbt_update(
	struct xfs_btree_cur	*cur,
	struct xfs_bmbt_irec	*irec)
{
	union xfs_btree_rec	rec;

	xfs_bmbt_disk_set_all(&rec.bmbt, irec);
	return xfs_btree_update(cur, &rec);
}

/*
 * Compute the worst-case number of indirect blocks that will be used
 * for ip's delayed extent of length "len".
 */
xfs_filblks_t
xfs_bmap_worst_indlen(
	struct xfs_inode	*ip,		/* incore inode pointer */
	xfs_filblks_t		len)		/* delayed extent length */
{
	struct xfs_mount	*mp = ip->i_mount;
	int			maxrecs = mp->m_bmap_dmxr[0];
	int			level;
	xfs_filblks_t		rval;

	for (level = 0, rval = 0;
	     level < XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK);
	     level++) {
		len += maxrecs - 1;
		do_div(len, maxrecs);
		rval += len;
		if (len == 1)
			return rval + XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK) -
				level - 1;
		if (level == 0)
			maxrecs = mp->m_bmap_dmxr[1];
	}
	return rval;
}

/*
 * Calculate the default attribute fork offset for newly created inodes.
 */
uint
xfs_default_attroffset(
	struct xfs_inode	*ip)
{
	if (ip->i_df.if_format == XFS_DINODE_FMT_DEV)
		return roundup(sizeof(xfs_dev_t), 8);
	return M_IGEO(ip->i_mount)->attr_fork_offset;
}

/*
 * Helper routine to reset inode i_forkoff field when switching attribute fork
 * from local to extent format - we reset it where possible to make space
 * available for inline data fork extents.
 */
STATIC void
xfs_bmap_forkoff_reset(
	xfs_inode_t	*ip,
	int		whichfork)
{
	if (whichfork == XFS_ATTR_FORK &&
	    ip->i_df.if_format != XFS_DINODE_FMT_DEV &&
	    ip->i_df.if_format != XFS_DINODE_FMT_BTREE) {
		uint	dfl_forkoff = xfs_default_attroffset(ip) >> 3;

		if (dfl_forkoff > ip->i_forkoff)
			ip->i_forkoff = dfl_forkoff;
	}
}

static int
xfs_bmap_read_buf(
	struct xfs_mount	*mp,		/* file system mount point */
	struct xfs_trans	*tp,		/* transaction pointer */
	xfs_fsblock_t		fsbno,		/* file system block number */
	struct xfs_buf		**bpp)		/* buffer for fsbno */
{
	struct xfs_buf		*bp;		/* return value */
	int			error;

	if (!xfs_verify_fsbno(mp, fsbno))
		return -EFSCORRUPTED;
	error = xfs_trans_read_buf(mp, tp, mp->m_ddev_targp,
			XFS_FSB_TO_DADDR(mp, fsbno), mp->m_bsize, 0, &bp,
			&xfs_bmbt_buf_ops);
	if (!error) {
		xfs_buf_set_ref(bp, XFS_BMAP_BTREE_REF);
		*bpp = bp;
	}
	return error;
}

#ifdef DEBUG
STATIC struct xfs_buf *
xfs_bmap_get_bp(
	struct xfs_btree_cur	*cur,
	xfs_fsblock_t		bno)
{
	struct xfs_log_item	*lip;
	int			i;

	if (!cur)
		return NULL;

	for (i = 0; i < cur->bc_maxlevels; i++) {
		if (!cur->bc_levels[i].bp)
			break;
		if (xfs_buf_daddr(cur->bc_levels[i].bp) == bno)
			return cur->bc_levels[i].bp;
	}

	/* Chase down all the log items to see if the bp is there */
	list_for_each_entry(lip, &cur->bc_tp->t_items, li_trans) {
		struct xfs_buf_log_item	*bip = (struct xfs_buf_log_item *)lip;

		if (bip->bli_item.li_type == XFS_LI_BUF &&
		    xfs_buf_daddr(bip->bli_buf) == bno)
			return bip->bli_buf;
	}

	return NULL;
}

STATIC void
xfs_check_block(
	struct xfs_btree_block	*block,
	xfs_mount_t		*mp,
	int			root,
	short			sz)
{
	int			i, j, dmxr;
	__be64			*pp, *thispa;	/* pointer to block address */
	xfs_bmbt_key_t		*prevp, *keyp;

	ASSERT(be16_to_cpu(block->bb_level) > 0);

	prevp = NULL;
	for( i = 1; i <= xfs_btree_get_numrecs(block); i++) {
		dmxr = mp->m_bmap_dmxr[0];
		keyp = xfs_bmbt_key_addr(mp, block, i);

		if (prevp) {
			ASSERT(be64_to_cpu(prevp->br_startoff) <
			       be64_to_cpu(keyp->br_startoff));
		}
		prevp = keyp;

		/*
		 * Compare the block numbers to see if there are dups.
		 */
		if (root)
			pp = xfs_bmap_broot_ptr_addr(mp, block, i, sz);
		else
			pp = xfs_bmbt_ptr_addr(mp, block, i, dmxr);

		for (j = i+1; j <= be16_to_cpu(block->bb_numrecs); j++) {
			if (root)
				thispa = xfs_bmap_broot_ptr_addr(mp, block, j, sz);
			else
				thispa = xfs_bmbt_ptr_addr(mp, block, j, dmxr);
			if (*thispa == *pp) {
				xfs_warn(mp, "%s: thispa(%d) == pp(%d) %lld",
					__func__, j, i,
					(unsigned long long)be64_to_cpu(*thispa));
				xfs_err(mp, "%s: ptrs are equal in node\n",
					__func__);
				xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);
			}
		}
	}
}

/*
 * Check that the extents for the inode ip are in the right order in all
 * btree leaves. THis becomes prohibitively expensive for large extent count
 * files, so don't bother with inodes that have more than 10,000 extents in
 * them. The btree record ordering checks will still be done, so for such large
 * bmapbt constructs that is going to catch most corruptions.
 */
STATIC void
xfs_bmap_check_leaf_extents(
	struct xfs_btree_cur	*cur,	/* btree cursor or null */
	xfs_inode_t		*ip,		/* incore inode pointer */
	int			whichfork)	/* data or attr fork */
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_btree_block	*block;	/* current btree block */
	xfs_fsblock_t		bno;	/* block # of "block" */
	struct xfs_buf		*bp;	/* buffer for "block" */
	int			error;	/* error return value */
	xfs_extnum_t		i=0, j;	/* index into the extents list */
	int			level;	/* btree level, for checking */
	__be64			*pp;	/* pointer to block address */
	xfs_bmbt_rec_t		*ep;	/* pointer to current extent */
	xfs_bmbt_rec_t		last = {0, 0}; /* last extent in prev block */
	xfs_bmbt_rec_t		*nextp;	/* pointer to next extent */
	int			bp_release = 0;

	if (ifp->if_format != XFS_DINODE_FMT_BTREE)
		return;

	/* skip large extent count inodes */
	if (ip->i_df.if_nextents > 10000)
		return;

	bno = NULLFSBLOCK;
	block = ifp->if_broot;
	/*
	 * Root level must use BMAP_BROOT_PTR_ADDR macro to get ptr out.
	 */
	level = be16_to_cpu(block->bb_level);
	ASSERT(level > 0);
	xfs_check_block(block, mp, 1, ifp->if_broot_bytes);
	pp = xfs_bmap_broot_ptr_addr(mp, block, 1, ifp->if_broot_bytes);
	bno = be64_to_cpu(*pp);

	ASSERT(bno != NULLFSBLOCK);
	ASSERT(XFS_FSB_TO_AGNO(mp, bno) < mp->m_sb.sb_agcount);
	ASSERT(XFS_FSB_TO_AGBNO(mp, bno) < mp->m_sb.sb_agblocks);

	/*
	 * Go down the tree until leaf level is reached, following the first
	 * pointer (leftmost) at each level.
	 */
	while (level-- > 0) {
		/* See if buf is in cur first */
		bp_release = 0;
		bp = xfs_bmap_get_bp(cur, XFS_FSB_TO_DADDR(mp, bno));
		if (!bp) {
			bp_release = 1;
			error = xfs_bmap_read_buf(mp, NULL, bno, &bp);
			if (xfs_metadata_is_sick(error))
				xfs_btree_mark_sick(cur);
			if (error)
				goto error_norelse;
		}
		block = XFS_BUF_TO_BLOCK(bp);
		if (level == 0)
			break;

		/*
		 * Check this block for basic sanity (increasing keys and
		 * no duplicate blocks).
		 */

		xfs_check_block(block, mp, 0, 0);
		pp = xfs_bmbt_ptr_addr(mp, block, 1, mp->m_bmap_dmxr[1]);
		bno = be64_to_cpu(*pp);
		if (XFS_IS_CORRUPT(mp, !xfs_verify_fsbno(mp, bno))) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}
		if (bp_release) {
			bp_release = 0;
			xfs_trans_brelse(NULL, bp);
		}
	}

	/*
	 * Here with bp and block set to the leftmost leaf node in the tree.
	 */
	i = 0;

	/*
	 * Loop over all leaf nodes checking that all extents are in the right order.
	 */
	for (;;) {
		xfs_fsblock_t	nextbno;
		xfs_extnum_t	num_recs;


		num_recs = xfs_btree_get_numrecs(block);

		/*
		 * Read-ahead the next leaf block, if any.
		 */

		nextbno = be64_to_cpu(block->bb_u.l.bb_rightsib);

		/*
		 * Check all the extents to make sure they are OK.
		 * If we had a previous block, the last entry should
		 * conform with the first entry in this one.
		 */

		ep = xfs_bmbt_rec_addr(mp, block, 1);
		if (i) {
			ASSERT(xfs_bmbt_disk_get_startoff(&last) +
			       xfs_bmbt_disk_get_blockcount(&last) <=
			       xfs_bmbt_disk_get_startoff(ep));
		}
		for (j = 1; j < num_recs; j++) {
			nextp = xfs_bmbt_rec_addr(mp, block, j + 1);
			ASSERT(xfs_bmbt_disk_get_startoff(ep) +
			       xfs_bmbt_disk_get_blockcount(ep) <=
			       xfs_bmbt_disk_get_startoff(nextp));
			ep = nextp;
		}

		last = *ep;
		i += num_recs;
		if (bp_release) {
			bp_release = 0;
			xfs_trans_brelse(NULL, bp);
		}
		bno = nextbno;
		/*
		 * If we've reached the end, stop.
		 */
		if (bno == NULLFSBLOCK)
			break;

		bp_release = 0;
		bp = xfs_bmap_get_bp(cur, XFS_FSB_TO_DADDR(mp, bno));
		if (!bp) {
			bp_release = 1;
			error = xfs_bmap_read_buf(mp, NULL, bno, &bp);
			if (xfs_metadata_is_sick(error))
				xfs_btree_mark_sick(cur);
			if (error)
				goto error_norelse;
		}
		block = XFS_BUF_TO_BLOCK(bp);
	}

	return;

error0:
	xfs_warn(mp, "%s: at error0", __func__);
	if (bp_release)
		xfs_trans_brelse(NULL, bp);
error_norelse:
	xfs_warn(mp, "%s: BAD after btree leaves for %llu extents",
		__func__, i);
	xfs_err(mp, "%s: CORRUPTED BTREE OR SOMETHING", __func__);
	xfs_force_shutdown(mp, SHUTDOWN_CORRUPT_INCORE);
	return;
}

/*
 * Validate that the bmbt_irecs being returned from bmapi are valid
 * given the caller's original parameters.  Specifically check the
 * ranges of the returned irecs to ensure that they only extend beyond
 * the given parameters if the XFS_BMAPI_ENTIRE flag was set.
 */
STATIC void
xfs_bmap_validate_ret(
	xfs_fileoff_t		bno,
	xfs_filblks_t		len,
	uint32_t		flags,
	xfs_bmbt_irec_t		*mval,
	int			nmap,
	int			ret_nmap)
{
	int			i;		/* index to map values */

	ASSERT(ret_nmap <= nmap);

	for (i = 0; i < ret_nmap; i++) {
		ASSERT(mval[i].br_blockcount > 0);
		if (!(flags & XFS_BMAPI_ENTIRE)) {
			ASSERT(mval[i].br_startoff >= bno);
			ASSERT(mval[i].br_blockcount <= len);
			ASSERT(mval[i].br_startoff + mval[i].br_blockcount <=
			       bno + len);
		} else {
			ASSERT(mval[i].br_startoff < bno + len);
			ASSERT(mval[i].br_startoff + mval[i].br_blockcount >
			       bno);
		}
		ASSERT(i == 0 ||
		       mval[i - 1].br_startoff + mval[i - 1].br_blockcount ==
		       mval[i].br_startoff);
		ASSERT(mval[i].br_startblock != DELAYSTARTBLOCK &&
		       mval[i].br_startblock != HOLESTARTBLOCK);
		ASSERT(mval[i].br_state == XFS_EXT_NORM ||
		       mval[i].br_state == XFS_EXT_UNWRITTEN);
	}
}

#else
#define xfs_bmap_check_leaf_extents(cur, ip, whichfork)		do { } while (0)
#define	xfs_bmap_validate_ret(bno,len,flags,mval,onmap,nmap)	do { } while (0)
#endif /* DEBUG */

/*
 * Inode fork format manipulation functions
 */

/*
 * Convert the inode format to extent format if it currently is in btree format,
 * but the extent list is small enough that it fits into the extent format.
 *
 * Since the extents are already in-core, all we have to do is give up the space
 * for the btree root and pitch the leaf block.
 */
STATIC int				/* error */
xfs_bmap_btree_to_extents(
	struct xfs_trans	*tp,	/* transaction pointer */
	struct xfs_inode	*ip,	/* incore inode pointer */
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			*logflagsp, /* inode logging flags */
	int			whichfork)  /* data or attr fork */
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_btree_block	*rblock = ifp->if_broot;
	struct xfs_btree_block	*cblock;/* child btree block */
	xfs_fsblock_t		cbno;	/* child block number */
	struct xfs_buf		*cbp;	/* child block's buffer */
	int			error;	/* error return value */
	__be64			*pp;	/* ptr to block address */
	struct xfs_owner_info	oinfo;

	/* check if we actually need the extent format first: */
	if (!xfs_bmap_wants_extents(ip, whichfork))
		return 0;

	ASSERT(cur);
	ASSERT(whichfork != XFS_COW_FORK);
	ASSERT(ifp->if_format == XFS_DINODE_FMT_BTREE);
	ASSERT(be16_to_cpu(rblock->bb_level) == 1);
	ASSERT(be16_to_cpu(rblock->bb_numrecs) == 1);
	ASSERT(xfs_bmbt_maxrecs(mp, ifp->if_broot_bytes, false) == 1);

	pp = xfs_bmap_broot_ptr_addr(mp, rblock, 1, ifp->if_broot_bytes);
	cbno = be64_to_cpu(*pp);
#ifdef DEBUG
	if (XFS_IS_CORRUPT(cur->bc_mp, !xfs_verify_fsbno(mp, cbno))) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}
#endif
	error = xfs_bmap_read_buf(mp, tp, cbno, &cbp);
	if (xfs_metadata_is_sick(error))
		xfs_btree_mark_sick(cur);
	if (error)
		return error;
	cblock = XFS_BUF_TO_BLOCK(cbp);
	if ((error = xfs_btree_check_block(cur, cblock, 0, cbp)))
		return error;

	xfs_rmap_ino_bmbt_owner(&oinfo, ip->i_ino, whichfork);
	error = xfs_free_extent_later(cur->bc_tp, cbno, 1, &oinfo,
			XFS_AG_RESV_NONE, 0);
	if (error)
		return error;

	ip->i_nblocks--;
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_BCOUNT, -1L);
	xfs_trans_binval(tp, cbp);
	if (cur->bc_levels[0].bp == cbp)
		cur->bc_levels[0].bp = NULL;
	xfs_bmap_broot_realloc(ip, whichfork, 0);
	ASSERT(ifp->if_broot == NULL);
	ifp->if_format = XFS_DINODE_FMT_EXTENTS;
	*logflagsp |= XFS_ILOG_CORE | xfs_ilog_fext(whichfork);
	return 0;
}

/*
 * Convert an extents-format file into a btree-format file.
 * The new file will have a root block (in the inode) and a single child block.
 */
STATIC int					/* error */
xfs_bmap_extents_to_btree(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode pointer */
	struct xfs_btree_cur	**curp,		/* cursor returned to caller */
	int			wasdel,		/* converting a delayed alloc */
	int			*logflagsp,	/* inode logging flags */
	int			whichfork)	/* data or attr fork */
{
	struct xfs_btree_block	*ablock;	/* allocated (child) bt block */
	struct xfs_buf		*abp;		/* buffer for ablock */
	struct xfs_alloc_arg	args;		/* allocation arguments */
	struct xfs_bmbt_rec	*arp;		/* child record pointer */
	struct xfs_btree_block	*block;		/* btree root block */
	struct xfs_btree_cur	*cur;		/* bmap btree cursor */
	int			error;		/* error return value */
	struct xfs_ifork	*ifp;		/* inode fork pointer */
	struct xfs_bmbt_key	*kp;		/* root block key pointer */
	struct xfs_mount	*mp;		/* mount structure */
	xfs_bmbt_ptr_t		*pp;		/* root block address pointer */
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	rec;
	xfs_extnum_t		cnt = 0;

	mp = ip->i_mount;
	ASSERT(whichfork != XFS_COW_FORK);
	ifp = xfs_ifork_ptr(ip, whichfork);
	ASSERT(ifp->if_format == XFS_DINODE_FMT_EXTENTS);

	/*
	 * Make space in the inode incore. This needs to be undone if we fail
	 * to expand the root.
	 */
	block = xfs_bmap_broot_realloc(ip, whichfork, 1);

	/*
	 * Fill in the root.
	 */
	xfs_bmbt_init_block(ip, block, NULL, 1, 1);
	/*
	 * Need a cursor.  Can't allocate until bb_level is filled in.
	 */
	cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);
	if (wasdel)
		cur->bc_flags |= XFS_BTREE_BMBT_WASDEL;
	/*
	 * Convert to a btree with two levels, one record in root.
	 */
	ifp->if_format = XFS_DINODE_FMT_BTREE;
	memset(&args, 0, sizeof(args));
	args.tp = tp;
	args.mp = mp;
	xfs_rmap_ino_bmbt_owner(&args.oinfo, ip->i_ino, whichfork);

	args.minlen = args.maxlen = args.prod = 1;
	args.wasdel = wasdel;
	*logflagsp = 0;
	error = xfs_alloc_vextent_start_ag(&args,
				XFS_INO_TO_FSB(mp, ip->i_ino));
	if (error)
		goto out_root_realloc;

	/*
	 * Allocation can't fail, the space was reserved.
	 */
	if (WARN_ON_ONCE(args.fsbno == NULLFSBLOCK)) {
		error = -ENOSPC;
		goto out_root_realloc;
	}

	cur->bc_bmap.allocated++;
	ip->i_nblocks++;
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_BCOUNT, 1L);
	error = xfs_trans_get_buf(tp, mp->m_ddev_targp,
			XFS_FSB_TO_DADDR(mp, args.fsbno),
			mp->m_bsize, 0, &abp);
	if (error)
		goto out_unreserve_dquot;

	/*
	 * Fill in the child block.
	 */
	ablock = XFS_BUF_TO_BLOCK(abp);
	xfs_bmbt_init_block(ip, ablock, abp, 0, 0);

	for_each_xfs_iext(ifp, &icur, &rec) {
		if (isnullstartblock(rec.br_startblock))
			continue;
		arp = xfs_bmbt_rec_addr(mp, ablock, 1 + cnt);
		xfs_bmbt_disk_set_all(arp, &rec);
		cnt++;
	}
	ASSERT(cnt == ifp->if_nextents);
	xfs_btree_set_numrecs(ablock, cnt);

	/*
	 * Fill in the root key and pointer.
	 */
	kp = xfs_bmbt_key_addr(mp, block, 1);
	arp = xfs_bmbt_rec_addr(mp, ablock, 1);
	kp->br_startoff = cpu_to_be64(xfs_bmbt_disk_get_startoff(arp));
	pp = xfs_bmbt_ptr_addr(mp, block, 1, xfs_bmbt_get_maxrecs(cur,
						be16_to_cpu(block->bb_level)));
	*pp = cpu_to_be64(args.fsbno);

	/*
	 * Do all this logging at the end so that
	 * the root is at the right level.
	 */
	xfs_btree_log_block(cur, abp, XFS_BB_ALL_BITS);
	xfs_btree_log_recs(cur, abp, 1, be16_to_cpu(ablock->bb_numrecs));
	ASSERT(*curp == NULL);
	*curp = cur;
	*logflagsp = XFS_ILOG_CORE | xfs_ilog_fbroot(whichfork);
	return 0;

out_unreserve_dquot:
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_BCOUNT, -1L);
out_root_realloc:
	xfs_bmap_broot_realloc(ip, whichfork, 0);
	ifp->if_format = XFS_DINODE_FMT_EXTENTS;
	ASSERT(ifp->if_broot == NULL);
	xfs_btree_del_cursor(cur, XFS_BTREE_ERROR);

	return error;
}

/*
 * Convert a local file to an extents file.
 * This code is out of bounds for data forks of regular files,
 * since the file data needs to get logged so things will stay consistent.
 * (The bmap-level manipulations are ok, though).
 */
void
xfs_bmap_local_to_extents_empty(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);

	ASSERT(whichfork != XFS_COW_FORK);
	ASSERT(ifp->if_format == XFS_DINODE_FMT_LOCAL);
	ASSERT(ifp->if_bytes == 0);
	ASSERT(ifp->if_nextents == 0);

	xfs_bmap_forkoff_reset(ip, whichfork);
	ifp->if_data = NULL;
	ifp->if_height = 0;
	ifp->if_format = XFS_DINODE_FMT_EXTENTS;
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
}


int					/* error */
xfs_bmap_local_to_extents(
	xfs_trans_t	*tp,		/* transaction pointer */
	xfs_inode_t	*ip,		/* incore inode pointer */
	xfs_extlen_t	total,		/* total blocks needed by transaction */
	int		*logflagsp,	/* inode logging flags */
	int		whichfork,
	void		(*init_fn)(struct xfs_trans *tp,
				   struct xfs_buf *bp,
				   struct xfs_inode *ip,
				   struct xfs_ifork *ifp, void *priv),
	void		*priv)
{
	int		error = 0;
	int		flags;		/* logging flags returned */
	struct xfs_ifork *ifp;		/* inode fork pointer */
	xfs_alloc_arg_t	args;		/* allocation arguments */
	struct xfs_buf	*bp;		/* buffer for extent block */
	struct xfs_bmbt_irec rec;
	struct xfs_iext_cursor icur;

	/*
	 * We don't want to deal with the case of keeping inode data inline yet.
	 * So sending the data fork of a regular inode is invalid.
	 */
	ASSERT(!(S_ISREG(VFS_I(ip)->i_mode) && whichfork == XFS_DATA_FORK));
	ifp = xfs_ifork_ptr(ip, whichfork);
	ASSERT(ifp->if_format == XFS_DINODE_FMT_LOCAL);

	if (!ifp->if_bytes) {
		xfs_bmap_local_to_extents_empty(tp, ip, whichfork);
		flags = XFS_ILOG_CORE;
		goto done;
	}

	flags = 0;
	error = 0;
	memset(&args, 0, sizeof(args));
	args.tp = tp;
	args.mp = ip->i_mount;
	args.total = total;
	args.minlen = args.maxlen = args.prod = 1;
	xfs_rmap_ino_owner(&args.oinfo, ip->i_ino, whichfork, 0);

	/*
	 * Allocate a block.  We know we need only one, since the
	 * file currently fits in an inode.
	 */
	args.total = total;
	args.minlen = args.maxlen = args.prod = 1;
	error = xfs_alloc_vextent_start_ag(&args,
			XFS_INO_TO_FSB(args.mp, ip->i_ino));
	if (error)
		goto done;

	/* Can't fail, the space was reserved. */
	ASSERT(args.fsbno != NULLFSBLOCK);
	ASSERT(args.len == 1);
	error = xfs_trans_get_buf(tp, args.mp->m_ddev_targp,
			XFS_FSB_TO_DADDR(args.mp, args.fsbno),
			args.mp->m_bsize, 0, &bp);
	if (error)
		goto done;

	/*
	 * Initialize the block, copy the data and log the remote buffer.
	 *
	 * The callout is responsible for logging because the remote format
	 * might differ from the local format and thus we don't know how much to
	 * log here. Note that init_fn must also set the buffer log item type
	 * correctly.
	 */
	init_fn(tp, bp, ip, ifp, priv);

	/* account for the change in fork size */
	xfs_idata_realloc(ip, -ifp->if_bytes, whichfork);
	xfs_bmap_local_to_extents_empty(tp, ip, whichfork);
	flags |= XFS_ILOG_CORE;

	ifp->if_data = NULL;
	ifp->if_height = 0;

	rec.br_startoff = 0;
	rec.br_startblock = args.fsbno;
	rec.br_blockcount = 1;
	rec.br_state = XFS_EXT_NORM;
	xfs_iext_first(ifp, &icur);
	xfs_iext_insert(ip, &icur, &rec, 0);

	ifp->if_nextents = 1;
	ip->i_nblocks = 1;
	xfs_trans_mod_dquot_byino(tp, ip, XFS_TRANS_DQ_BCOUNT, 1L);
	flags |= xfs_ilog_fext(whichfork);

done:
	*logflagsp = flags;
	return error;
}

/*
 * Called from xfs_bmap_add_attrfork to handle btree format files.
 */
STATIC int					/* error */
xfs_bmap_add_attrfork_btree(
	xfs_trans_t		*tp,		/* transaction pointer */
	xfs_inode_t		*ip,		/* incore inode pointer */
	int			*flags)		/* inode logging flags */
{
	struct xfs_btree_block	*block = ip->i_df.if_broot;
	struct xfs_btree_cur	*cur;		/* btree cursor */
	int			error;		/* error return value */
	xfs_mount_t		*mp;		/* file system mount struct */
	int			stat;		/* newroot status */

	mp = ip->i_mount;

	if (xfs_bmap_bmdr_space(block) <= xfs_inode_data_fork_size(ip))
		*flags |= XFS_ILOG_DBROOT;
	else {
		cur = xfs_bmbt_init_cursor(mp, tp, ip, XFS_DATA_FORK);
		error = xfs_bmbt_lookup_first(cur, &stat);
		if (error)
			goto error0;
		/* must be at least one entry */
		if (XFS_IS_CORRUPT(mp, stat != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto error0;
		}
		if ((error = xfs_btree_new_iroot(cur, flags, &stat)))
			goto error0;
		if (stat == 0) {
			xfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
			return -ENOSPC;
		}
		cur->bc_bmap.allocated = 0;
		xfs_btree_del_cursor(cur, XFS_BTREE_NOERROR);
	}
	return 0;
error0:
	xfs_btree_del_cursor(cur, XFS_BTREE_ERROR);
	return error;
}

/*
 * Called from xfs_bmap_add_attrfork to handle extents format files.
 */
STATIC int					/* error */
xfs_bmap_add_attrfork_extents(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode pointer */
	int			*flags)		/* inode logging flags */
{
	struct xfs_btree_cur	*cur;		/* bmap btree cursor */
	int			error;		/* error return value */

	if (ip->i_df.if_nextents * sizeof(struct xfs_bmbt_rec) <=
	    xfs_inode_data_fork_size(ip))
		return 0;
	cur = NULL;
	error = xfs_bmap_extents_to_btree(tp, ip, &cur, 0, flags,
					  XFS_DATA_FORK);
	if (cur) {
		cur->bc_bmap.allocated = 0;
		xfs_btree_del_cursor(cur, error);
	}
	return error;
}

/*
 * Called from xfs_bmap_add_attrfork to handle local format files. Each
 * different data fork content type needs a different callout to do the
 * conversion. Some are basic and only require special block initialisation
 * callouts for the data formating, others (directories) are so specialised they
 * handle everything themselves.
 *
 * XXX (dgc): investigate whether directory conversion can use the generic
 * formatting callout. It should be possible - it's just a very complex
 * formatter.
 */
STATIC int					/* error */
xfs_bmap_add_attrfork_local(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode pointer */
	int			*flags)		/* inode logging flags */
{
	struct xfs_da_args	dargs;		/* args for dir/attr code */

	if (ip->i_df.if_bytes <= xfs_inode_data_fork_size(ip))
		return 0;

	if (S_ISDIR(VFS_I(ip)->i_mode)) {
		memset(&dargs, 0, sizeof(dargs));
		dargs.geo = ip->i_mount->m_dir_geo;
		dargs.dp = ip;
		dargs.total = dargs.geo->fsbcount;
		dargs.whichfork = XFS_DATA_FORK;
		dargs.trans = tp;
		dargs.owner = ip->i_ino;
		return xfs_dir2_sf_to_block(&dargs);
	}

	if (S_ISLNK(VFS_I(ip)->i_mode))
		return xfs_bmap_local_to_extents(tp, ip, 1, flags,
				XFS_DATA_FORK, xfs_symlink_local_to_remote,
				NULL);

	/* should only be called for types that support local format data */
	ASSERT(0);
	xfs_bmap_mark_sick(ip, XFS_ATTR_FORK);
	return -EFSCORRUPTED;
}

/*
 * Set an inode attr fork offset based on the format of the data fork.
 */
static int
xfs_bmap_set_attrforkoff(
	struct xfs_inode	*ip,
	int			size,
	int			*version)
{
	int			default_size = xfs_default_attroffset(ip) >> 3;

	switch (ip->i_df.if_format) {
	case XFS_DINODE_FMT_DEV:
		ip->i_forkoff = default_size;
		break;
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		ip->i_forkoff = xfs_attr_shortform_bytesfit(ip, size);
		if (!ip->i_forkoff)
			ip->i_forkoff = default_size;
		else if (xfs_has_attr2(ip->i_mount) && version)
			*version = 2;
		break;
	default:
		ASSERT(0);
		return -EINVAL;
	}

	return 0;
}

/*
 * Convert inode from non-attributed to attributed.  Caller must hold the
 * ILOCK_EXCL and the file cannot have an attr fork.
 */
int						/* error code */
xfs_bmap_add_attrfork(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,		/* incore inode pointer */
	int			size,		/* space new attribute needs */
	int			rsvd)		/* xact may use reserved blks */
{
	struct xfs_mount	*mp = tp->t_mountp;
	int			version = 1;	/* superblock attr version */
	int			logflags;	/* logging flags */
	int			error;		/* error return value */

	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);
	if (!xfs_is_metadir_inode(ip))
		ASSERT(!XFS_NOT_DQATTACHED(mp, ip));
	ASSERT(!xfs_inode_has_attr_fork(ip));

	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = xfs_bmap_set_attrforkoff(ip, size, &version);
	if (error)
		return error;

	xfs_ifork_init_attr(ip, XFS_DINODE_FMT_EXTENTS, 0);
	logflags = 0;
	switch (ip->i_df.if_format) {
	case XFS_DINODE_FMT_LOCAL:
		error = xfs_bmap_add_attrfork_local(tp, ip, &logflags);
		break;
	case XFS_DINODE_FMT_EXTENTS:
		error = xfs_bmap_add_attrfork_extents(tp, ip, &logflags);
		break;
	case XFS_DINODE_FMT_BTREE:
		error = xfs_bmap_add_attrfork_btree(tp, ip, &logflags);
		break;
	default:
		error = 0;
		break;
	}
	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	if (error)
		return error;
	if (!xfs_has_attr(mp) ||
	   (!xfs_has_attr2(mp) && version == 2)) {
		bool log_sb = false;

		spin_lock(&mp->m_sb_lock);
		if (!xfs_has_attr(mp)) {
			xfs_add_attr(mp);
			log_sb = true;
		}
		if (!xfs_has_attr2(mp) && version == 2) {
			xfs_add_attr2(mp);
			log_sb = true;
		}
		spin_unlock(&mp->m_sb_lock);
		if (log_sb)
			xfs_log_sb(tp);
	}

	return 0;
}

/*
 * Internal and external extent tree search functions.
 */

struct xfs_iread_state {
	struct xfs_iext_cursor	icur;
	xfs_extnum_t		loaded;
};

int
xfs_bmap_complain_bad_rec(
	struct xfs_inode		*ip,
	int				whichfork,
	xfs_failaddr_t			fa,
	const struct xfs_bmbt_irec	*irec)
{
	struct xfs_mount		*mp = ip->i_mount;
	const char			*forkname;

	switch (whichfork) {
	case XFS_DATA_FORK:	forkname = "data"; break;
	case XFS_ATTR_FORK:	forkname = "attr"; break;
	case XFS_COW_FORK:	forkname = "CoW"; break;
	default:		forkname = "???"; break;
	}

	xfs_warn(mp,
 "Bmap BTree record corruption in inode 0x%llx %s fork detected at %pS!",
				ip->i_ino, forkname, fa);
	xfs_warn(mp,
		"Offset 0x%llx, start block 0x%llx, block count 0x%llx state 0x%x",
		irec->br_startoff, irec->br_startblock, irec->br_blockcount,
		irec->br_state);

	return -EFSCORRUPTED;
}

/* Stuff every bmbt record from this block into the incore extent map. */
static int
xfs_iread_bmbt_block(
	struct xfs_btree_cur	*cur,
	int			level,
	void			*priv)
{
	struct xfs_iread_state	*ir = priv;
	struct xfs_mount	*mp = cur->bc_mp;
	struct xfs_inode	*ip = cur->bc_ino.ip;
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;
	struct xfs_bmbt_rec	*frp;
	xfs_extnum_t		num_recs;
	xfs_extnum_t		j;
	int			whichfork = cur->bc_ino.whichfork;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);

	block = xfs_btree_get_block(cur, level, &bp);

	/* Abort if we find more records than nextents. */
	num_recs = xfs_btree_get_numrecs(block);
	if (unlikely(ir->loaded + num_recs > ifp->if_nextents)) {
		xfs_warn(ip->i_mount, "corrupt dinode %llu, (btree extents).",
				(unsigned long long)ip->i_ino);
		xfs_inode_verifier_error(ip, -EFSCORRUPTED, __func__, block,
				sizeof(*block), __this_address);
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	/* Copy records into the incore cache. */
	frp = xfs_bmbt_rec_addr(mp, block, 1);
	for (j = 0; j < num_recs; j++, frp++, ir->loaded++) {
		struct xfs_bmbt_irec	new;
		xfs_failaddr_t		fa;

		xfs_bmbt_disk_get_all(frp, &new);
		fa = xfs_bmap_validate_extent(ip, whichfork, &new);
		if (fa) {
			xfs_inode_verifier_error(ip, -EFSCORRUPTED,
					"xfs_iread_extents(2)", frp,
					sizeof(*frp), fa);
			xfs_bmap_mark_sick(ip, whichfork);
			return xfs_bmap_complain_bad_rec(ip, whichfork, fa,
					&new);
		}
		xfs_iext_insert(ip, &ir->icur, &new,
				xfs_bmap_fork_to_state(whichfork));
		trace_xfs_read_extent(ip, &ir->icur,
				xfs_bmap_fork_to_state(whichfork), _THIS_IP_);
		xfs_iext_next(ifp, &ir->icur);
	}

	return 0;
}

/*
 * Read in extents from a btree-format inode.
 */
int
xfs_iread_extents(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xfs_iread_state	ir;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_btree_cur	*cur;
	int			error;

	if (!xfs_need_iread_extents(ifp))
		return 0;

	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);

	ir.loaded = 0;
	xfs_iext_first(ifp, &ir.icur);
	cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);
	error = xfs_btree_visit_blocks(cur, xfs_iread_bmbt_block,
			XFS_BTREE_VISIT_RECORDS, &ir);
	xfs_btree_del_cursor(cur, error);
	if (error)
		goto out;

	if (XFS_IS_CORRUPT(mp, ir.loaded != ifp->if_nextents)) {
		xfs_bmap_mark_sick(ip, whichfork);
		error = -EFSCORRUPTED;
		goto out;
	}
	ASSERT(ir.loaded == xfs_iext_count(ifp));
	/*
	 * Use release semantics so that we can use acquire semantics in
	 * xfs_need_iread_extents and be guaranteed to see a valid mapping tree
	 * after that load.
	 */
	smp_store_release(&ifp->if_needextents, 0);
	return 0;
out:
	if (xfs_metadata_is_sick(error))
		xfs_bmap_mark_sick(ip, whichfork);
	xfs_iext_destroy(ifp);
	return error;
}

/*
 * Returns the relative block number of the first unused block(s) in the given
 * fork with at least "len" logically contiguous blocks free.  This is the
 * lowest-address hole if the fork has holes, else the first block past the end
 * of fork.  Return 0 if the fork is currently local (in-inode).
 */
int						/* error */
xfs_bmap_first_unused(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode */
	xfs_extlen_t		len,		/* size of hole to find */
	xfs_fileoff_t		*first_unused,	/* unused block */
	int			whichfork)	/* data or attr fork */
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec	got;
	struct xfs_iext_cursor	icur;
	xfs_fileoff_t		lastaddr = 0;
	xfs_fileoff_t		lowest, max;
	int			error;

	if (ifp->if_format == XFS_DINODE_FMT_LOCAL) {
		*first_unused = 0;
		return 0;
	}

	ASSERT(xfs_ifork_has_extents(ifp));

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	lowest = max = *first_unused;
	for_each_xfs_iext(ifp, &icur, &got) {
		/*
		 * See if the hole before this extent will work.
		 */
		if (got.br_startoff >= lowest + len &&
		    got.br_startoff - max >= len)
			break;
		lastaddr = got.br_startoff + got.br_blockcount;
		max = XFS_FILEOFF_MAX(lastaddr, lowest);
	}

	*first_unused = max;
	return 0;
}

/*
 * Returns the file-relative block number of the last block - 1 before
 * last_block (input value) in the file.
 * This is not based on i_size, it is based on the extent records.
 * Returns 0 for local files, as they do not have extent records.
 */
int						/* error */
xfs_bmap_last_before(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode */
	xfs_fileoff_t		*last_block,	/* last block */
	int			whichfork)	/* data or attr fork */
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec	got;
	struct xfs_iext_cursor	icur;
	int			error;

	switch (ifp->if_format) {
	case XFS_DINODE_FMT_LOCAL:
		*last_block = 0;
		return 0;
	case XFS_DINODE_FMT_BTREE:
	case XFS_DINODE_FMT_EXTENTS:
		break;
	default:
		ASSERT(0);
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	if (!xfs_iext_lookup_extent_before(ip, ifp, last_block, &icur, &got))
		*last_block = 0;
	return 0;
}

int
xfs_bmap_last_extent(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*rec,
	int			*is_empty)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_iext_cursor	icur;
	int			error;

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	xfs_iext_last(ifp, &icur);
	if (!xfs_iext_get_extent(ifp, &icur, rec))
		*is_empty = 1;
	else
		*is_empty = 0;
	return 0;
}

/*
 * Check the last inode extent to determine whether this allocation will result
 * in blocks being allocated at the end of the file. When we allocate new data
 * blocks at the end of the file which do not start at the previous data block,
 * we will try to align the new blocks at stripe unit boundaries.
 *
 * Returns 1 in bma->aeof if the file (fork) is empty as any new write will be
 * at, or past the EOF.
 */
STATIC int
xfs_bmap_isaeof(
	struct xfs_bmalloca	*bma,
	int			whichfork)
{
	struct xfs_bmbt_irec	rec;
	int			is_empty;
	int			error;

	bma->aeof = false;
	error = xfs_bmap_last_extent(NULL, bma->ip, whichfork, &rec,
				     &is_empty);
	if (error)
		return error;

	if (is_empty) {
		bma->aeof = true;
		return 0;
	}

	/*
	 * Check if we are allocation or past the last extent, or at least into
	 * the last delayed allocated extent.
	 */
	bma->aeof = bma->offset >= rec.br_startoff + rec.br_blockcount ||
		(bma->offset >= rec.br_startoff &&
		 isnullstartblock(rec.br_startblock));
	return 0;
}

/*
 * Returns the file-relative block number of the first block past eof in
 * the file.  This is not based on i_size, it is based on the extent records.
 * Returns 0 for local files, as they do not have extent records.
 */
int
xfs_bmap_last_offset(
	struct xfs_inode	*ip,
	xfs_fileoff_t		*last_block,
	int			whichfork)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec	rec;
	int			is_empty;
	int			error;

	*last_block = 0;

	if (ifp->if_format == XFS_DINODE_FMT_LOCAL)
		return 0;

	if (XFS_IS_CORRUPT(ip->i_mount, !xfs_ifork_has_extents(ifp))) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	error = xfs_bmap_last_extent(NULL, ip, whichfork, &rec, &is_empty);
	if (error || is_empty)
		return error;

	*last_block = rec.br_startoff + rec.br_blockcount;
	return 0;
}

/*
 * Extent tree manipulation functions used during allocation.
 */

static inline bool
xfs_bmap_same_rtgroup(
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*left,
	struct xfs_bmbt_irec	*right)
{
	struct xfs_mount	*mp = ip->i_mount;

	if (xfs_ifork_is_realtime(ip, whichfork) && xfs_has_rtgroups(mp)) {
		if (xfs_rtb_to_rgno(mp, left->br_startblock) !=
		    xfs_rtb_to_rgno(mp, right->br_startblock))
			return false;
	}

	return true;
}

/*
 * Convert a delayed allocation to a real allocation.
 */
STATIC int				/* error */
xfs_bmap_add_extent_delay_real(
	struct xfs_bmalloca	*bma,
	int			whichfork)
{
	struct xfs_mount	*mp = bma->ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(bma->ip, whichfork);
	struct xfs_bmbt_irec	*new = &bma->got;
	int			error;	/* error return value */
	int			i;	/* temp state */
	xfs_fileoff_t		new_endoff;	/* end offset of new entry */
	xfs_bmbt_irec_t		r[3];	/* neighbor extent entries */
					/* left is 0, right is 1, prev is 2 */
	int			rval=0;	/* return value (logging flags) */
	uint32_t		state = xfs_bmap_fork_to_state(whichfork);
	xfs_filblks_t		da_new; /* new count del alloc blocks used */
	xfs_filblks_t		da_old; /* old count del alloc blocks used */
	xfs_filblks_t		temp=0;	/* value for da_new calculations */
	int			tmp_rval;	/* partial logging flags */
	struct xfs_bmbt_irec	old;

	ASSERT(whichfork != XFS_ATTR_FORK);
	ASSERT(!isnullstartblock(new->br_startblock));
	ASSERT(!bma->cur || (bma->cur->bc_flags & XFS_BTREE_BMBT_WASDEL));

	XFS_STATS_INC(mp, xs_add_exlist);

#define	LEFT		r[0]
#define	RIGHT		r[1]
#define	PREV		r[2]

	/*
	 * Set up a bunch of variables to make the tests simpler.
	 */
	xfs_iext_get_extent(ifp, &bma->icur, &PREV);
	new_endoff = new->br_startoff + new->br_blockcount;
	ASSERT(isnullstartblock(PREV.br_startblock));
	ASSERT(PREV.br_startoff <= new->br_startoff);
	ASSERT(PREV.br_startoff + PREV.br_blockcount >= new_endoff);

	da_old = startblockval(PREV.br_startblock);
	da_new = 0;

	/*
	 * Set flags determining what part of the previous delayed allocation
	 * extent is being replaced by a real allocation.
	 */
	if (PREV.br_startoff == new->br_startoff)
		state |= BMAP_LEFT_FILLING;
	if (PREV.br_startoff + PREV.br_blockcount == new_endoff)
		state |= BMAP_RIGHT_FILLING;

	/*
	 * Check and set flags if this segment has a left neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 */
	if (xfs_iext_peek_prev_extent(ifp, &bma->icur, &LEFT)) {
		state |= BMAP_LEFT_VALID;
		if (isnullstartblock(LEFT.br_startblock))
			state |= BMAP_LEFT_DELAY;
	}

	if ((state & BMAP_LEFT_VALID) && !(state & BMAP_LEFT_DELAY) &&
	    LEFT.br_startoff + LEFT.br_blockcount == new->br_startoff &&
	    LEFT.br_startblock + LEFT.br_blockcount == new->br_startblock &&
	    LEFT.br_state == new->br_state &&
	    LEFT.br_blockcount + new->br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    xfs_bmap_same_rtgroup(bma->ip, whichfork, &LEFT, new))
		state |= BMAP_LEFT_CONTIG;

	/*
	 * Check and set flags if this segment has a right neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 * Also check for all-three-contiguous being too large.
	 */
	if (xfs_iext_peek_next_extent(ifp, &bma->icur, &RIGHT)) {
		state |= BMAP_RIGHT_VALID;
		if (isnullstartblock(RIGHT.br_startblock))
			state |= BMAP_RIGHT_DELAY;
	}

	if ((state & BMAP_RIGHT_VALID) && !(state & BMAP_RIGHT_DELAY) &&
	    new_endoff == RIGHT.br_startoff &&
	    new->br_startblock + new->br_blockcount == RIGHT.br_startblock &&
	    new->br_state == RIGHT.br_state &&
	    new->br_blockcount + RIGHT.br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    ((state & (BMAP_LEFT_CONTIG | BMAP_LEFT_FILLING |
		       BMAP_RIGHT_FILLING)) !=
		      (BMAP_LEFT_CONTIG | BMAP_LEFT_FILLING |
		       BMAP_RIGHT_FILLING) ||
	     LEFT.br_blockcount + new->br_blockcount + RIGHT.br_blockcount
			<= XFS_MAX_BMBT_EXTLEN) &&
	    xfs_bmap_same_rtgroup(bma->ip, whichfork, new, &RIGHT))
		state |= BMAP_RIGHT_CONTIG;

	error = 0;
	/*
	 * Switch out based on the FILLING and CONTIG state bits.
	 */
	switch (state & (BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG |
			 BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG)) {
	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG |
	     BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The left and right neighbors are both contiguous with new.
		 */
		LEFT.br_blockcount += PREV.br_blockcount + RIGHT.br_blockcount;

		xfs_iext_remove(bma->ip, &bma->icur, state);
		xfs_iext_remove(bma->ip, &bma->icur, state);
		xfs_iext_prev(ifp, &bma->icur);
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &LEFT);
		ifp->if_nextents--;

		if (bma->cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(bma->cur, &RIGHT, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_delete(bma->cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_decrement(bma->cur, 0, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(bma->cur, &LEFT);
			if (error)
				goto done;
		}
		ASSERT(da_new <= da_old);
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG:
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The left neighbor is contiguous, the right is not.
		 */
		old = LEFT;
		LEFT.br_blockcount += PREV.br_blockcount;

		xfs_iext_remove(bma->ip, &bma->icur, state);
		xfs_iext_prev(ifp, &bma->icur);
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &LEFT);

		if (bma->cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(bma->cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(bma->cur, &LEFT);
			if (error)
				goto done;
		}
		ASSERT(da_new <= da_old);
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * The right neighbor is contiguous, the left is not. Take care
		 * with delay -> unwritten extent allocation here because the
		 * delalloc record we are overwriting is always written.
		 */
		PREV.br_startblock = new->br_startblock;
		PREV.br_blockcount += RIGHT.br_blockcount;
		PREV.br_state = new->br_state;

		xfs_iext_next(ifp, &bma->icur);
		xfs_iext_remove(bma->ip, &bma->icur, state);
		xfs_iext_prev(ifp, &bma->icur);
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &PREV);

		if (bma->cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(bma->cur, &RIGHT, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(bma->cur, &PREV);
			if (error)
				goto done;
		}
		ASSERT(da_new <= da_old);
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING:
		/*
		 * Filling in all of a previously delayed allocation extent.
		 * Neither the left nor right neighbors are contiguous with
		 * the new one.
		 */
		PREV.br_startblock = new->br_startblock;
		PREV.br_state = new->br_state;
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &PREV);
		ifp->if_nextents++;

		if (bma->cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(bma->cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_insert(bma->cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}
		ASSERT(da_new <= da_old);
		break;

	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG:
		/*
		 * Filling in the first part of a previous delayed allocation.
		 * The left neighbor is contiguous.
		 */
		old = LEFT;
		temp = PREV.br_blockcount - new->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(bma->ip, temp),
				startblockval(PREV.br_startblock));

		LEFT.br_blockcount += new->br_blockcount;

		PREV.br_blockcount = temp;
		PREV.br_startoff += new->br_blockcount;
		PREV.br_startblock = nullstartblock(da_new);

		xfs_iext_update_extent(bma->ip, state, &bma->icur, &PREV);
		xfs_iext_prev(ifp, &bma->icur);
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &LEFT);

		if (bma->cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(bma->cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(bma->cur, &LEFT);
			if (error)
				goto done;
		}
		ASSERT(da_new <= da_old);
		break;

	case BMAP_LEFT_FILLING:
		/*
		 * Filling in the first part of a previous delayed allocation.
		 * The left neighbor is not contiguous.
		 */
		xfs_iext_update_extent(bma->ip, state, &bma->icur, new);
		ifp->if_nextents++;

		if (bma->cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(bma->cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_insert(bma->cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}

		if (xfs_bmap_needs_btree(bma->ip, whichfork)) {
			error = xfs_bmap_extents_to_btree(bma->tp, bma->ip,
					&bma->cur, 1, &tmp_rval, whichfork);
			rval |= tmp_rval;
			if (error)
				goto done;
		}

		temp = PREV.br_blockcount - new->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(bma->ip, temp),
			startblockval(PREV.br_startblock) -
			(bma->cur ? bma->cur->bc_bmap.allocated : 0));

		PREV.br_startoff = new_endoff;
		PREV.br_blockcount = temp;
		PREV.br_startblock = nullstartblock(da_new);
		xfs_iext_next(ifp, &bma->icur);
		xfs_iext_insert(bma->ip, &bma->icur, &PREV, state);
		xfs_iext_prev(ifp, &bma->icur);
		break;

	case BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Filling in the last part of a previous delayed allocation.
		 * The right neighbor is contiguous with the new allocation.
		 */
		old = RIGHT;
		RIGHT.br_startoff = new->br_startoff;
		RIGHT.br_startblock = new->br_startblock;
		RIGHT.br_blockcount += new->br_blockcount;

		if (bma->cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(bma->cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(bma->cur, &RIGHT);
			if (error)
				goto done;
		}

		temp = PREV.br_blockcount - new->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(bma->ip, temp),
			startblockval(PREV.br_startblock));

		PREV.br_blockcount = temp;
		PREV.br_startblock = nullstartblock(da_new);

		xfs_iext_update_extent(bma->ip, state, &bma->icur, &PREV);
		xfs_iext_next(ifp, &bma->icur);
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &RIGHT);
		ASSERT(da_new <= da_old);
		break;

	case BMAP_RIGHT_FILLING:
		/*
		 * Filling in the last part of a previous delayed allocation.
		 * The right neighbor is not contiguous.
		 */
		xfs_iext_update_extent(bma->ip, state, &bma->icur, new);
		ifp->if_nextents++;

		if (bma->cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(bma->cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_insert(bma->cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}

		if (xfs_bmap_needs_btree(bma->ip, whichfork)) {
			error = xfs_bmap_extents_to_btree(bma->tp, bma->ip,
				&bma->cur, 1, &tmp_rval, whichfork);
			rval |= tmp_rval;
			if (error)
				goto done;
		}

		temp = PREV.br_blockcount - new->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(bma->ip, temp),
			startblockval(PREV.br_startblock) -
			(bma->cur ? bma->cur->bc_bmap.allocated : 0));

		PREV.br_startblock = nullstartblock(da_new);
		PREV.br_blockcount = temp;
		xfs_iext_insert(bma->ip, &bma->icur, &PREV, state);
		xfs_iext_next(ifp, &bma->icur);
		ASSERT(da_new <= da_old);
		break;

	case 0:
		/*
		 * Filling in the middle part of a previous delayed allocation.
		 * Contiguity is impossible here.
		 * This case is avoided almost all the time.
		 *
		 * We start with a delayed allocation:
		 *
		 * +ddddddddddddddddddddddddddddddddddddddddddddddddddddddd+
		 *  PREV @ idx
		 *
	         * and we are allocating:
		 *                     +rrrrrrrrrrrrrrrrr+
		 *			      new
		 *
		 * and we set it up for insertion as:
		 * +ddddddddddddddddddd+rrrrrrrrrrrrrrrrr+ddddddddddddddddd+
		 *                            new
		 *  PREV @ idx          LEFT              RIGHT
		 *                      inserted at idx + 1
		 */
		old = PREV;

		/* LEFT is the new middle */
		LEFT = *new;

		/* RIGHT is the new right */
		RIGHT.br_state = PREV.br_state;
		RIGHT.br_startoff = new_endoff;
		RIGHT.br_blockcount =
			PREV.br_startoff + PREV.br_blockcount - new_endoff;
		RIGHT.br_startblock =
			nullstartblock(xfs_bmap_worst_indlen(bma->ip,
					RIGHT.br_blockcount));

		/* truncate PREV */
		PREV.br_blockcount = new->br_startoff - PREV.br_startoff;
		PREV.br_startblock =
			nullstartblock(xfs_bmap_worst_indlen(bma->ip,
					PREV.br_blockcount));
		xfs_iext_update_extent(bma->ip, state, &bma->icur, &PREV);

		xfs_iext_next(ifp, &bma->icur);
		xfs_iext_insert(bma->ip, &bma->icur, &RIGHT, state);
		xfs_iext_insert(bma->ip, &bma->icur, &LEFT, state);
		ifp->if_nextents++;

		if (bma->cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(bma->cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_insert(bma->cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(bma->cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}

		if (xfs_bmap_needs_btree(bma->ip, whichfork)) {
			error = xfs_bmap_extents_to_btree(bma->tp, bma->ip,
					&bma->cur, 1, &tmp_rval, whichfork);
			rval |= tmp_rval;
			if (error)
				goto done;
		}

		da_new = startblockval(PREV.br_startblock) +
			 startblockval(RIGHT.br_startblock);
		break;

	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_LEFT_FILLING | BMAP_RIGHT_CONTIG:
	case BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG:
	case BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_LEFT_CONTIG:
	case BMAP_RIGHT_CONTIG:
		/*
		 * These cases are all impossible.
		 */
		ASSERT(0);
	}

	/* add reverse mapping unless caller opted out */
	if (!(bma->flags & XFS_BMAPI_NORMAP))
		xfs_rmap_map_extent(bma->tp, bma->ip, whichfork, new);

	/* convert to a btree if necessary */
	if (xfs_bmap_needs_btree(bma->ip, whichfork)) {
		int	tmp_logflags;	/* partial log flag return val */

		ASSERT(bma->cur == NULL);
		error = xfs_bmap_extents_to_btree(bma->tp, bma->ip,
				&bma->cur, da_old > 0, &tmp_logflags,
				whichfork);
		bma->logflags |= tmp_logflags;
		if (error)
			goto done;
	}

	if (da_new != da_old)
		xfs_mod_delalloc(bma->ip, 0, (int64_t)da_new - da_old);

	if (bma->cur) {
		da_new += bma->cur->bc_bmap.allocated;
		bma->cur->bc_bmap.allocated = 0;
	}

	/* adjust for changes in reserved delayed indirect blocks */
	if (da_new < da_old)
		xfs_add_fdblocks(mp, da_old - da_new);
	else if (da_new > da_old)
		error = xfs_dec_fdblocks(mp, da_new - da_old, true);

	xfs_bmap_check_leaf_extents(bma->cur, bma->ip, whichfork);
done:
	if (whichfork != XFS_COW_FORK)
		bma->logflags |= rval;
	return error;
#undef	LEFT
#undef	RIGHT
#undef	PREV
}

/*
 * Convert an unwritten allocation to a real allocation or vice versa.
 */
int					/* error */
xfs_bmap_add_extent_unwritten_real(
	struct xfs_trans	*tp,
	xfs_inode_t		*ip,	/* incore inode pointer */
	int			whichfork,
	struct xfs_iext_cursor	*icur,
	struct xfs_btree_cur	**curp,	/* if *curp is null, not a btree */
	xfs_bmbt_irec_t		*new,	/* new data to add to file extents */
	int			*logflagsp) /* inode logging flags */
{
	struct xfs_btree_cur	*cur;	/* btree cursor */
	int			error;	/* error return value */
	int			i;	/* temp state */
	struct xfs_ifork	*ifp;	/* inode fork pointer */
	xfs_fileoff_t		new_endoff;	/* end offset of new entry */
	xfs_bmbt_irec_t		r[3];	/* neighbor extent entries */
					/* left is 0, right is 1, prev is 2 */
	int			rval=0;	/* return value (logging flags) */
	uint32_t		state = xfs_bmap_fork_to_state(whichfork);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_bmbt_irec	old;

	*logflagsp = 0;

	cur = *curp;
	ifp = xfs_ifork_ptr(ip, whichfork);

	ASSERT(!isnullstartblock(new->br_startblock));

	XFS_STATS_INC(mp, xs_add_exlist);

#define	LEFT		r[0]
#define	RIGHT		r[1]
#define	PREV		r[2]

	/*
	 * Set up a bunch of variables to make the tests simpler.
	 */
	error = 0;
	xfs_iext_get_extent(ifp, icur, &PREV);
	ASSERT(new->br_state != PREV.br_state);
	new_endoff = new->br_startoff + new->br_blockcount;
	ASSERT(PREV.br_startoff <= new->br_startoff);
	ASSERT(PREV.br_startoff + PREV.br_blockcount >= new_endoff);

	/*
	 * Set flags determining what part of the previous oldext allocation
	 * extent is being replaced by a newext allocation.
	 */
	if (PREV.br_startoff == new->br_startoff)
		state |= BMAP_LEFT_FILLING;
	if (PREV.br_startoff + PREV.br_blockcount == new_endoff)
		state |= BMAP_RIGHT_FILLING;

	/*
	 * Check and set flags if this segment has a left neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 */
	if (xfs_iext_peek_prev_extent(ifp, icur, &LEFT)) {
		state |= BMAP_LEFT_VALID;
		if (isnullstartblock(LEFT.br_startblock))
			state |= BMAP_LEFT_DELAY;
	}

	if ((state & BMAP_LEFT_VALID) && !(state & BMAP_LEFT_DELAY) &&
	    LEFT.br_startoff + LEFT.br_blockcount == new->br_startoff &&
	    LEFT.br_startblock + LEFT.br_blockcount == new->br_startblock &&
	    LEFT.br_state == new->br_state &&
	    LEFT.br_blockcount + new->br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    xfs_bmap_same_rtgroup(ip, whichfork, &LEFT, new))
		state |= BMAP_LEFT_CONTIG;

	/*
	 * Check and set flags if this segment has a right neighbor.
	 * Don't set contiguous if the combined extent would be too large.
	 * Also check for all-three-contiguous being too large.
	 */
	if (xfs_iext_peek_next_extent(ifp, icur, &RIGHT)) {
		state |= BMAP_RIGHT_VALID;
		if (isnullstartblock(RIGHT.br_startblock))
			state |= BMAP_RIGHT_DELAY;
	}

	if ((state & BMAP_RIGHT_VALID) && !(state & BMAP_RIGHT_DELAY) &&
	    new_endoff == RIGHT.br_startoff &&
	    new->br_startblock + new->br_blockcount == RIGHT.br_startblock &&
	    new->br_state == RIGHT.br_state &&
	    new->br_blockcount + RIGHT.br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    ((state & (BMAP_LEFT_CONTIG | BMAP_LEFT_FILLING |
		       BMAP_RIGHT_FILLING)) !=
		      (BMAP_LEFT_CONTIG | BMAP_LEFT_FILLING |
		       BMAP_RIGHT_FILLING) ||
	     LEFT.br_blockcount + new->br_blockcount + RIGHT.br_blockcount
			<= XFS_MAX_BMBT_EXTLEN) &&
	    xfs_bmap_same_rtgroup(ip, whichfork, new, &RIGHT))
		state |= BMAP_RIGHT_CONTIG;

	/*
	 * Switch out based on the FILLING and CONTIG state bits.
	 */
	switch (state & (BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG |
			 BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG)) {
	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG |
	     BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The left and right neighbors are both contiguous with new.
		 */
		LEFT.br_blockcount += PREV.br_blockcount + RIGHT.br_blockcount;

		xfs_iext_remove(ip, icur, state);
		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &LEFT);
		ifp->if_nextents -= 2;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &RIGHT, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_delete(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_decrement(cur, 0, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_delete(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_decrement(cur, 0, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &LEFT);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG:
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The left neighbor is contiguous, the right is not.
		 */
		LEFT.br_blockcount += PREV.br_blockcount;

		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &LEFT);
		ifp->if_nextents--;
		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &PREV, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_delete(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_decrement(cur, 0, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &LEFT);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Setting all of a previous oldext extent to newext.
		 * The right neighbor is contiguous, the left is not.
		 */
		PREV.br_blockcount += RIGHT.br_blockcount;
		PREV.br_state = new->br_state;

		xfs_iext_next(ifp, icur);
		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &PREV);
		ifp->if_nextents--;

		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &RIGHT, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_delete(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_decrement(cur, 0, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING:
		/*
		 * Setting all of a previous oldext extent to newext.
		 * Neither the left nor right neighbors are contiguous with
		 * the new one.
		 */
		PREV.br_state = new->br_state;
		xfs_iext_update_extent(ip, state, icur, &PREV);

		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG:
		/*
		 * Setting the first part of a previous oldext extent to newext.
		 * The left neighbor is contiguous.
		 */
		LEFT.br_blockcount += new->br_blockcount;

		old = PREV;
		PREV.br_startoff += new->br_blockcount;
		PREV.br_startblock += new->br_blockcount;
		PREV.br_blockcount -= new->br_blockcount;

		xfs_iext_update_extent(ip, state, icur, &PREV);
		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &LEFT);

		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
			error = xfs_btree_decrement(cur, 0, &i);
			if (error)
				goto done;
			error = xfs_bmbt_update(cur, &LEFT);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_FILLING:
		/*
		 * Setting the first part of a previous oldext extent to newext.
		 * The left neighbor is not contiguous.
		 */
		old = PREV;
		PREV.br_startoff += new->br_blockcount;
		PREV.br_startblock += new->br_blockcount;
		PREV.br_blockcount -= new->br_blockcount;

		xfs_iext_update_extent(ip, state, icur, &PREV);
		xfs_iext_insert(ip, icur, new, state);
		ifp->if_nextents++;

		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
			cur->bc_rec.b = *new;
			if ((error = xfs_btree_insert(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}
		break;

	case BMAP_RIGHT_FILLING | BMAP_RIGHT_CONTIG:
		/*
		 * Setting the last part of a previous oldext extent to newext.
		 * The right neighbor is contiguous with the new allocation.
		 */
		old = PREV;
		PREV.br_blockcount -= new->br_blockcount;

		RIGHT.br_startoff = new->br_startoff;
		RIGHT.br_startblock = new->br_startblock;
		RIGHT.br_blockcount += new->br_blockcount;

		xfs_iext_update_extent(ip, state, icur, &PREV);
		xfs_iext_next(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &RIGHT);

		if (cur == NULL)
			rval = XFS_ILOG_DEXT;
		else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
			error = xfs_btree_increment(cur, 0, &i);
			if (error)
				goto done;
			error = xfs_bmbt_update(cur, &RIGHT);
			if (error)
				goto done;
		}
		break;

	case BMAP_RIGHT_FILLING:
		/*
		 * Setting the last part of a previous oldext extent to newext.
		 * The right neighbor is not contiguous.
		 */
		old = PREV;
		PREV.br_blockcount -= new->br_blockcount;

		xfs_iext_update_extent(ip, state, icur, &PREV);
		xfs_iext_next(ifp, icur);
		xfs_iext_insert(ip, icur, new, state);
		ifp->if_nextents++;

		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &PREV);
			if (error)
				goto done;
			error = xfs_bmbt_lookup_eq(cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			if ((error = xfs_btree_insert(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}
		break;

	case 0:
		/*
		 * Setting the middle part of a previous oldext extent to
		 * newext.  Contiguity is impossible here.
		 * One extent becomes three extents.
		 */
		old = PREV;
		PREV.br_blockcount = new->br_startoff - PREV.br_startoff;

		r[0] = *new;
		r[1].br_startoff = new_endoff;
		r[1].br_blockcount =
			old.br_startoff + old.br_blockcount - new_endoff;
		r[1].br_startblock = new->br_startblock + new->br_blockcount;
		r[1].br_state = PREV.br_state;

		xfs_iext_update_extent(ip, state, icur, &PREV);
		xfs_iext_next(ifp, icur);
		xfs_iext_insert(ip, icur, &r[1], state);
		xfs_iext_insert(ip, icur, &r[0], state);
		ifp->if_nextents += 2;

		if (cur == NULL)
			rval = XFS_ILOG_CORE | XFS_ILOG_DEXT;
		else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			/* new right extent - oldext */
			error = xfs_bmbt_update(cur, &r[1]);
			if (error)
				goto done;
			/* new left extent - oldext */
			cur->bc_rec.b = PREV;
			if ((error = xfs_btree_insert(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			/*
			 * Reset the cursor to the position of the new extent
			 * we are about to insert as we can't trust it after
			 * the previous insert.
			 */
			error = xfs_bmbt_lookup_eq(cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			/* new middle extent - newext */
			if ((error = xfs_btree_insert(cur, &i)))
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}
		break;

	case BMAP_LEFT_FILLING | BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_LEFT_FILLING | BMAP_RIGHT_CONTIG:
	case BMAP_RIGHT_FILLING | BMAP_LEFT_CONTIG:
	case BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
	case BMAP_LEFT_CONTIG:
	case BMAP_RIGHT_CONTIG:
		/*
		 * These cases are all impossible.
		 */
		ASSERT(0);
	}

	/* update reverse mappings */
	xfs_rmap_convert_extent(mp, tp, ip, whichfork, new);

	/* convert to a btree if necessary */
	if (xfs_bmap_needs_btree(ip, whichfork)) {
		int	tmp_logflags;	/* partial log flag return val */

		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(tp, ip, &cur, 0,
				&tmp_logflags, whichfork);
		*logflagsp |= tmp_logflags;
		if (error)
			goto done;
	}

	/* clear out the allocated field, done with it now in any case. */
	if (cur) {
		cur->bc_bmap.allocated = 0;
		*curp = cur;
	}

	xfs_bmap_check_leaf_extents(*curp, ip, whichfork);
done:
	*logflagsp |= rval;
	return error;
#undef	LEFT
#undef	RIGHT
#undef	PREV
}

/*
 * Convert a hole to a real allocation.
 */
STATIC int				/* error */
xfs_bmap_add_extent_hole_real(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_iext_cursor	*icur,
	struct xfs_btree_cur	**curp,
	struct xfs_bmbt_irec	*new,
	int			*logflagsp,
	uint32_t		flags)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_btree_cur	*cur = *curp;
	int			error;	/* error return value */
	int			i;	/* temp state */
	xfs_bmbt_irec_t		left;	/* left neighbor extent entry */
	xfs_bmbt_irec_t		right;	/* right neighbor extent entry */
	int			rval=0;	/* return value (logging flags) */
	uint32_t		state = xfs_bmap_fork_to_state(whichfork);
	struct xfs_bmbt_irec	old;

	ASSERT(!isnullstartblock(new->br_startblock));
	ASSERT(!cur || !(cur->bc_flags & XFS_BTREE_BMBT_WASDEL));

	XFS_STATS_INC(mp, xs_add_exlist);

	/*
	 * Check and set flags if this segment has a left neighbor.
	 */
	if (xfs_iext_peek_prev_extent(ifp, icur, &left)) {
		state |= BMAP_LEFT_VALID;
		if (isnullstartblock(left.br_startblock))
			state |= BMAP_LEFT_DELAY;
	}

	/*
	 * Check and set flags if this segment has a current value.
	 * Not true if we're inserting into the "hole" at eof.
	 */
	if (xfs_iext_get_extent(ifp, icur, &right)) {
		state |= BMAP_RIGHT_VALID;
		if (isnullstartblock(right.br_startblock))
			state |= BMAP_RIGHT_DELAY;
	}

	/*
	 * We're inserting a real allocation between "left" and "right".
	 * Set the contiguity flags.  Don't let extents get too large.
	 */
	if ((state & BMAP_LEFT_VALID) && !(state & BMAP_LEFT_DELAY) &&
	    left.br_startoff + left.br_blockcount == new->br_startoff &&
	    left.br_startblock + left.br_blockcount == new->br_startblock &&
	    left.br_state == new->br_state &&
	    left.br_blockcount + new->br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    xfs_bmap_same_rtgroup(ip, whichfork, &left, new))
		state |= BMAP_LEFT_CONTIG;

	if ((state & BMAP_RIGHT_VALID) && !(state & BMAP_RIGHT_DELAY) &&
	    new->br_startoff + new->br_blockcount == right.br_startoff &&
	    new->br_startblock + new->br_blockcount == right.br_startblock &&
	    new->br_state == right.br_state &&
	    new->br_blockcount + right.br_blockcount <= XFS_MAX_BMBT_EXTLEN &&
	    (!(state & BMAP_LEFT_CONTIG) ||
	     left.br_blockcount + new->br_blockcount +
	     right.br_blockcount <= XFS_MAX_BMBT_EXTLEN) &&
	    xfs_bmap_same_rtgroup(ip, whichfork, new, &right))
		state |= BMAP_RIGHT_CONTIG;

	error = 0;
	/*
	 * Select which case we're in here, and implement it.
	 */
	switch (state & (BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG)) {
	case BMAP_LEFT_CONTIG | BMAP_RIGHT_CONTIG:
		/*
		 * New allocation is contiguous with real allocations on the
		 * left and on the right.
		 * Merge all three into a single extent record.
		 */
		left.br_blockcount += new->br_blockcount + right.br_blockcount;

		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &left);
		ifp->if_nextents--;

		if (cur == NULL) {
			rval = XFS_ILOG_CORE | xfs_ilog_fext(whichfork);
		} else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, &right, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_delete(cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_decrement(cur, 0, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &left);
			if (error)
				goto done;
		}
		break;

	case BMAP_LEFT_CONTIG:
		/*
		 * New allocation is contiguous with a real allocation
		 * on the left.
		 * Merge the new allocation with the left neighbor.
		 */
		old = left;
		left.br_blockcount += new->br_blockcount;

		xfs_iext_prev(ifp, icur);
		xfs_iext_update_extent(ip, state, icur, &left);

		if (cur == NULL) {
			rval = xfs_ilog_fext(whichfork);
		} else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &left);
			if (error)
				goto done;
		}
		break;

	case BMAP_RIGHT_CONTIG:
		/*
		 * New allocation is contiguous with a real allocation
		 * on the right.
		 * Merge the new allocation with the right neighbor.
		 */
		old = right;

		right.br_startoff = new->br_startoff;
		right.br_startblock = new->br_startblock;
		right.br_blockcount += new->br_blockcount;
		xfs_iext_update_extent(ip, state, icur, &right);

		if (cur == NULL) {
			rval = xfs_ilog_fext(whichfork);
		} else {
			rval = 0;
			error = xfs_bmbt_lookup_eq(cur, &old, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_bmbt_update(cur, &right);
			if (error)
				goto done;
		}
		break;

	case 0:
		/*
		 * New allocation is not contiguous with another
		 * real allocation.
		 * Insert a new entry.
		 */
		xfs_iext_insert(ip, icur, new, state);
		ifp->if_nextents++;

		if (cur == NULL) {
			rval = XFS_ILOG_CORE | xfs_ilog_fext(whichfork);
		} else {
			rval = XFS_ILOG_CORE;
			error = xfs_bmbt_lookup_eq(cur, new, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 0)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
			error = xfs_btree_insert(cur, &i);
			if (error)
				goto done;
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				error = -EFSCORRUPTED;
				goto done;
			}
		}
		break;
	}

	/* add reverse mapping unless caller opted out */
	if (!(flags & XFS_BMAPI_NORMAP))
		xfs_rmap_map_extent(tp, ip, whichfork, new);

	/* convert to a btree if necessary */
	if (xfs_bmap_needs_btree(ip, whichfork)) {
		int	tmp_logflags;	/* partial log flag return val */

		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(tp, ip, curp, 0,
				&tmp_logflags, whichfork);
		*logflagsp |= tmp_logflags;
		cur = *curp;
		if (error)
			goto done;
	}

	/* clear out the allocated field, done with it now in any case. */
	if (cur)
		cur->bc_bmap.allocated = 0;

	xfs_bmap_check_leaf_extents(cur, ip, whichfork);
done:
	*logflagsp |= rval;
	return error;
}

/*
 * Functions used in the extent read, allocate and remove paths
 */

/*
 * Adjust the size of the new extent based on i_extsize and rt extsize.
 */
int
xfs_bmap_extsize_align(
	xfs_mount_t	*mp,
	xfs_bmbt_irec_t	*gotp,		/* next extent pointer */
	xfs_bmbt_irec_t	*prevp,		/* previous extent pointer */
	xfs_extlen_t	extsz,		/* align to this extent size */
	int		rt,		/* is this a realtime inode? */
	int		eof,		/* is extent at end-of-file? */
	int		delay,		/* creating delalloc extent? */
	int		convert,	/* overwriting unwritten extent? */
	xfs_fileoff_t	*offp,		/* in/out: aligned offset */
	xfs_extlen_t	*lenp)		/* in/out: aligned length */
{
	xfs_fileoff_t	orig_off;	/* original offset */
	xfs_extlen_t	orig_alen;	/* original length */
	xfs_fileoff_t	orig_end;	/* original off+len */
	xfs_fileoff_t	nexto;		/* next file offset */
	xfs_fileoff_t	prevo;		/* previous file offset */
	xfs_fileoff_t	align_off;	/* temp for offset */
	xfs_extlen_t	align_alen;	/* temp for length */
	xfs_extlen_t	temp;		/* temp for calculations */

	if (convert)
		return 0;

	orig_off = align_off = *offp;
	orig_alen = align_alen = *lenp;
	orig_end = orig_off + orig_alen;

	/*
	 * If this request overlaps an existing extent, then don't
	 * attempt to perform any additional alignment.
	 */
	if (!delay && !eof &&
	    (orig_off >= gotp->br_startoff) &&
	    (orig_end <= gotp->br_startoff + gotp->br_blockcount)) {
		return 0;
	}

	/*
	 * If the file offset is unaligned vs. the extent size
	 * we need to align it.  This will be possible unless
	 * the file was previously written with a kernel that didn't
	 * perform this alignment, or if a truncate shot us in the
	 * foot.
	 */
	div_u64_rem(orig_off, extsz, &temp);
	if (temp) {
		align_alen += temp;
		align_off -= temp;
	}

	/* Same adjustment for the end of the requested area. */
	temp = (align_alen % extsz);
	if (temp)
		align_alen += extsz - temp;

	/*
	 * For large extent hint sizes, the aligned extent might be larger than
	 * XFS_BMBT_MAX_EXTLEN. In that case, reduce the size by an extsz so
	 * that it pulls the length back under XFS_BMBT_MAX_EXTLEN. The outer
	 * allocation loops handle short allocation just fine, so it is safe to
	 * do this. We only want to do it when we are forced to, though, because
	 * it means more allocation operations are required.
	 */
	while (align_alen > XFS_MAX_BMBT_EXTLEN)
		align_alen -= extsz;
	ASSERT(align_alen <= XFS_MAX_BMBT_EXTLEN);

	/*
	 * If the previous block overlaps with this proposed allocation
	 * then move the start forward without adjusting the length.
	 */
	if (prevp->br_startoff != NULLFILEOFF) {
		if (prevp->br_startblock == HOLESTARTBLOCK)
			prevo = prevp->br_startoff;
		else
			prevo = prevp->br_startoff + prevp->br_blockcount;
	} else
		prevo = 0;
	if (align_off != orig_off && align_off < prevo)
		align_off = prevo;
	/*
	 * If the next block overlaps with this proposed allocation
	 * then move the start back without adjusting the length,
	 * but not before offset 0.
	 * This may of course make the start overlap previous block,
	 * and if we hit the offset 0 limit then the next block
	 * can still overlap too.
	 */
	if (!eof && gotp->br_startoff != NULLFILEOFF) {
		if ((delay && gotp->br_startblock == HOLESTARTBLOCK) ||
		    (!delay && gotp->br_startblock == DELAYSTARTBLOCK))
			nexto = gotp->br_startoff + gotp->br_blockcount;
		else
			nexto = gotp->br_startoff;
	} else
		nexto = NULLFILEOFF;
	if (!eof &&
	    align_off + align_alen != orig_end &&
	    align_off + align_alen > nexto)
		align_off = nexto > align_alen ? nexto - align_alen : 0;
	/*
	 * If we're now overlapping the next or previous extent that
	 * means we can't fit an extsz piece in this hole.  Just move
	 * the start forward to the first valid spot and set
	 * the length so we hit the end.
	 */
	if (align_off != orig_off && align_off < prevo)
		align_off = prevo;
	if (align_off + align_alen != orig_end &&
	    align_off + align_alen > nexto &&
	    nexto != NULLFILEOFF) {
		ASSERT(nexto > prevo);
		align_alen = nexto - align_off;
	}

	/*
	 * If realtime, and the result isn't a multiple of the realtime
	 * extent size we need to remove blocks until it is.
	 */
	if (rt && (temp = xfs_extlen_to_rtxmod(mp, align_alen))) {
		/*
		 * We're not covering the original request, or
		 * we won't be able to once we fix the length.
		 */
		if (orig_off < align_off ||
		    orig_end > align_off + align_alen ||
		    align_alen - temp < orig_alen)
			return -EINVAL;
		/*
		 * Try to fix it by moving the start up.
		 */
		if (align_off + temp <= orig_off) {
			align_alen -= temp;
			align_off += temp;
		}
		/*
		 * Try to fix it by moving the end in.
		 */
		else if (align_off + align_alen - temp >= orig_end)
			align_alen -= temp;
		/*
		 * Set the start to the minimum then trim the length.
		 */
		else {
			align_alen -= orig_off - align_off;
			align_off = orig_off;
			align_alen -= xfs_extlen_to_rtxmod(mp, align_alen);
		}
		/*
		 * Result doesn't cover the request, fail it.
		 */
		if (orig_off < align_off || orig_end > align_off + align_alen)
			return -EINVAL;
	} else {
		ASSERT(orig_off >= align_off);
		/* see XFS_BMBT_MAX_EXTLEN handling above */
		ASSERT(orig_end <= align_off + align_alen ||
		       align_alen + extsz > XFS_MAX_BMBT_EXTLEN);
	}

#ifdef DEBUG
	if (!eof && gotp->br_startoff != NULLFILEOFF)
		ASSERT(align_off + align_alen <= gotp->br_startoff);
	if (prevp->br_startoff != NULLFILEOFF)
		ASSERT(align_off >= prevp->br_startoff + prevp->br_blockcount);
#endif

	*lenp = align_alen;
	*offp = align_off;
	return 0;
}

static inline bool
xfs_bmap_adjacent_valid(
	struct xfs_bmalloca	*ap,
	xfs_fsblock_t		x,
	xfs_fsblock_t		y)
{
	struct xfs_mount	*mp = ap->ip->i_mount;

	if (XFS_IS_REALTIME_INODE(ap->ip) &&
	    (ap->datatype & XFS_ALLOC_USERDATA)) {
		if (!xfs_has_rtgroups(mp))
			return x < mp->m_sb.sb_rblocks;

		return xfs_rtb_to_rgno(mp, x) == xfs_rtb_to_rgno(mp, y) &&
			xfs_rtb_to_rgno(mp, x) < mp->m_sb.sb_rgcount &&
			xfs_rtb_to_rtx(mp, x) < mp->m_sb.sb_rgextents;

	}

	return XFS_FSB_TO_AGNO(mp, x) == XFS_FSB_TO_AGNO(mp, y) &&
		XFS_FSB_TO_AGNO(mp, x) < mp->m_sb.sb_agcount &&
		XFS_FSB_TO_AGBNO(mp, x) < mp->m_sb.sb_agblocks;
}

#define XFS_ALLOC_GAP_UNITS	4

/* returns true if ap->blkno was modified */
bool
xfs_bmap_adjacent(
	struct xfs_bmalloca	*ap)	/* bmap alloc argument struct */
{
	xfs_fsblock_t		adjust;		/* adjustment to block numbers */

	/*
	 * If allocating at eof, and there's a previous real block,
	 * try to use its last block as our starting point.
	 */
	if (ap->eof && ap->prev.br_startoff != NULLFILEOFF &&
	    !isnullstartblock(ap->prev.br_startblock) &&
	    xfs_bmap_adjacent_valid(ap,
			ap->prev.br_startblock + ap->prev.br_blockcount,
			ap->prev.br_startblock)) {
		ap->blkno = ap->prev.br_startblock + ap->prev.br_blockcount;
		/*
		 * Adjust for the gap between prevp and us.
		 */
		adjust = ap->offset -
			(ap->prev.br_startoff + ap->prev.br_blockcount);
		if (adjust && xfs_bmap_adjacent_valid(ap, ap->blkno + adjust,
				ap->prev.br_startblock))
			ap->blkno += adjust;
		return true;
	}
	/*
	 * If not at eof, then compare the two neighbor blocks.
	 * Figure out whether either one gives us a good starting point,
	 * and pick the better one.
	 */
	if (!ap->eof) {
		xfs_fsblock_t	gotbno;		/* right side block number */
		xfs_fsblock_t	gotdiff=0;	/* right side difference */
		xfs_fsblock_t	prevbno;	/* left side block number */
		xfs_fsblock_t	prevdiff=0;	/* left side difference */

		/*
		 * If there's a previous (left) block, select a requested
		 * start block based on it.
		 */
		if (ap->prev.br_startoff != NULLFILEOFF &&
		    !isnullstartblock(ap->prev.br_startblock) &&
		    (prevbno = ap->prev.br_startblock +
			       ap->prev.br_blockcount) &&
		    xfs_bmap_adjacent_valid(ap, prevbno,
				ap->prev.br_startblock)) {
			/*
			 * Calculate gap to end of previous block.
			 */
			adjust = prevdiff = ap->offset -
				(ap->prev.br_startoff +
				 ap->prev.br_blockcount);
			/*
			 * Figure the startblock based on the previous block's
			 * end and the gap size.
			 * Heuristic!
			 * If the gap is large relative to the piece we're
			 * allocating, or using it gives us an invalid block
			 * number, then just use the end of the previous block.
			 */
			if (prevdiff <= XFS_ALLOC_GAP_UNITS * ap->length &&
			    xfs_bmap_adjacent_valid(ap, prevbno + prevdiff,
					ap->prev.br_startblock))
				prevbno += adjust;
			else
				prevdiff += adjust;
		}
		/*
		 * No previous block or can't follow it, just default.
		 */
		else
			prevbno = NULLFSBLOCK;
		/*
		 * If there's a following (right) block, select a requested
		 * start block based on it.
		 */
		if (!isnullstartblock(ap->got.br_startblock)) {
			/*
			 * Calculate gap to start of next block.
			 */
			adjust = gotdiff = ap->got.br_startoff - ap->offset;
			/*
			 * Figure the startblock based on the next block's
			 * start and the gap size.
			 */
			gotbno = ap->got.br_startblock;
			/*
			 * Heuristic!
			 * If the gap is large relative to the piece we're
			 * allocating, or using it gives us an invalid block
			 * number, then just use the start of the next block
			 * offset by our length.
			 */
			if (gotdiff <= XFS_ALLOC_GAP_UNITS * ap->length &&
			    xfs_bmap_adjacent_valid(ap, gotbno - gotdiff,
					gotbno))
				gotbno -= adjust;
			else if (xfs_bmap_adjacent_valid(ap, gotbno - ap->length,
					gotbno)) {
				gotbno -= ap->length;
				gotdiff += adjust - ap->length;
			} else
				gotdiff += adjust;
		}
		/*
		 * No next block, just default.
		 */
		else
			gotbno = NULLFSBLOCK;
		/*
		 * If both valid, pick the better one, else the only good
		 * one, else ap->blkno is already set (to 0 or the inode block).
		 */
		if (prevbno != NULLFSBLOCK && gotbno != NULLFSBLOCK) {
			ap->blkno = prevdiff <= gotdiff ? prevbno : gotbno;
			return true;
		}
		if (prevbno != NULLFSBLOCK) {
			ap->blkno = prevbno;
			return true;
		}
		if (gotbno != NULLFSBLOCK) {
			ap->blkno = gotbno;
			return true;
		}
	}

	return false;
}

int
xfs_bmap_longest_free_extent(
	struct xfs_perag	*pag,
	struct xfs_trans	*tp,
	xfs_extlen_t		*blen)
{
	xfs_extlen_t		longest;
	int			error = 0;

	if (!xfs_perag_initialised_agf(pag)) {
		error = xfs_alloc_read_agf(pag, tp, XFS_ALLOC_FLAG_TRYLOCK,
				NULL);
		if (error)
			return error;
	}

	longest = xfs_alloc_longest_free_extent(pag,
				xfs_alloc_min_freelist(pag_mount(pag), pag),
				xfs_ag_resv_needed(pag, XFS_AG_RESV_NONE));
	if (*blen < longest)
		*blen = longest;

	return 0;
}

static xfs_extlen_t
xfs_bmap_select_minlen(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	xfs_extlen_t		blen)
{

	/*
	 * Since we used XFS_ALLOC_FLAG_TRYLOCK in _longest_free_extent(), it is
	 * possible that there is enough contiguous free space for this request.
	 */
	if (blen < ap->minlen)
		return ap->minlen;

	/*
	 * If the best seen length is less than the request length,
	 * use the best as the minimum, otherwise we've got the maxlen we
	 * were asked for.
	 */
	if (blen < args->maxlen)
		return blen;
	return args->maxlen;
}

static int
xfs_bmap_btalloc_select_lengths(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	xfs_extlen_t		*blen)
{
	struct xfs_mount	*mp = args->mp;
	struct xfs_perag	*pag;
	xfs_agnumber_t		agno, startag;
	int			error = 0;

	if (ap->tp->t_flags & XFS_TRANS_LOWMODE) {
		args->total = ap->minlen;
		args->minlen = ap->minlen;
		return 0;
	}

	args->total = ap->total;
	startag = XFS_FSB_TO_AGNO(mp, ap->blkno);
	if (startag == NULLAGNUMBER)
		startag = 0;

	*blen = 0;
	for_each_perag_wrap(mp, startag, agno, pag) {
		error = xfs_bmap_longest_free_extent(pag, args->tp, blen);
		if (error && error != -EAGAIN)
			break;
		error = 0;
		if (*blen >= args->maxlen)
			break;
	}
	if (pag)
		xfs_perag_rele(pag);

	args->minlen = xfs_bmap_select_minlen(ap, args, *blen);
	return error;
}

/* Update all inode and quota accounting for the allocation we just did. */
void
xfs_bmap_alloc_account(
	struct xfs_bmalloca	*ap)
{
	bool			isrt = XFS_IS_REALTIME_INODE(ap->ip) &&
					!(ap->flags & XFS_BMAPI_ATTRFORK);
	uint			fld;

	if (ap->flags & XFS_BMAPI_COWFORK) {
		/*
		 * COW fork blocks are in-core only and thus are treated as
		 * in-core quota reservation (like delalloc blocks) even when
		 * converted to real blocks. The quota reservation is not
		 * accounted to disk until blocks are remapped to the data
		 * fork. So if these blocks were previously delalloc, we
		 * already have quota reservation and there's nothing to do
		 * yet.
		 */
		if (ap->wasdel) {
			xfs_mod_delalloc(ap->ip, -(int64_t)ap->length, 0);
			return;
		}

		/*
		 * Otherwise, we've allocated blocks in a hole. The transaction
		 * has acquired in-core quota reservation for this extent.
		 * Rather than account these as real blocks, however, we reduce
		 * the transaction quota reservation based on the allocation.
		 * This essentially transfers the transaction quota reservation
		 * to that of a delalloc extent.
		 */
		ap->ip->i_delayed_blks += ap->length;
		xfs_trans_mod_dquot_byino(ap->tp, ap->ip, isrt ?
				XFS_TRANS_DQ_RES_RTBLKS : XFS_TRANS_DQ_RES_BLKS,
				-(long)ap->length);
		return;
	}

	/* data/attr fork only */
	ap->ip->i_nblocks += ap->length;
	xfs_trans_log_inode(ap->tp, ap->ip, XFS_ILOG_CORE);
	if (ap->wasdel) {
		ap->ip->i_delayed_blks -= ap->length;
		xfs_mod_delalloc(ap->ip, -(int64_t)ap->length, 0);
		fld = isrt ? XFS_TRANS_DQ_DELRTBCOUNT : XFS_TRANS_DQ_DELBCOUNT;
	} else {
		fld = isrt ? XFS_TRANS_DQ_RTBCOUNT : XFS_TRANS_DQ_BCOUNT;
	}

	xfs_trans_mod_dquot_byino(ap->tp, ap->ip, fld, ap->length);
}

static int
xfs_bmap_compute_alignments(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args)
{
	struct xfs_mount	*mp = args->mp;
	xfs_extlen_t		align = 0; /* minimum allocation alignment */
	int			stripe_align = 0;

	/* stripe alignment for allocation is determined by mount parameters */
	if (mp->m_swidth && xfs_has_swalloc(mp))
		stripe_align = mp->m_swidth;
	else if (mp->m_dalign)
		stripe_align = mp->m_dalign;

	if (ap->flags & XFS_BMAPI_COWFORK)
		align = xfs_get_cowextsz_hint(ap->ip);
	else if (ap->datatype & XFS_ALLOC_USERDATA)
		align = xfs_get_extsz_hint(ap->ip);
	if (align) {
		if (xfs_bmap_extsize_align(mp, &ap->got, &ap->prev, align, 0,
					ap->eof, 0, ap->conv, &ap->offset,
					&ap->length))
			ASSERT(0);
		ASSERT(ap->length);
	}

	/* apply extent size hints if obtained earlier */
	if (align) {
		args->prod = align;
		div_u64_rem(ap->offset, args->prod, &args->mod);
		if (args->mod)
			args->mod = args->prod - args->mod;
	} else if (mp->m_sb.sb_blocksize >= PAGE_SIZE) {
		args->prod = 1;
		args->mod = 0;
	} else {
		args->prod = PAGE_SIZE >> mp->m_sb.sb_blocklog;
		div_u64_rem(ap->offset, args->prod, &args->mod);
		if (args->mod)
			args->mod = args->prod - args->mod;
	}

	return stripe_align;
}

static void
xfs_bmap_process_allocated_extent(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	xfs_fileoff_t		orig_offset,
	xfs_extlen_t		orig_length)
{
	ap->blkno = args->fsbno;
	ap->length = args->len;
	/*
	 * If the extent size hint is active, we tried to round the
	 * caller's allocation request offset down to extsz and the
	 * length up to another extsz boundary.  If we found a free
	 * extent we mapped it in starting at this new offset.  If the
	 * newly mapped space isn't long enough to cover any of the
	 * range of offsets that was originally requested, move the
	 * mapping up so that we can fill as much of the caller's
	 * original request as possible.  Free space is apparently
	 * very fragmented so we're unlikely to be able to satisfy the
	 * hints anyway.
	 */
	if (ap->length <= orig_length)
		ap->offset = orig_offset;
	else if (ap->offset + ap->length < orig_offset + orig_length)
		ap->offset = orig_offset + orig_length - ap->length;
	xfs_bmap_alloc_account(ap);
}

static int
xfs_bmap_exact_minlen_extent_alloc(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args)
{
	if (ap->minlen != 1) {
		args->fsbno = NULLFSBLOCK;
		return 0;
	}

	args->alloc_minlen_only = 1;
	args->minlen = args->maxlen = ap->minlen;
	args->total = ap->total;

	/*
	 * Unlike the longest extent available in an AG, we don't track
	 * the length of an AG's shortest extent.
	 * XFS_ERRTAG_BMAP_ALLOC_MINLEN_EXTENT is a debug only knob and
	 * hence we can afford to start traversing from the 0th AG since
	 * we need not be concerned about a drop in performance in
	 * "debug only" code paths.
	 */
	ap->blkno = XFS_AGB_TO_FSB(ap->ip->i_mount, 0, 0);

	/*
	 * Call xfs_bmap_btalloc_low_space here as it first does a "normal" AG
	 * iteration and then drops args->total to args->minlen, which might be
	 * required to find an allocation for the transaction reservation when
	 * the file system is very full.
	 */
	return xfs_bmap_btalloc_low_space(ap, args);
}

/*
 * If we are not low on available data blocks and we are allocating at
 * EOF, optimise allocation for contiguous file extension and/or stripe
 * alignment of the new extent.
 *
 * NOTE: ap->aeof is only set if the allocation length is >= the
 * stripe unit and the allocation offset is at the end of file.
 */
static int
xfs_bmap_btalloc_at_eof(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	xfs_extlen_t		blen,
	int			stripe_align,
	bool			ag_only)
{
	struct xfs_mount	*mp = args->mp;
	struct xfs_perag	*caller_pag = args->pag;
	int			error;

	/*
	 * If there are already extents in the file, and xfs_bmap_adjacent() has
	 * given a better blkno, try an exact EOF block allocation to extend the
	 * file as a contiguous extent. If that fails, or it's the first
	 * allocation in a file, just try for a stripe aligned allocation.
	 */
	if (ap->eof) {
		xfs_extlen_t	nextminlen = 0;

		/*
		 * Compute the minlen+alignment for the next case.  Set slop so
		 * that the value of minlen+alignment+slop doesn't go up between
		 * the calls.
		 */
		args->alignment = 1;
		if (blen > stripe_align && blen <= args->maxlen)
			nextminlen = blen - stripe_align;
		else
			nextminlen = args->minlen;
		if (nextminlen + stripe_align > args->minlen + 1)
			args->minalignslop = nextminlen + stripe_align -
					args->minlen - 1;
		else
			args->minalignslop = 0;

		if (!caller_pag)
			args->pag = xfs_perag_get(mp, XFS_FSB_TO_AGNO(mp, ap->blkno));
		error = xfs_alloc_vextent_exact_bno(args, ap->blkno);
		if (!caller_pag) {
			xfs_perag_put(args->pag);
			args->pag = NULL;
		}
		if (error)
			return error;

		if (args->fsbno != NULLFSBLOCK)
			return 0;
		/*
		 * Exact allocation failed. Reset to try an aligned allocation
		 * according to the original allocation specification.
		 */
		args->alignment = stripe_align;
		args->minlen = nextminlen;
		args->minalignslop = 0;
	} else {
		/*
		 * Adjust minlen to try and preserve alignment if we
		 * can't guarantee an aligned maxlen extent.
		 */
		args->alignment = stripe_align;
		if (blen > args->alignment &&
		    blen <= args->maxlen + args->alignment)
			args->minlen = blen - args->alignment;
		args->minalignslop = 0;
	}

	if (ag_only) {
		error = xfs_alloc_vextent_near_bno(args, ap->blkno);
	} else {
		args->pag = NULL;
		error = xfs_alloc_vextent_start_ag(args, ap->blkno);
		ASSERT(args->pag == NULL);
		args->pag = caller_pag;
	}
	if (error)
		return error;

	if (args->fsbno != NULLFSBLOCK)
		return 0;

	/*
	 * Allocation failed, so turn return the allocation args to their
	 * original non-aligned state so the caller can proceed on allocation
	 * failure as if this function was never called.
	 */
	args->alignment = 1;
	return 0;
}

/*
 * We have failed multiple allocation attempts so now are in a low space
 * allocation situation. Try a locality first full filesystem minimum length
 * allocation whilst still maintaining necessary total block reservation
 * requirements.
 *
 * If that fails, we are now critically low on space, so perform a last resort
 * allocation attempt: no reserve, no locality, blocking, minimum length, full
 * filesystem free space scan. We also indicate to future allocations in this
 * transaction that we are critically low on space so they don't waste time on
 * allocation modes that are unlikely to succeed.
 */
int
xfs_bmap_btalloc_low_space(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args)
{
	int			error;

	if (args->minlen > ap->minlen) {
		args->minlen = ap->minlen;
		error = xfs_alloc_vextent_start_ag(args, ap->blkno);
		if (error || args->fsbno != NULLFSBLOCK)
			return error;
	}

	/* Last ditch attempt before failure is declared. */
	args->total = ap->minlen;
	error = xfs_alloc_vextent_first_ag(args, 0);
	if (error)
		return error;
	ap->tp->t_flags |= XFS_TRANS_LOWMODE;
	return 0;
}

static int
xfs_bmap_btalloc_filestreams(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	int			stripe_align)
{
	xfs_extlen_t		blen = 0;
	int			error = 0;


	error = xfs_filestream_select_ag(ap, args, &blen);
	if (error)
		return error;
	ASSERT(args->pag);

	/*
	 * If we are in low space mode, then optimal allocation will fail so
	 * prepare for minimal allocation and jump to the low space algorithm
	 * immediately.
	 */
	if (ap->tp->t_flags & XFS_TRANS_LOWMODE) {
		args->minlen = ap->minlen;
		ASSERT(args->fsbno == NULLFSBLOCK);
		goto out_low_space;
	}

	args->minlen = xfs_bmap_select_minlen(ap, args, blen);
	if (ap->aeof)
		error = xfs_bmap_btalloc_at_eof(ap, args, blen, stripe_align,
				true);

	if (!error && args->fsbno == NULLFSBLOCK)
		error = xfs_alloc_vextent_near_bno(args, ap->blkno);

out_low_space:
	/*
	 * We are now done with the perag reference for the filestreams
	 * association provided by xfs_filestream_select_ag(). Release it now as
	 * we've either succeeded, had a fatal error or we are out of space and
	 * need to do a full filesystem scan for free space which will take it's
	 * own references.
	 */
	xfs_perag_rele(args->pag);
	args->pag = NULL;
	if (error || args->fsbno != NULLFSBLOCK)
		return error;

	return xfs_bmap_btalloc_low_space(ap, args);
}

static int
xfs_bmap_btalloc_best_length(
	struct xfs_bmalloca	*ap,
	struct xfs_alloc_arg	*args,
	int			stripe_align)
{
	xfs_extlen_t		blen = 0;
	int			error;

	ap->blkno = XFS_INO_TO_FSB(args->mp, ap->ip->i_ino);
	if (!xfs_bmap_adjacent(ap))
		ap->eof = false;

	/*
	 * Search for an allocation group with a single extent large enough for
	 * the request.  If one isn't found, then adjust the minimum allocation
	 * size to the largest space found.
	 */
	error = xfs_bmap_btalloc_select_lengths(ap, args, &blen);
	if (error)
		return error;

	/*
	 * Don't attempt optimal EOF allocation if previous allocations barely
	 * succeeded due to being near ENOSPC. It is highly unlikely we'll get
	 * optimal or even aligned allocations in this case, so don't waste time
	 * trying.
	 */
	if (ap->aeof && !(ap->tp->t_flags & XFS_TRANS_LOWMODE)) {
		error = xfs_bmap_btalloc_at_eof(ap, args, blen, stripe_align,
				false);
		if (error || args->fsbno != NULLFSBLOCK)
			return error;
	}

	error = xfs_alloc_vextent_start_ag(args, ap->blkno);
	if (error || args->fsbno != NULLFSBLOCK)
		return error;

	return xfs_bmap_btalloc_low_space(ap, args);
}

static int
xfs_bmap_btalloc(
	struct xfs_bmalloca	*ap)
{
	struct xfs_mount	*mp = ap->ip->i_mount;
	struct xfs_alloc_arg	args = {
		.tp		= ap->tp,
		.mp		= mp,
		.fsbno		= NULLFSBLOCK,
		.oinfo		= XFS_RMAP_OINFO_SKIP_UPDATE,
		.minleft	= ap->minleft,
		.wasdel		= ap->wasdel,
		.resv		= XFS_AG_RESV_NONE,
		.datatype	= ap->datatype,
		.alignment	= 1,
		.minalignslop	= 0,
	};
	xfs_fileoff_t		orig_offset;
	xfs_extlen_t		orig_length;
	int			error;
	int			stripe_align;

	ASSERT(ap->length);
	orig_offset = ap->offset;
	orig_length = ap->length;

	stripe_align = xfs_bmap_compute_alignments(ap, &args);

	/* Trim the allocation back to the maximum an AG can fit. */
	args.maxlen = min(ap->length, mp->m_ag_max_usable);

	if (unlikely(XFS_TEST_ERROR(false, mp,
			XFS_ERRTAG_BMAP_ALLOC_MINLEN_EXTENT)))
		error = xfs_bmap_exact_minlen_extent_alloc(ap, &args);
	else if ((ap->datatype & XFS_ALLOC_USERDATA) &&
			xfs_inode_is_filestream(ap->ip))
		error = xfs_bmap_btalloc_filestreams(ap, &args, stripe_align);
	else
		error = xfs_bmap_btalloc_best_length(ap, &args, stripe_align);
	if (error)
		return error;

	if (args.fsbno != NULLFSBLOCK) {
		xfs_bmap_process_allocated_extent(ap, &args, orig_offset,
			orig_length);
	} else {
		ap->blkno = NULLFSBLOCK;
		ap->length = 0;
	}
	return 0;
}

/* Trim extent to fit a logical block range. */
void
xfs_trim_extent(
	struct xfs_bmbt_irec	*irec,
	xfs_fileoff_t		bno,
	xfs_filblks_t		len)
{
	xfs_fileoff_t		distance;
	xfs_fileoff_t		end = bno + len;

	if (irec->br_startoff + irec->br_blockcount <= bno ||
	    irec->br_startoff >= end) {
		irec->br_blockcount = 0;
		return;
	}

	if (irec->br_startoff < bno) {
		distance = bno - irec->br_startoff;
		if (isnullstartblock(irec->br_startblock))
			irec->br_startblock = DELAYSTARTBLOCK;
		if (irec->br_startblock != DELAYSTARTBLOCK &&
		    irec->br_startblock != HOLESTARTBLOCK)
			irec->br_startblock += distance;
		irec->br_startoff += distance;
		irec->br_blockcount -= distance;
	}

	if (end < irec->br_startoff + irec->br_blockcount) {
		distance = irec->br_startoff + irec->br_blockcount - end;
		irec->br_blockcount -= distance;
	}
}

/*
 * Trim the returned map to the required bounds
 */
STATIC void
xfs_bmapi_trim_map(
	struct xfs_bmbt_irec	*mval,
	struct xfs_bmbt_irec	*got,
	xfs_fileoff_t		*bno,
	xfs_filblks_t		len,
	xfs_fileoff_t		obno,
	xfs_fileoff_t		end,
	int			n,
	uint32_t		flags)
{
	if ((flags & XFS_BMAPI_ENTIRE) ||
	    got->br_startoff + got->br_blockcount <= obno) {
		*mval = *got;
		if (isnullstartblock(got->br_startblock))
			mval->br_startblock = DELAYSTARTBLOCK;
		return;
	}

	if (obno > *bno)
		*bno = obno;
	ASSERT((*bno >= obno) || (n == 0));
	ASSERT(*bno < end);
	mval->br_startoff = *bno;
	if (isnullstartblock(got->br_startblock))
		mval->br_startblock = DELAYSTARTBLOCK;
	else
		mval->br_startblock = got->br_startblock +
					(*bno - got->br_startoff);
	/*
	 * Return the minimum of what we got and what we asked for for
	 * the length.  We can use the len variable here because it is
	 * modified below and we could have been there before coming
	 * here if the first part of the allocation didn't overlap what
	 * was asked for.
	 */
	mval->br_blockcount = XFS_FILBLKS_MIN(end - *bno,
			got->br_blockcount - (*bno - got->br_startoff));
	mval->br_state = got->br_state;
	ASSERT(mval->br_blockcount <= len);
	return;
}

/*
 * Update and validate the extent map to return
 */
STATIC void
xfs_bmapi_update_map(
	struct xfs_bmbt_irec	**map,
	xfs_fileoff_t		*bno,
	xfs_filblks_t		*len,
	xfs_fileoff_t		obno,
	xfs_fileoff_t		end,
	int			*n,
	uint32_t		flags)
{
	xfs_bmbt_irec_t	*mval = *map;

	ASSERT((flags & XFS_BMAPI_ENTIRE) ||
	       ((mval->br_startoff + mval->br_blockcount) <= end));
	ASSERT((flags & XFS_BMAPI_ENTIRE) || (mval->br_blockcount <= *len) ||
	       (mval->br_startoff < obno));

	*bno = mval->br_startoff + mval->br_blockcount;
	*len = end - *bno;
	if (*n > 0 && mval->br_startoff == mval[-1].br_startoff) {
		/* update previous map with new information */
		ASSERT(mval->br_startblock == mval[-1].br_startblock);
		ASSERT(mval->br_blockcount > mval[-1].br_blockcount);
		ASSERT(mval->br_state == mval[-1].br_state);
		mval[-1].br_blockcount = mval->br_blockcount;
		mval[-1].br_state = mval->br_state;
	} else if (*n > 0 && mval->br_startblock != DELAYSTARTBLOCK &&
		   mval[-1].br_startblock != DELAYSTARTBLOCK &&
		   mval[-1].br_startblock != HOLESTARTBLOCK &&
		   mval->br_startblock == mval[-1].br_startblock +
					  mval[-1].br_blockcount &&
		   mval[-1].br_state == mval->br_state) {
		ASSERT(mval->br_startoff ==
		       mval[-1].br_startoff + mval[-1].br_blockcount);
		mval[-1].br_blockcount += mval->br_blockcount;
	} else if (*n > 0 &&
		   mval->br_startblock == DELAYSTARTBLOCK &&
		   mval[-1].br_startblock == DELAYSTARTBLOCK &&
		   mval->br_startoff ==
		   mval[-1].br_startoff + mval[-1].br_blockcount) {
		mval[-1].br_blockcount += mval->br_blockcount;
		mval[-1].br_state = mval->br_state;
	} else if (!((*n == 0) &&
		     ((mval->br_startoff + mval->br_blockcount) <=
		      obno))) {
		mval++;
		(*n)++;
	}
	*map = mval;
}

/*
 * Map file blocks to filesystem blocks without allocation.
 */
int
xfs_bmapi_read(
	struct xfs_inode	*ip,
	xfs_fileoff_t		bno,
	xfs_filblks_t		len,
	struct xfs_bmbt_irec	*mval,
	int			*nmap,
	uint32_t		flags)
{
	struct xfs_mount	*mp = ip->i_mount;
	int			whichfork = xfs_bmapi_whichfork(flags);
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec	got;
	xfs_fileoff_t		obno;
	xfs_fileoff_t		end;
	struct xfs_iext_cursor	icur;
	int			error;
	bool			eof = false;
	int			n = 0;

	ASSERT(*nmap >= 1);
	ASSERT(!(flags & ~(XFS_BMAPI_ATTRFORK | XFS_BMAPI_ENTIRE)));
	xfs_assert_ilocked(ip, XFS_ILOCK_SHARED | XFS_ILOCK_EXCL);

	if (WARN_ON_ONCE(!ifp)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	XFS_STATS_INC(mp, xs_blk_mapr);

	error = xfs_iread_extents(NULL, ip, whichfork);
	if (error)
		return error;

	if (!xfs_iext_lookup_extent(ip, ifp, bno, &icur, &got))
		eof = true;
	end = bno + len;
	obno = bno;

	while (bno < end && n < *nmap) {
		/* Reading past eof, act as though there's a hole up to end. */
		if (eof)
			got.br_startoff = end;
		if (got.br_startoff > bno) {
			/* Reading in a hole.  */
			mval->br_startoff = bno;
			mval->br_startblock = HOLESTARTBLOCK;
			mval->br_blockcount =
				XFS_FILBLKS_MIN(len, got.br_startoff - bno);
			mval->br_state = XFS_EXT_NORM;
			bno += mval->br_blockcount;
			len -= mval->br_blockcount;
			mval++;
			n++;
			continue;
		}

		/* set up the extent map to return. */
		xfs_bmapi_trim_map(mval, &got, &bno, len, obno, end, n, flags);
		xfs_bmapi_update_map(&mval, &bno, &len, obno, end, &n, flags);

		/* If we're done, stop now. */
		if (bno >= end || n >= *nmap)
			break;

		/* Else go on to the next record. */
		if (!xfs_iext_next_extent(ifp, &icur, &got))
			eof = true;
	}
	*nmap = n;
	return 0;
}

static int
xfs_bmapi_allocate(
	struct xfs_bmalloca	*bma)
{
	struct xfs_mount	*mp = bma->ip->i_mount;
	int			whichfork = xfs_bmapi_whichfork(bma->flags);
	struct xfs_ifork	*ifp = xfs_ifork_ptr(bma->ip, whichfork);
	int			error;

	ASSERT(bma->length > 0);
	ASSERT(bma->length <= XFS_MAX_BMBT_EXTLEN);

	if (bma->flags & XFS_BMAPI_CONTIG)
		bma->minlen = bma->length;
	else
		bma->minlen = 1;

	if (!(bma->flags & XFS_BMAPI_METADATA)) {
		/*
		 * For the data and COW fork, the first data in the file is
		 * treated differently to all other allocations. For the
		 * attribute fork, we only need to ensure the allocated range
		 * is not on the busy list.
		 */
		bma->datatype = XFS_ALLOC_NOBUSY;
		if (whichfork == XFS_DATA_FORK || whichfork == XFS_COW_FORK) {
			bma->datatype |= XFS_ALLOC_USERDATA;
			if (bma->offset == 0)
				bma->datatype |= XFS_ALLOC_INITIAL_USER_DATA;

			if (mp->m_dalign && bma->length >= mp->m_dalign) {
				error = xfs_bmap_isaeof(bma, whichfork);
				if (error)
					return error;
			}
		}
	}

	if ((bma->datatype & XFS_ALLOC_USERDATA) &&
	    XFS_IS_REALTIME_INODE(bma->ip))
		error = xfs_bmap_rtalloc(bma);
	else
		error = xfs_bmap_btalloc(bma);
	if (error)
		return error;
	if (bma->blkno == NULLFSBLOCK)
		return -ENOSPC;

	if (WARN_ON_ONCE(!xfs_valid_startblock(bma->ip, bma->blkno))) {
		xfs_bmap_mark_sick(bma->ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (bma->flags & XFS_BMAPI_ZERO) {
		error = xfs_zero_extent(bma->ip, bma->blkno, bma->length);
		if (error)
			return error;
	}

	if (ifp->if_format == XFS_DINODE_FMT_BTREE && !bma->cur)
		bma->cur = xfs_bmbt_init_cursor(mp, bma->tp, bma->ip, whichfork);
	/*
	 * Bump the number of extents we've allocated
	 * in this call.
	 */
	bma->nallocs++;

	if (bma->cur && bma->wasdel)
		bma->cur->bc_flags |= XFS_BTREE_BMBT_WASDEL;

	bma->got.br_startoff = bma->offset;
	bma->got.br_startblock = bma->blkno;
	bma->got.br_blockcount = bma->length;
	bma->got.br_state = XFS_EXT_NORM;

	if (bma->flags & XFS_BMAPI_PREALLOC)
		bma->got.br_state = XFS_EXT_UNWRITTEN;

	if (bma->wasdel)
		error = xfs_bmap_add_extent_delay_real(bma, whichfork);
	else
		error = xfs_bmap_add_extent_hole_real(bma->tp, bma->ip,
				whichfork, &bma->icur, &bma->cur, &bma->got,
				&bma->logflags, bma->flags);
	if (error)
		return error;

	/*
	 * Update our extent pointer, given that xfs_bmap_add_extent_delay_real
	 * or xfs_bmap_add_extent_hole_real might have merged it into one of
	 * the neighbouring ones.
	 */
	xfs_iext_get_extent(ifp, &bma->icur, &bma->got);

	ASSERT(bma->got.br_startoff <= bma->offset);
	ASSERT(bma->got.br_startoff + bma->got.br_blockcount >=
	       bma->offset + bma->length);
	ASSERT(bma->got.br_state == XFS_EXT_NORM ||
	       bma->got.br_state == XFS_EXT_UNWRITTEN);
	return 0;
}

STATIC int
xfs_bmapi_convert_unwritten(
	struct xfs_bmalloca	*bma,
	struct xfs_bmbt_irec	*mval,
	xfs_filblks_t		len,
	uint32_t		flags)
{
	int			whichfork = xfs_bmapi_whichfork(flags);
	struct xfs_ifork	*ifp = xfs_ifork_ptr(bma->ip, whichfork);
	int			tmp_logflags = 0;
	int			error;

	/* check if we need to do unwritten->real conversion */
	if (mval->br_state == XFS_EXT_UNWRITTEN &&
	    (flags & XFS_BMAPI_PREALLOC))
		return 0;

	/* check if we need to do real->unwritten conversion */
	if (mval->br_state == XFS_EXT_NORM &&
	    (flags & (XFS_BMAPI_PREALLOC | XFS_BMAPI_CONVERT)) !=
			(XFS_BMAPI_PREALLOC | XFS_BMAPI_CONVERT))
		return 0;

	/*
	 * Modify (by adding) the state flag, if writing.
	 */
	ASSERT(mval->br_blockcount <= len);
	if (ifp->if_format == XFS_DINODE_FMT_BTREE && !bma->cur) {
		bma->cur = xfs_bmbt_init_cursor(bma->ip->i_mount, bma->tp,
					bma->ip, whichfork);
	}
	mval->br_state = (mval->br_state == XFS_EXT_UNWRITTEN)
				? XFS_EXT_NORM : XFS_EXT_UNWRITTEN;

	/*
	 * Before insertion into the bmbt, zero the range being converted
	 * if required.
	 */
	if (flags & XFS_BMAPI_ZERO) {
		error = xfs_zero_extent(bma->ip, mval->br_startblock,
					mval->br_blockcount);
		if (error)
			return error;
	}

	error = xfs_bmap_add_extent_unwritten_real(bma->tp, bma->ip, whichfork,
			&bma->icur, &bma->cur, mval, &tmp_logflags);
	/*
	 * Log the inode core unconditionally in the unwritten extent conversion
	 * path because the conversion might not have done so (e.g., if the
	 * extent count hasn't changed). We need to make sure the inode is dirty
	 * in the transaction for the sake of fsync(), even if nothing has
	 * changed, because fsync() will not force the log for this transaction
	 * unless it sees the inode pinned.
	 *
	 * Note: If we're only converting cow fork extents, there aren't
	 * any on-disk updates to make, so we don't need to log anything.
	 */
	if (whichfork != XFS_COW_FORK)
		bma->logflags |= tmp_logflags | XFS_ILOG_CORE;
	if (error)
		return error;

	/*
	 * Update our extent pointer, given that
	 * xfs_bmap_add_extent_unwritten_real might have merged it into one
	 * of the neighbouring ones.
	 */
	xfs_iext_get_extent(ifp, &bma->icur, &bma->got);

	/*
	 * We may have combined previously unwritten space with written space,
	 * so generate another request.
	 */
	if (mval->br_blockcount < len)
		return -EAGAIN;
	return 0;
}

xfs_extlen_t
xfs_bmapi_minleft(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			fork)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, fork);

	if (tp && tp->t_highest_agno != NULLAGNUMBER)
		return 0;
	if (ifp->if_format != XFS_DINODE_FMT_BTREE)
		return 1;
	return be16_to_cpu(ifp->if_broot->bb_level) + 1;
}

/*
 * Log whatever the flags say, even if error.  Otherwise we might miss detecting
 * a case where the data is changed, there's an error, and it's not logged so we
 * don't shutdown when we should.  Don't bother logging extents/btree changes if
 * we converted to the other format.
 */
static void
xfs_bmapi_finish(
	struct xfs_bmalloca	*bma,
	int			whichfork,
	int			error)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(bma->ip, whichfork);

	if ((bma->logflags & xfs_ilog_fext(whichfork)) &&
	    ifp->if_format != XFS_DINODE_FMT_EXTENTS)
		bma->logflags &= ~xfs_ilog_fext(whichfork);
	else if ((bma->logflags & xfs_ilog_fbroot(whichfork)) &&
		 ifp->if_format != XFS_DINODE_FMT_BTREE)
		bma->logflags &= ~xfs_ilog_fbroot(whichfork);

	if (bma->logflags)
		xfs_trans_log_inode(bma->tp, bma->ip, bma->logflags);
	if (bma->cur)
		xfs_btree_del_cursor(bma->cur, error);
}

/*
 * Map file blocks to filesystem blocks, and allocate blocks or convert the
 * extent state if necessary.  Details behaviour is controlled by the flags
 * parameter.  Only allocates blocks from a single allocation group, to avoid
 * locking problems.
 *
 * Returns 0 on success and places the extent mappings in mval.  nmaps is used
 * as an input/output parameter where the caller specifies the maximum number
 * of mappings that may be returned and xfs_bmapi_write passes back the number
 * of mappings (including existing mappings) it found.
 *
 * Returns a negative error code on failure, including -ENOSPC when it could not
 * allocate any blocks and -ENOSR when it did allocate blocks to convert a
 * delalloc range, but those blocks were before the passed in range.
 */
int
xfs_bmapi_write(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode */
	xfs_fileoff_t		bno,		/* starting file offs. mapped */
	xfs_filblks_t		len,		/* length to map in file */
	uint32_t		flags,		/* XFS_BMAPI_... */
	xfs_extlen_t		total,		/* total blocks needed */
	struct xfs_bmbt_irec	*mval,		/* output: map values */
	int			*nmap)		/* i/o: mval size/count */
{
	struct xfs_bmalloca	bma = {
		.tp		= tp,
		.ip		= ip,
		.total		= total,
	};
	struct xfs_mount	*mp = ip->i_mount;
	int			whichfork = xfs_bmapi_whichfork(flags);
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	xfs_fileoff_t		end;		/* end of mapped file region */
	bool			eof = false;	/* after the end of extents */
	int			error;		/* error return */
	int			n;		/* current extent index */
	xfs_fileoff_t		obno;		/* old block number (offset) */

#ifdef DEBUG
	xfs_fileoff_t		orig_bno;	/* original block number value */
	int			orig_flags;	/* original flags arg value */
	xfs_filblks_t		orig_len;	/* original value of len arg */
	struct xfs_bmbt_irec	*orig_mval;	/* original value of mval */
	int			orig_nmap;	/* original value of *nmap */

	orig_bno = bno;
	orig_len = len;
	orig_flags = flags;
	orig_mval = mval;
	orig_nmap = *nmap;
#endif

	ASSERT(*nmap >= 1);
	ASSERT(*nmap <= XFS_BMAP_MAX_NMAP);
	ASSERT(tp != NULL);
	ASSERT(len > 0);
	ASSERT(ifp->if_format != XFS_DINODE_FMT_LOCAL);
	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);
	ASSERT(!(flags & XFS_BMAPI_REMAP));

	/* zeroing is for currently only for data extents, not metadata */
	ASSERT((flags & (XFS_BMAPI_METADATA | XFS_BMAPI_ZERO)) !=
			(XFS_BMAPI_METADATA | XFS_BMAPI_ZERO));
	/*
	 * we can allocate unwritten extents or pre-zero allocated blocks,
	 * but it makes no sense to do both at once. This would result in
	 * zeroing the unwritten extent twice, but it still being an
	 * unwritten extent....
	 */
	ASSERT((flags & (XFS_BMAPI_PREALLOC | XFS_BMAPI_ZERO)) !=
			(XFS_BMAPI_PREALLOC | XFS_BMAPI_ZERO));

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	XFS_STATS_INC(mp, xs_blk_mapw);

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		goto error0;

	if (!xfs_iext_lookup_extent(ip, ifp, bno, &bma.icur, &bma.got))
		eof = true;
	if (!xfs_iext_peek_prev_extent(ifp, &bma.icur, &bma.prev))
		bma.prev.br_startoff = NULLFILEOFF;
	bma.minleft = xfs_bmapi_minleft(tp, ip, whichfork);

	n = 0;
	end = bno + len;
	obno = bno;
	while (bno < end && n < *nmap) {
		bool			need_alloc = false, wasdelay = false;

		/* in hole or beyond EOF? */
		if (eof || bma.got.br_startoff > bno) {
			/*
			 * CoW fork conversions should /never/ hit EOF or
			 * holes.  There should always be something for us
			 * to work on.
			 */
			ASSERT(!((flags & XFS_BMAPI_CONVERT) &&
			         (flags & XFS_BMAPI_COWFORK)));

			need_alloc = true;
		} else if (isnullstartblock(bma.got.br_startblock)) {
			wasdelay = true;
		}

		/*
		 * First, deal with the hole before the allocated space
		 * that we found, if any.
		 */
		if (need_alloc || wasdelay) {
			bma.eof = eof;
			bma.conv = !!(flags & XFS_BMAPI_CONVERT);
			bma.wasdel = wasdelay;
			bma.offset = bno;
			bma.flags = flags;

			/*
			 * There's a 32/64 bit type mismatch between the
			 * allocation length request (which can be 64 bits in
			 * length) and the bma length request, which is
			 * xfs_extlen_t and therefore 32 bits. Hence we have to
			 * be careful and do the min() using the larger type to
			 * avoid overflows.
			 */
			bma.length = XFS_FILBLKS_MIN(len, XFS_MAX_BMBT_EXTLEN);

			if (wasdelay) {
				bma.length = XFS_FILBLKS_MIN(bma.length,
					bma.got.br_blockcount -
					(bno - bma.got.br_startoff));
			} else {
				if (!eof)
					bma.length = XFS_FILBLKS_MIN(bma.length,
						bma.got.br_startoff - bno);
			}

			ASSERT(bma.length > 0);
			error = xfs_bmapi_allocate(&bma);
			if (error) {
				/*
				 * If we already allocated space in a previous
				 * iteration return what we go so far when
				 * running out of space.
				 */
				if (error == -ENOSPC && bma.nallocs)
					break;
				goto error0;
			}

			/*
			 * If this is a CoW allocation, record the data in
			 * the refcount btree for orphan recovery.
			 */
			if (whichfork == XFS_COW_FORK)
				xfs_refcount_alloc_cow_extent(tp,
						XFS_IS_REALTIME_INODE(ip),
						bma.blkno, bma.length);
		}

		/* Deal with the allocated space we found.  */
		xfs_bmapi_trim_map(mval, &bma.got, &bno, len, obno,
							end, n, flags);

		/* Execute unwritten extent conversion if necessary */
		error = xfs_bmapi_convert_unwritten(&bma, mval, len, flags);
		if (error == -EAGAIN)
			continue;
		if (error)
			goto error0;

		/* update the extent map to return */
		xfs_bmapi_update_map(&mval, &bno, &len, obno, end, &n, flags);

		/*
		 * If we're done, stop now.  Stop when we've allocated
		 * XFS_BMAP_MAX_NMAP extents no matter what.  Otherwise
		 * the transaction may get too big.
		 */
		if (bno >= end || n >= *nmap || bma.nallocs >= *nmap)
			break;

		/* Else go on to the next record. */
		bma.prev = bma.got;
		if (!xfs_iext_next_extent(ifp, &bma.icur, &bma.got))
			eof = true;
	}

	error = xfs_bmap_btree_to_extents(tp, ip, bma.cur, &bma.logflags,
			whichfork);
	if (error)
		goto error0;

	ASSERT(ifp->if_format != XFS_DINODE_FMT_BTREE ||
	       ifp->if_nextents > XFS_IFORK_MAXEXT(ip, whichfork));
	xfs_bmapi_finish(&bma, whichfork, 0);
	xfs_bmap_validate_ret(orig_bno, orig_len, orig_flags, orig_mval,
		orig_nmap, n);

	/*
	 * When converting delayed allocations, xfs_bmapi_allocate ignores
	 * the passed in bno and always converts from the start of the found
	 * delalloc extent.
	 *
	 * To avoid a successful return with *nmap set to 0, return the magic
	 * -ENOSR error code for this particular case so that the caller can
	 * handle it.
	 */
	if (!n) {
		ASSERT(bma.nallocs >= *nmap);
		return -ENOSR;
	}
	*nmap = n;
	return 0;
error0:
	xfs_bmapi_finish(&bma, whichfork, error);
	return error;
}

/*
 * Convert an existing delalloc extent to real blocks based on file offset. This
 * attempts to allocate the entire delalloc extent and may require multiple
 * invocations to allocate the target offset if a large enough physical extent
 * is not available.
 */
static int
xfs_bmapi_convert_one_delalloc(
	struct xfs_inode	*ip,
	int			whichfork,
	xfs_off_t		offset,
	struct iomap		*iomap,
	unsigned int		*seq)
{
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_mount	*mp = ip->i_mount;
	xfs_fileoff_t		offset_fsb = XFS_B_TO_FSBT(mp, offset);
	struct xfs_bmalloca	bma = { NULL };
	uint16_t		flags = 0;
	struct xfs_trans	*tp;
	int			error;

	if (whichfork == XFS_COW_FORK)
		flags |= IOMAP_F_SHARED;

	/*
	 * Space for the extent and indirect blocks was reserved when the
	 * delalloc extent was created so there's no need to do so here.
	 */
	error = xfs_trans_alloc(mp, &M_RES(mp)->tr_write, 0, 0,
				XFS_TRANS_RESERVE, &tp);
	if (error)
		return error;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	xfs_trans_ijoin(tp, ip, 0);

	error = xfs_iext_count_extend(tp, ip, whichfork,
			XFS_IEXT_ADD_NOSPLIT_CNT);
	if (error)
		goto out_trans_cancel;

	if (!xfs_iext_lookup_extent(ip, ifp, offset_fsb, &bma.icur, &bma.got) ||
	    bma.got.br_startoff > offset_fsb) {
		/*
		 * No extent found in the range we are trying to convert.  This
		 * should only happen for the COW fork, where another thread
		 * might have moved the extent to the data fork in the meantime.
		 */
		WARN_ON_ONCE(whichfork != XFS_COW_FORK);
		error = -EAGAIN;
		goto out_trans_cancel;
	}

	/*
	 * If we find a real extent here we raced with another thread converting
	 * the extent.  Just return the real extent at this offset.
	 */
	if (!isnullstartblock(bma.got.br_startblock)) {
		xfs_bmbt_to_iomap(ip, iomap, &bma.got, 0, flags,
				xfs_iomap_inode_sequence(ip, flags));
		if (seq)
			*seq = READ_ONCE(ifp->if_seq);
		goto out_trans_cancel;
	}

	bma.tp = tp;
	bma.ip = ip;
	bma.wasdel = true;
	bma.minleft = xfs_bmapi_minleft(tp, ip, whichfork);

	/*
	 * Always allocate convert from the start of the delalloc extent even if
	 * that is outside the passed in range to create large contiguous
	 * extents on disk.
	 */
	bma.offset = bma.got.br_startoff;
	bma.length = bma.got.br_blockcount;

	/*
	 * When we're converting the delalloc reservations backing dirty pages
	 * in the page cache, we must be careful about how we create the new
	 * extents:
	 *
	 * New CoW fork extents are created unwritten, turned into real extents
	 * when we're about to write the data to disk, and mapped into the data
	 * fork after the write finishes.  End of story.
	 *
	 * New data fork extents must be mapped in as unwritten and converted
	 * to real extents after the write succeeds to avoid exposing stale
	 * disk contents if we crash.
	 */
	bma.flags = XFS_BMAPI_PREALLOC;
	if (whichfork == XFS_COW_FORK)
		bma.flags |= XFS_BMAPI_COWFORK;

	if (!xfs_iext_peek_prev_extent(ifp, &bma.icur, &bma.prev))
		bma.prev.br_startoff = NULLFILEOFF;

	error = xfs_bmapi_allocate(&bma);
	if (error)
		goto out_finish;

	XFS_STATS_ADD(mp, xs_xstrat_bytes, XFS_FSB_TO_B(mp, bma.length));
	XFS_STATS_INC(mp, xs_xstrat_quick);

	ASSERT(!isnullstartblock(bma.got.br_startblock));
	xfs_bmbt_to_iomap(ip, iomap, &bma.got, 0, flags,
				xfs_iomap_inode_sequence(ip, flags));
	if (seq)
		*seq = READ_ONCE(ifp->if_seq);

	if (whichfork == XFS_COW_FORK)
		xfs_refcount_alloc_cow_extent(tp, XFS_IS_REALTIME_INODE(ip),
				bma.blkno, bma.length);

	error = xfs_bmap_btree_to_extents(tp, ip, bma.cur, &bma.logflags,
			whichfork);
	if (error)
		goto out_finish;

	xfs_bmapi_finish(&bma, whichfork, 0);
	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;

out_finish:
	xfs_bmapi_finish(&bma, whichfork, error);
out_trans_cancel:
	xfs_trans_cancel(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);
	return error;
}

/*
 * Pass in a dellalloc extent and convert it to real extents, return the real
 * extent that maps offset_fsb in iomap.
 */
int
xfs_bmapi_convert_delalloc(
	struct xfs_inode	*ip,
	int			whichfork,
	loff_t			offset,
	struct iomap		*iomap,
	unsigned int		*seq)
{
	int			error;

	/*
	 * Attempt to allocate whatever delalloc extent currently backs offset
	 * and put the result into iomap.  Allocate in a loop because it may
	 * take several attempts to allocate real blocks for a contiguous
	 * delalloc extent if free space is sufficiently fragmented.
	 */
	do {
		error = xfs_bmapi_convert_one_delalloc(ip, whichfork, offset,
					iomap, seq);
		if (error)
			return error;
	} while (iomap->offset + iomap->length <= offset);

	return 0;
}

int
xfs_bmapi_remap(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_fileoff_t		bno,
	xfs_filblks_t		len,
	xfs_fsblock_t		startblock,
	uint32_t		flags)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp;
	struct xfs_btree_cur	*cur = NULL;
	struct xfs_bmbt_irec	got;
	struct xfs_iext_cursor	icur;
	int			whichfork = xfs_bmapi_whichfork(flags);
	int			logflags = 0, error;

	ifp = xfs_ifork_ptr(ip, whichfork);
	ASSERT(len > 0);
	ASSERT(len <= (xfs_filblks_t)XFS_MAX_BMBT_EXTLEN);
	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);
	ASSERT(!(flags & ~(XFS_BMAPI_ATTRFORK | XFS_BMAPI_PREALLOC |
			   XFS_BMAPI_NORMAP)));
	ASSERT((flags & (XFS_BMAPI_ATTRFORK | XFS_BMAPI_PREALLOC)) !=
			(XFS_BMAPI_ATTRFORK | XFS_BMAPI_PREALLOC));

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	if (xfs_iext_lookup_extent(ip, ifp, bno, &icur, &got)) {
		/* make sure we only reflink into a hole. */
		ASSERT(got.br_startoff > bno);
		ASSERT(got.br_startoff - bno >= len);
	}

	ip->i_nblocks += len;
	ip->i_delayed_blks -= len; /* see xfs_bmap_defer_add */
	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	if (ifp->if_format == XFS_DINODE_FMT_BTREE)
		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);

	got.br_startoff = bno;
	got.br_startblock = startblock;
	got.br_blockcount = len;
	if (flags & XFS_BMAPI_PREALLOC)
		got.br_state = XFS_EXT_UNWRITTEN;
	else
		got.br_state = XFS_EXT_NORM;

	error = xfs_bmap_add_extent_hole_real(tp, ip, whichfork, &icur,
			&cur, &got, &logflags, flags);
	if (error)
		goto error0;

	error = xfs_bmap_btree_to_extents(tp, ip, cur, &logflags, whichfork);

error0:
	if (ip->i_df.if_format != XFS_DINODE_FMT_EXTENTS)
		logflags &= ~XFS_ILOG_DEXT;
	else if (ip->i_df.if_format != XFS_DINODE_FMT_BTREE)
		logflags &= ~XFS_ILOG_DBROOT;

	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	if (cur)
		xfs_btree_del_cursor(cur, error);
	return error;
}

/*
 * When a delalloc extent is split (e.g., due to a hole punch), the original
 * indlen reservation must be shared across the two new extents that are left
 * behind.
 *
 * Given the original reservation and the worst case indlen for the two new
 * extents (as calculated by xfs_bmap_worst_indlen()), split the original
 * reservation fairly across the two new extents. If necessary, steal available
 * blocks from a deleted extent to make up a reservation deficiency (e.g., if
 * ores == 1). The number of stolen blocks is returned. The availability and
 * subsequent accounting of stolen blocks is the responsibility of the caller.
 */
static void
xfs_bmap_split_indlen(
	xfs_filblks_t			ores,		/* original res. */
	xfs_filblks_t			*indlen1,	/* ext1 worst indlen */
	xfs_filblks_t			*indlen2)	/* ext2 worst indlen */
{
	xfs_filblks_t			len1 = *indlen1;
	xfs_filblks_t			len2 = *indlen2;
	xfs_filblks_t			nres = len1 + len2; /* new total res. */
	xfs_filblks_t			resfactor;

	/*
	 * We can't meet the total required reservation for the two extents.
	 * Calculate the percent of the overall shortage between both extents
	 * and apply this percentage to each of the requested indlen values.
	 * This distributes the shortage fairly and reduces the chances that one
	 * of the two extents is left with nothing when extents are repeatedly
	 * split.
	 */
	resfactor = (ores * 100);
	do_div(resfactor, nres);
	len1 *= resfactor;
	do_div(len1, 100);
	len2 *= resfactor;
	do_div(len2, 100);
	ASSERT(len1 + len2 <= ores);
	ASSERT(len1 < *indlen1 && len2 < *indlen2);

	/*
	 * Hand out the remainder to each extent. If one of the two reservations
	 * is zero, we want to make sure that one gets a block first. The loop
	 * below starts with len1, so hand len2 a block right off the bat if it
	 * is zero.
	 */
	ores -= (len1 + len2);
	ASSERT((*indlen1 - len1) + (*indlen2 - len2) >= ores);
	if (ores && !len2 && *indlen2) {
		len2++;
		ores--;
	}
	while (ores) {
		if (len1 < *indlen1) {
			len1++;
			ores--;
		}
		if (!ores)
			break;
		if (len2 < *indlen2) {
			len2++;
			ores--;
		}
	}

	*indlen1 = len1;
	*indlen2 = len2;
}

void
xfs_bmap_del_extent_delay(
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_iext_cursor	*icur,
	struct xfs_bmbt_irec	*got,
	struct xfs_bmbt_irec	*del,
	uint32_t		bflags)	/* bmapi flags */
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec	new;
	int64_t			da_old, da_new, da_diff = 0;
	xfs_fileoff_t		del_endoff, got_endoff;
	xfs_filblks_t		got_indlen, new_indlen, stolen = 0;
	uint32_t		state = xfs_bmap_fork_to_state(whichfork);
	uint64_t		fdblocks;
	bool			isrt;

	XFS_STATS_INC(mp, xs_del_exlist);

	isrt = xfs_ifork_is_realtime(ip, whichfork);
	del_endoff = del->br_startoff + del->br_blockcount;
	got_endoff = got->br_startoff + got->br_blockcount;
	da_old = startblockval(got->br_startblock);
	da_new = 0;

	ASSERT(del->br_blockcount > 0);
	ASSERT(got->br_startoff <= del->br_startoff);
	ASSERT(got_endoff >= del_endoff);

	/*
	 * Update the inode delalloc counter now and wait to update the
	 * sb counters as we might have to borrow some blocks for the
	 * indirect block accounting.
	 */
	xfs_quota_unreserve_blkres(ip, del->br_blockcount);
	ip->i_delayed_blks -= del->br_blockcount;

	if (got->br_startoff == del->br_startoff)
		state |= BMAP_LEFT_FILLING;
	if (got_endoff == del_endoff)
		state |= BMAP_RIGHT_FILLING;

	switch (state & (BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING)) {
	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING:
		/*
		 * Matches the whole extent.  Delete the entry.
		 */
		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		break;
	case BMAP_LEFT_FILLING:
		/*
		 * Deleting the first part of the extent.
		 */
		got->br_startoff = del_endoff;
		got->br_blockcount -= del->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip,
				got->br_blockcount), da_old);
		got->br_startblock = nullstartblock((int)da_new);
		xfs_iext_update_extent(ip, state, icur, got);
		break;
	case BMAP_RIGHT_FILLING:
		/*
		 * Deleting the last part of the extent.
		 */
		got->br_blockcount = got->br_blockcount - del->br_blockcount;
		da_new = XFS_FILBLKS_MIN(xfs_bmap_worst_indlen(ip,
				got->br_blockcount), da_old);
		got->br_startblock = nullstartblock((int)da_new);
		xfs_iext_update_extent(ip, state, icur, got);
		break;
	case 0:
		/*
		 * Deleting the middle of the extent.
		 *
		 * Distribute the original indlen reservation across the two new
		 * extents.  Steal blocks from the deleted extent if necessary.
		 * Stealing blocks simply fudges the fdblocks accounting below.
		 * Warn if either of the new indlen reservations is zero as this
		 * can lead to delalloc problems.
		 */
		got->br_blockcount = del->br_startoff - got->br_startoff;
		got_indlen = xfs_bmap_worst_indlen(ip, got->br_blockcount);

		new.br_blockcount = got_endoff - del_endoff;
		new_indlen = xfs_bmap_worst_indlen(ip, new.br_blockcount);

		WARN_ON_ONCE(!got_indlen || !new_indlen);
		/*
		 * Steal as many blocks as we can to try and satisfy the worst
		 * case indlen for both new extents.
		 *
		 * However, we can't just steal reservations from the data
		 * blocks if this is an RT inodes as the data and metadata
		 * blocks come from different pools.  We'll have to live with
		 * under-filled indirect reservation in this case.
		 */
		da_new = got_indlen + new_indlen;
		if (da_new > da_old && !isrt) {
			stolen = XFS_FILBLKS_MIN(da_new - da_old,
						 del->br_blockcount);
			da_old += stolen;
		}
		if (da_new > da_old)
			xfs_bmap_split_indlen(da_old, &got_indlen, &new_indlen);
		da_new = got_indlen + new_indlen;

		got->br_startblock = nullstartblock((int)got_indlen);

		new.br_startoff = del_endoff;
		new.br_state = got->br_state;
		new.br_startblock = nullstartblock((int)new_indlen);

		xfs_iext_update_extent(ip, state, icur, got);
		xfs_iext_next(ifp, icur);
		xfs_iext_insert(ip, icur, &new, state);

		del->br_blockcount -= stolen;
		break;
	}

	ASSERT(da_old >= da_new);
	da_diff = da_old - da_new;
	fdblocks = da_diff;

	if (bflags & XFS_BMAPI_REMAP) {
		;
	} else if (isrt) {
		xfs_rtbxlen_t	rtxlen;

		rtxlen = xfs_blen_to_rtbxlen(mp, del->br_blockcount);
		if (xfs_is_zoned_inode(ip))
			xfs_zoned_add_available(mp, rtxlen);
		xfs_add_frextents(mp, rtxlen);
	} else {
		fdblocks += del->br_blockcount;
	}

	xfs_add_fdblocks(mp, fdblocks);
	xfs_mod_delalloc(ip, -(int64_t)del->br_blockcount, -da_diff);
}

void
xfs_bmap_del_extent_cow(
	struct xfs_inode	*ip,
	struct xfs_iext_cursor	*icur,
	struct xfs_bmbt_irec	*got,
	struct xfs_bmbt_irec	*del)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_COW_FORK);
	struct xfs_bmbt_irec	new;
	xfs_fileoff_t		del_endoff, got_endoff;
	uint32_t		state = BMAP_COWFORK;

	XFS_STATS_INC(mp, xs_del_exlist);

	del_endoff = del->br_startoff + del->br_blockcount;
	got_endoff = got->br_startoff + got->br_blockcount;

	ASSERT(del->br_blockcount > 0);
	ASSERT(got->br_startoff <= del->br_startoff);
	ASSERT(got_endoff >= del_endoff);
	ASSERT(!isnullstartblock(got->br_startblock));

	if (got->br_startoff == del->br_startoff)
		state |= BMAP_LEFT_FILLING;
	if (got_endoff == del_endoff)
		state |= BMAP_RIGHT_FILLING;

	switch (state & (BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING)) {
	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING:
		/*
		 * Matches the whole extent.  Delete the entry.
		 */
		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		break;
	case BMAP_LEFT_FILLING:
		/*
		 * Deleting the first part of the extent.
		 */
		got->br_startoff = del_endoff;
		got->br_blockcount -= del->br_blockcount;
		got->br_startblock = del->br_startblock + del->br_blockcount;
		xfs_iext_update_extent(ip, state, icur, got);
		break;
	case BMAP_RIGHT_FILLING:
		/*
		 * Deleting the last part of the extent.
		 */
		got->br_blockcount -= del->br_blockcount;
		xfs_iext_update_extent(ip, state, icur, got);
		break;
	case 0:
		/*
		 * Deleting the middle of the extent.
		 */
		got->br_blockcount = del->br_startoff - got->br_startoff;

		new.br_startoff = del_endoff;
		new.br_blockcount = got_endoff - del_endoff;
		new.br_state = got->br_state;
		new.br_startblock = del->br_startblock + del->br_blockcount;

		xfs_iext_update_extent(ip, state, icur, got);
		xfs_iext_next(ifp, icur);
		xfs_iext_insert(ip, icur, &new, state);
		break;
	}
	ip->i_delayed_blks -= del->br_blockcount;
}

static int
xfs_bmap_free_rtblocks(
	struct xfs_trans	*tp,
	struct xfs_bmbt_irec	*del)
{
	struct xfs_rtgroup	*rtg;
	int			error;

	rtg = xfs_rtgroup_grab(tp->t_mountp, 0);
	if (!rtg)
		return -EIO;

	/*
	 * Ensure the bitmap and summary inodes are locked and joined to the
	 * transaction before modifying them.
	 */
	if (!(tp->t_flags & XFS_TRANS_RTBITMAP_LOCKED)) {
		tp->t_flags |= XFS_TRANS_RTBITMAP_LOCKED;
		xfs_rtgroup_lock(rtg, XFS_RTGLOCK_BITMAP);
		xfs_rtgroup_trans_join(tp, rtg, XFS_RTGLOCK_BITMAP);
	}

	error = xfs_rtfree_blocks(tp, rtg, del->br_startblock,
			del->br_blockcount);
	xfs_rtgroup_rele(rtg);
	return error;
}

/*
 * Called by xfs_bmapi to update file extent records and the btree
 * after removing space.
 */
STATIC int				/* error */
xfs_bmap_del_extent_real(
	xfs_inode_t		*ip,	/* incore inode pointer */
	xfs_trans_t		*tp,	/* current transaction pointer */
	struct xfs_iext_cursor	*icur,
	struct xfs_btree_cur	*cur,	/* if null, not a btree */
	xfs_bmbt_irec_t		*del,	/* data to remove from extents */
	int			*logflagsp, /* inode logging flags */
	int			whichfork, /* data or attr fork */
	uint32_t		bflags)	/* bmapi flags */
{
	xfs_fsblock_t		del_endblock=0;	/* first block past del */
	xfs_fileoff_t		del_endoff;	/* first offset past del */
	int			error = 0;	/* error return value */
	struct xfs_bmbt_irec	got;	/* current extent entry */
	xfs_fileoff_t		got_endoff;	/* first offset past got */
	int			i;	/* temp state */
	struct xfs_ifork	*ifp;	/* inode fork pointer */
	xfs_mount_t		*mp;	/* mount structure */
	xfs_filblks_t		nblks;	/* quota/sb block count */
	xfs_bmbt_irec_t		new;	/* new record to be inserted */
	/* REFERENCED */
	uint			qfield;	/* quota field to update */
	uint32_t		state = xfs_bmap_fork_to_state(whichfork);
	struct xfs_bmbt_irec	old;

	*logflagsp = 0;

	mp = ip->i_mount;
	XFS_STATS_INC(mp, xs_del_exlist);

	ifp = xfs_ifork_ptr(ip, whichfork);
	ASSERT(del->br_blockcount > 0);
	xfs_iext_get_extent(ifp, icur, &got);
	ASSERT(got.br_startoff <= del->br_startoff);
	del_endoff = del->br_startoff + del->br_blockcount;
	got_endoff = got.br_startoff + got.br_blockcount;
	ASSERT(got_endoff >= del_endoff);
	ASSERT(!isnullstartblock(got.br_startblock));
	qfield = 0;

	/*
	 * If it's the case where the directory code is running with no block
	 * reservation, and the deleted block is in the middle of its extent,
	 * and the resulting insert of an extent would cause transformation to
	 * btree format, then reject it.  The calling code will then swap blocks
	 * around instead.  We have to do this now, rather than waiting for the
	 * conversion to btree format, since the transaction will be dirty then.
	 */
	if (tp->t_blk_res == 0 &&
	    ifp->if_format == XFS_DINODE_FMT_EXTENTS &&
	    ifp->if_nextents >= XFS_IFORK_MAXEXT(ip, whichfork) &&
	    del->br_startoff > got.br_startoff && del_endoff < got_endoff)
		return -ENOSPC;

	*logflagsp = XFS_ILOG_CORE;
	if (xfs_ifork_is_realtime(ip, whichfork))
		qfield = XFS_TRANS_DQ_RTBCOUNT;
	else
		qfield = XFS_TRANS_DQ_BCOUNT;
	nblks = del->br_blockcount;

	del_endblock = del->br_startblock + del->br_blockcount;
	if (cur) {
		error = xfs_bmbt_lookup_eq(cur, &got, &i);
		if (error)
			return error;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			return -EFSCORRUPTED;
		}
	}

	if (got.br_startoff == del->br_startoff)
		state |= BMAP_LEFT_FILLING;
	if (got_endoff == del_endoff)
		state |= BMAP_RIGHT_FILLING;

	switch (state & (BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING)) {
	case BMAP_LEFT_FILLING | BMAP_RIGHT_FILLING:
		/*
		 * Matches the whole extent.  Delete the entry.
		 */
		xfs_iext_remove(ip, icur, state);
		xfs_iext_prev(ifp, icur);
		ifp->if_nextents--;

		*logflagsp |= XFS_ILOG_CORE;
		if (!cur) {
			*logflagsp |= xfs_ilog_fext(whichfork);
			break;
		}
		if ((error = xfs_btree_delete(cur, &i)))
			return error;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			return -EFSCORRUPTED;
		}
		break;
	case BMAP_LEFT_FILLING:
		/*
		 * Deleting the first part of the extent.
		 */
		got.br_startoff = del_endoff;
		got.br_startblock = del_endblock;
		got.br_blockcount -= del->br_blockcount;
		xfs_iext_update_extent(ip, state, icur, &got);
		if (!cur) {
			*logflagsp |= xfs_ilog_fext(whichfork);
			break;
		}
		error = xfs_bmbt_update(cur, &got);
		if (error)
			return error;
		break;
	case BMAP_RIGHT_FILLING:
		/*
		 * Deleting the last part of the extent.
		 */
		got.br_blockcount -= del->br_blockcount;
		xfs_iext_update_extent(ip, state, icur, &got);
		if (!cur) {
			*logflagsp |= xfs_ilog_fext(whichfork);
			break;
		}
		error = xfs_bmbt_update(cur, &got);
		if (error)
			return error;
		break;
	case 0:
		/*
		 * Deleting the middle of the extent.
		 */

		old = got;

		got.br_blockcount = del->br_startoff - got.br_startoff;
		xfs_iext_update_extent(ip, state, icur, &got);

		new.br_startoff = del_endoff;
		new.br_blockcount = got_endoff - del_endoff;
		new.br_state = got.br_state;
		new.br_startblock = del_endblock;

		*logflagsp |= XFS_ILOG_CORE;
		if (cur) {
			error = xfs_bmbt_update(cur, &got);
			if (error)
				return error;
			error = xfs_btree_increment(cur, 0, &i);
			if (error)
				return error;
			cur->bc_rec.b = new;
			error = xfs_btree_insert(cur, &i);
			if (error && error != -ENOSPC)
				return error;
			/*
			 * If get no-space back from btree insert, it tried a
			 * split, and we have a zero block reservation.  Fix up
			 * our state and return the error.
			 */
			if (error == -ENOSPC) {
				/*
				 * Reset the cursor, don't trust it after any
				 * insert operation.
				 */
				error = xfs_bmbt_lookup_eq(cur, &got, &i);
				if (error)
					return error;
				if (XFS_IS_CORRUPT(mp, i != 1)) {
					xfs_btree_mark_sick(cur);
					return -EFSCORRUPTED;
				}
				/*
				 * Update the btree record back
				 * to the original value.
				 */
				error = xfs_bmbt_update(cur, &old);
				if (error)
					return error;
				/*
				 * Reset the extent record back
				 * to the original value.
				 */
				xfs_iext_update_extent(ip, state, icur, &old);
				*logflagsp = 0;
				return -ENOSPC;
			}
			if (XFS_IS_CORRUPT(mp, i != 1)) {
				xfs_btree_mark_sick(cur);
				return -EFSCORRUPTED;
			}
		} else
			*logflagsp |= xfs_ilog_fext(whichfork);

		ifp->if_nextents++;
		xfs_iext_next(ifp, icur);
		xfs_iext_insert(ip, icur, &new, state);
		break;
	}

	/* remove reverse mapping */
	xfs_rmap_unmap_extent(tp, ip, whichfork, del);

	/*
	 * If we need to, add to list of extents to delete.
	 */
	if (!(bflags & XFS_BMAPI_REMAP)) {
		bool	isrt = xfs_ifork_is_realtime(ip, whichfork);

		if (xfs_is_reflink_inode(ip) && whichfork == XFS_DATA_FORK) {
			xfs_refcount_decrease_extent(tp, isrt, del);
		} else if (isrt && !xfs_has_rtgroups(mp)) {
			error = xfs_bmap_free_rtblocks(tp, del);
		} else {
			unsigned int	efi_flags = 0;

			if ((bflags & XFS_BMAPI_NODISCARD) ||
			    del->br_state == XFS_EXT_UNWRITTEN)
				efi_flags |= XFS_FREE_EXTENT_SKIP_DISCARD;

			/*
			 * Historically, we did not use EFIs to free realtime
			 * extents.  However, when reverse mapping is enabled,
			 * we must maintain the same order of operations as the
			 * data device, which is: Remove the file mapping,
			 * remove the reverse mapping, and then free the
			 * blocks.  Reflink for realtime volumes requires the
			 * same sort of ordering.  Both features rely on
			 * rtgroups, so let's gate rt EFI usage on rtgroups.
			 */
			if (isrt)
				efi_flags |= XFS_FREE_EXTENT_REALTIME;

			error = xfs_free_extent_later(tp, del->br_startblock,
					del->br_blockcount, NULL,
					XFS_AG_RESV_NONE, efi_flags);
		}
		if (error)
			return error;
	}

	/*
	 * Adjust inode # blocks in the file.
	 */
	if (nblks)
		ip->i_nblocks -= nblks;
	/*
	 * Adjust quota data.
	 */
	if (qfield && !(bflags & XFS_BMAPI_REMAP))
		xfs_trans_mod_dquot_byino(tp, ip, qfield, (long)-nblks);

	return 0;
}

/*
 * Unmap (remove) blocks from a file.
 * If nexts is nonzero then the number of extents to remove is limited to
 * that value.  If not all extents in the block range can be removed then
 * *done is set.
 */
static int
__xfs_bunmapi(
	struct xfs_trans	*tp,		/* transaction pointer */
	struct xfs_inode	*ip,		/* incore inode */
	xfs_fileoff_t		start,		/* first file offset deleted */
	xfs_filblks_t		*rlen,		/* i/o: amount remaining */
	uint32_t		flags,		/* misc flags */
	xfs_extnum_t		nexts)		/* number of extents max */
{
	struct xfs_btree_cur	*cur;		/* bmap btree cursor */
	struct xfs_bmbt_irec	del;		/* extent being deleted */
	int			error;		/* error return value */
	xfs_extnum_t		extno;		/* extent number in list */
	struct xfs_bmbt_irec	got;		/* current extent record */
	struct xfs_ifork	*ifp;		/* inode fork pointer */
	int			isrt;		/* freeing in rt area */
	int			logflags;	/* transaction logging flags */
	xfs_extlen_t		mod;		/* rt extent offset */
	struct xfs_mount	*mp = ip->i_mount;
	int			tmp_logflags;	/* partial logging flags */
	int			wasdel;		/* was a delayed alloc extent */
	int			whichfork;	/* data or attribute fork */
	xfs_filblks_t		len = *rlen;	/* length to unmap in file */
	xfs_fileoff_t		end;
	struct xfs_iext_cursor	icur;
	bool			done = false;

	trace_xfs_bunmap(ip, start, len, flags, _RET_IP_);

	whichfork = xfs_bmapi_whichfork(flags);
	ASSERT(whichfork != XFS_COW_FORK);
	ifp = xfs_ifork_ptr(ip, whichfork);
	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp))) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}
	if (xfs_is_shutdown(mp))
		return -EIO;

	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);
	ASSERT(len > 0);
	ASSERT(nexts >= 0);

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	if (xfs_iext_count(ifp) == 0) {
		*rlen = 0;
		return 0;
	}
	XFS_STATS_INC(mp, xs_blk_unmap);
	isrt = xfs_ifork_is_realtime(ip, whichfork);
	end = start + len;

	if (!xfs_iext_lookup_extent_before(ip, ifp, &end, &icur, &got)) {
		*rlen = 0;
		return 0;
	}
	end--;

	logflags = 0;
	if (ifp->if_format == XFS_DINODE_FMT_BTREE) {
		ASSERT(ifp->if_format == XFS_DINODE_FMT_BTREE);
		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);
	} else
		cur = NULL;

	extno = 0;
	while (end != (xfs_fileoff_t)-1 && end >= start &&
	       (nexts == 0 || extno < nexts)) {
		/*
		 * Is the found extent after a hole in which end lives?
		 * Just back up to the previous extent, if so.
		 */
		if (got.br_startoff > end &&
		    !xfs_iext_prev_extent(ifp, &icur, &got)) {
			done = true;
			break;
		}
		/*
		 * Is the last block of this extent before the range
		 * we're supposed to delete?  If so, we're done.
		 */
		end = XFS_FILEOFF_MIN(end,
			got.br_startoff + got.br_blockcount - 1);
		if (end < start)
			break;
		/*
		 * Then deal with the (possibly delayed) allocated space
		 * we found.
		 */
		del = got;
		wasdel = isnullstartblock(del.br_startblock);

		if (got.br_startoff < start) {
			del.br_startoff = start;
			del.br_blockcount -= start - got.br_startoff;
			if (!wasdel)
				del.br_startblock += start - got.br_startoff;
		}
		if (del.br_startoff + del.br_blockcount > end + 1)
			del.br_blockcount = end + 1 - del.br_startoff;

		if (!isrt || (flags & XFS_BMAPI_REMAP))
			goto delete;

		mod = xfs_rtb_to_rtxoff(mp,
				del.br_startblock + del.br_blockcount);
		if (mod) {
			/*
			 * Realtime extent not lined up at the end.
			 * The extent could have been split into written
			 * and unwritten pieces, or we could just be
			 * unmapping part of it.  But we can't really
			 * get rid of part of a realtime extent.
			 */
			if (del.br_state == XFS_EXT_UNWRITTEN) {
				/*
				 * This piece is unwritten, or we're not
				 * using unwritten extents.  Skip over it.
				 */
				ASSERT((flags & XFS_BMAPI_REMAP) || end >= mod);
				end -= mod > del.br_blockcount ?
					del.br_blockcount : mod;
				if (end < got.br_startoff &&
				    !xfs_iext_prev_extent(ifp, &icur, &got)) {
					done = true;
					break;
				}
				continue;
			}
			/*
			 * It's written, turn it unwritten.
			 * This is better than zeroing it.
			 */
			ASSERT(del.br_state == XFS_EXT_NORM);
			ASSERT(tp->t_blk_res > 0);
			/*
			 * If this spans a realtime extent boundary,
			 * chop it back to the start of the one we end at.
			 */
			if (del.br_blockcount > mod) {
				del.br_startoff += del.br_blockcount - mod;
				del.br_startblock += del.br_blockcount - mod;
				del.br_blockcount = mod;
			}
			del.br_state = XFS_EXT_UNWRITTEN;
			error = xfs_bmap_add_extent_unwritten_real(tp, ip,
					whichfork, &icur, &cur, &del,
					&logflags);
			if (error)
				goto error0;
			goto nodelete;
		}

		mod = xfs_rtb_to_rtxoff(mp, del.br_startblock);
		if (mod) {
			xfs_extlen_t off = mp->m_sb.sb_rextsize - mod;

			/*
			 * Realtime extent is lined up at the end but not
			 * at the front.  We'll get rid of full extents if
			 * we can.
			 */
			if (del.br_blockcount > off) {
				del.br_blockcount -= off;
				del.br_startoff += off;
				del.br_startblock += off;
			} else if (del.br_startoff == start &&
				   (del.br_state == XFS_EXT_UNWRITTEN ||
				    tp->t_blk_res == 0)) {
				/*
				 * Can't make it unwritten.  There isn't
				 * a full extent here so just skip it.
				 */
				ASSERT(end >= del.br_blockcount);
				end -= del.br_blockcount;
				if (got.br_startoff > end &&
				    !xfs_iext_prev_extent(ifp, &icur, &got)) {
					done = true;
					break;
				}
				continue;
			} else if (del.br_state == XFS_EXT_UNWRITTEN) {
				struct xfs_bmbt_irec	prev;
				xfs_fileoff_t		unwrite_start;

				/*
				 * This one is already unwritten.
				 * It must have a written left neighbor.
				 * Unwrite the killed part of that one and
				 * try again.
				 */
				if (!xfs_iext_prev_extent(ifp, &icur, &prev))
					ASSERT(0);
				ASSERT(prev.br_state == XFS_EXT_NORM);
				ASSERT(!isnullstartblock(prev.br_startblock));
				ASSERT(del.br_startblock ==
				       prev.br_startblock + prev.br_blockcount);
				unwrite_start = max3(start,
						     del.br_startoff - mod,
						     prev.br_startoff);
				mod = unwrite_start - prev.br_startoff;
				prev.br_startoff = unwrite_start;
				prev.br_startblock += mod;
				prev.br_blockcount -= mod;
				prev.br_state = XFS_EXT_UNWRITTEN;
				error = xfs_bmap_add_extent_unwritten_real(tp,
						ip, whichfork, &icur, &cur,
						&prev, &logflags);
				if (error)
					goto error0;
				goto nodelete;
			} else {
				ASSERT(del.br_state == XFS_EXT_NORM);
				del.br_state = XFS_EXT_UNWRITTEN;
				error = xfs_bmap_add_extent_unwritten_real(tp,
						ip, whichfork, &icur, &cur,
						&del, &logflags);
				if (error)
					goto error0;
				goto nodelete;
			}
		}

delete:
		if (wasdel) {
			xfs_bmap_del_extent_delay(ip, whichfork, &icur, &got,
					&del, flags);
		} else {
			error = xfs_bmap_del_extent_real(ip, tp, &icur, cur,
					&del, &tmp_logflags, whichfork,
					flags);
			logflags |= tmp_logflags;
			if (error)
				goto error0;
		}

		end = del.br_startoff - 1;
nodelete:
		/*
		 * If not done go on to the next (previous) record.
		 */
		if (end != (xfs_fileoff_t)-1 && end >= start) {
			if (!xfs_iext_get_extent(ifp, &icur, &got) ||
			    (got.br_startoff > end &&
			     !xfs_iext_prev_extent(ifp, &icur, &got))) {
				done = true;
				break;
			}
			extno++;
		}
	}
	if (done || end == (xfs_fileoff_t)-1 || end < start)
		*rlen = 0;
	else
		*rlen = end - start + 1;

	/*
	 * Convert to a btree if necessary.
	 */
	if (xfs_bmap_needs_btree(ip, whichfork)) {
		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(tp, ip, &cur, 0,
				&tmp_logflags, whichfork);
		logflags |= tmp_logflags;
	} else {
		error = xfs_bmap_btree_to_extents(tp, ip, cur, &logflags,
			whichfork);
	}

error0:
	/*
	 * Log everything.  Do this after conversion, there's no point in
	 * logging the extent records if we've converted to btree format.
	 */
	if ((logflags & xfs_ilog_fext(whichfork)) &&
	    ifp->if_format != XFS_DINODE_FMT_EXTENTS)
		logflags &= ~xfs_ilog_fext(whichfork);
	else if ((logflags & xfs_ilog_fbroot(whichfork)) &&
		 ifp->if_format != XFS_DINODE_FMT_BTREE)
		logflags &= ~xfs_ilog_fbroot(whichfork);
	/*
	 * Log inode even in the error case, if the transaction
	 * is dirty we'll need to shut down the filesystem.
	 */
	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	if (cur) {
		if (!error)
			cur->bc_bmap.allocated = 0;
		xfs_btree_del_cursor(cur, error);
	}
	return error;
}

/* Unmap a range of a file. */
int
xfs_bunmapi(
	xfs_trans_t		*tp,
	struct xfs_inode	*ip,
	xfs_fileoff_t		bno,
	xfs_filblks_t		len,
	uint32_t		flags,
	xfs_extnum_t		nexts,
	int			*done)
{
	int			error;

	error = __xfs_bunmapi(tp, ip, bno, &len, flags, nexts);
	*done = (len == 0);
	return error;
}

/*
 * Determine whether an extent shift can be accomplished by a merge with the
 * extent that precedes the target hole of the shift.
 */
STATIC bool
xfs_bmse_can_merge(
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*left,	/* preceding extent */
	struct xfs_bmbt_irec	*got,	/* current extent to shift */
	xfs_fileoff_t		shift)	/* shift fsb */
{
	xfs_fileoff_t		startoff;

	startoff = got->br_startoff - shift;

	/*
	 * The extent, once shifted, must be adjacent in-file and on-disk with
	 * the preceding extent.
	 */
	if ((left->br_startoff + left->br_blockcount != startoff) ||
	    (left->br_startblock + left->br_blockcount != got->br_startblock) ||
	    (left->br_state != got->br_state) ||
	    (left->br_blockcount + got->br_blockcount > XFS_MAX_BMBT_EXTLEN) ||
	    !xfs_bmap_same_rtgroup(ip, whichfork, left, got))
		return false;

	return true;
}

/*
 * A bmap extent shift adjusts the file offset of an extent to fill a preceding
 * hole in the file. If an extent shift would result in the extent being fully
 * adjacent to the extent that currently precedes the hole, we can merge with
 * the preceding extent rather than do the shift.
 *
 * This function assumes the caller has verified a shift-by-merge is possible
 * with the provided extents via xfs_bmse_can_merge().
 */
STATIC int
xfs_bmse_merge(
	struct xfs_trans		*tp,
	struct xfs_inode		*ip,
	int				whichfork,
	xfs_fileoff_t			shift,		/* shift fsb */
	struct xfs_iext_cursor		*icur,
	struct xfs_bmbt_irec		*got,		/* extent to shift */
	struct xfs_bmbt_irec		*left,		/* preceding extent */
	struct xfs_btree_cur		*cur,
	int				*logflags)	/* output */
{
	struct xfs_ifork		*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_bmbt_irec		new;
	xfs_filblks_t			blockcount;
	int				error, i;
	struct xfs_mount		*mp = ip->i_mount;

	blockcount = left->br_blockcount + got->br_blockcount;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL | XFS_ILOCK_EXCL);
	ASSERT(xfs_bmse_can_merge(ip, whichfork, left, got, shift));

	new = *left;
	new.br_blockcount = blockcount;

	/*
	 * Update the on-disk extent count, the btree if necessary and log the
	 * inode.
	 */
	ifp->if_nextents--;
	*logflags |= XFS_ILOG_CORE;
	if (!cur) {
		*logflags |= XFS_ILOG_DEXT;
		goto done;
	}

	/* lookup and remove the extent to merge */
	error = xfs_bmbt_lookup_eq(cur, got, &i);
	if (error)
		return error;
	if (XFS_IS_CORRUPT(mp, i != 1)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	error = xfs_btree_delete(cur, &i);
	if (error)
		return error;
	if (XFS_IS_CORRUPT(mp, i != 1)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	/* lookup and update size of the previous extent */
	error = xfs_bmbt_lookup_eq(cur, left, &i);
	if (error)
		return error;
	if (XFS_IS_CORRUPT(mp, i != 1)) {
		xfs_btree_mark_sick(cur);
		return -EFSCORRUPTED;
	}

	error = xfs_bmbt_update(cur, &new);
	if (error)
		return error;

	/* change to extent format if required after extent removal */
	error = xfs_bmap_btree_to_extents(tp, ip, cur, logflags, whichfork);
	if (error)
		return error;

done:
	xfs_iext_remove(ip, icur, 0);
	xfs_iext_prev(ifp, icur);
	xfs_iext_update_extent(ip, xfs_bmap_fork_to_state(whichfork), icur,
			&new);

	/* update reverse mapping. rmap functions merge the rmaps for us */
	xfs_rmap_unmap_extent(tp, ip, whichfork, got);
	memcpy(&new, got, sizeof(new));
	new.br_startoff = left->br_startoff + left->br_blockcount;
	xfs_rmap_map_extent(tp, ip, whichfork, &new);
	return 0;
}

static int
xfs_bmap_shift_update_extent(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_iext_cursor	*icur,
	struct xfs_bmbt_irec	*got,
	struct xfs_btree_cur	*cur,
	int			*logflags,
	xfs_fileoff_t		startoff)
{
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_bmbt_irec	prev = *got;
	int			error, i;

	*logflags |= XFS_ILOG_CORE;

	got->br_startoff = startoff;

	if (cur) {
		error = xfs_bmbt_lookup_eq(cur, &prev, &i);
		if (error)
			return error;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			return -EFSCORRUPTED;
		}

		error = xfs_bmbt_update(cur, got);
		if (error)
			return error;
	} else {
		*logflags |= XFS_ILOG_DEXT;
	}

	xfs_iext_update_extent(ip, xfs_bmap_fork_to_state(whichfork), icur,
			got);

	/* update reverse mapping */
	xfs_rmap_unmap_extent(tp, ip, whichfork, &prev);
	xfs_rmap_map_extent(tp, ip, whichfork, got);
	return 0;
}

int
xfs_bmap_collapse_extents(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_fileoff_t		*next_fsb,
	xfs_fileoff_t		offset_shift_fsb,
	bool			*done)
{
	int			whichfork = XFS_DATA_FORK;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_btree_cur	*cur = NULL;
	struct xfs_bmbt_irec	got, prev;
	struct xfs_iext_cursor	icur;
	xfs_fileoff_t		new_startoff;
	int			error = 0;
	int			logflags = 0;

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL | XFS_ILOCK_EXCL);

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	if (ifp->if_format == XFS_DINODE_FMT_BTREE)
		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);

	if (!xfs_iext_lookup_extent(ip, ifp, *next_fsb, &icur, &got)) {
		*done = true;
		goto del_cursor;
	}
	if (XFS_IS_CORRUPT(mp, isnullstartblock(got.br_startblock))) {
		xfs_bmap_mark_sick(ip, whichfork);
		error = -EFSCORRUPTED;
		goto del_cursor;
	}

	new_startoff = got.br_startoff - offset_shift_fsb;
	if (xfs_iext_peek_prev_extent(ifp, &icur, &prev)) {
		if (new_startoff < prev.br_startoff + prev.br_blockcount) {
			error = -EINVAL;
			goto del_cursor;
		}

		if (xfs_bmse_can_merge(ip, whichfork, &prev, &got,
				offset_shift_fsb)) {
			error = xfs_bmse_merge(tp, ip, whichfork,
					offset_shift_fsb, &icur, &got, &prev,
					cur, &logflags);
			if (error)
				goto del_cursor;
			goto done;
		}
	} else {
		if (got.br_startoff < offset_shift_fsb) {
			error = -EINVAL;
			goto del_cursor;
		}
	}

	error = xfs_bmap_shift_update_extent(tp, ip, whichfork, &icur, &got,
			cur, &logflags, new_startoff);
	if (error)
		goto del_cursor;

done:
	if (!xfs_iext_next_extent(ifp, &icur, &got)) {
		*done = true;
		goto del_cursor;
	}

	*next_fsb = got.br_startoff;
del_cursor:
	if (cur)
		xfs_btree_del_cursor(cur, error);
	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	return error;
}

/* Make sure we won't be right-shifting an extent past the maximum bound. */
int
xfs_bmap_can_insert_extents(
	struct xfs_inode	*ip,
	xfs_fileoff_t		off,
	xfs_fileoff_t		shift)
{
	struct xfs_bmbt_irec	got;
	int			is_empty;
	int			error = 0;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL);

	if (xfs_is_shutdown(ip->i_mount))
		return -EIO;

	xfs_ilock(ip, XFS_ILOCK_EXCL);
	error = xfs_bmap_last_extent(NULL, ip, XFS_DATA_FORK, &got, &is_empty);
	if (!error && !is_empty && got.br_startoff >= off &&
	    ((got.br_startoff + shift) & BMBT_STARTOFF_MASK) < got.br_startoff)
		error = -EINVAL;
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	return error;
}

int
xfs_bmap_insert_extents(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_fileoff_t		*next_fsb,
	xfs_fileoff_t		offset_shift_fsb,
	bool			*done,
	xfs_fileoff_t		stop_fsb)
{
	int			whichfork = XFS_DATA_FORK;
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_btree_cur	*cur = NULL;
	struct xfs_bmbt_irec	got, next;
	struct xfs_iext_cursor	icur;
	xfs_fileoff_t		new_startoff;
	int			error = 0;
	int			logflags = 0;

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	xfs_assert_ilocked(ip, XFS_IOLOCK_EXCL | XFS_ILOCK_EXCL);

	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	if (ifp->if_format == XFS_DINODE_FMT_BTREE)
		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);

	if (*next_fsb == NULLFSBLOCK) {
		xfs_iext_last(ifp, &icur);
		if (!xfs_iext_get_extent(ifp, &icur, &got) ||
		    stop_fsb > got.br_startoff) {
			*done = true;
			goto del_cursor;
		}
	} else {
		if (!xfs_iext_lookup_extent(ip, ifp, *next_fsb, &icur, &got)) {
			*done = true;
			goto del_cursor;
		}
	}
	if (XFS_IS_CORRUPT(mp, isnullstartblock(got.br_startblock))) {
		xfs_bmap_mark_sick(ip, whichfork);
		error = -EFSCORRUPTED;
		goto del_cursor;
	}

	if (XFS_IS_CORRUPT(mp, stop_fsb > got.br_startoff)) {
		xfs_bmap_mark_sick(ip, whichfork);
		error = -EFSCORRUPTED;
		goto del_cursor;
	}

	new_startoff = got.br_startoff + offset_shift_fsb;
	if (xfs_iext_peek_next_extent(ifp, &icur, &next)) {
		if (new_startoff + got.br_blockcount > next.br_startoff) {
			error = -EINVAL;
			goto del_cursor;
		}

		/*
		 * Unlike a left shift (which involves a hole punch), a right
		 * shift does not modify extent neighbors in any way.  We should
		 * never find mergeable extents in this scenario.  Check anyways
		 * and warn if we encounter two extents that could be one.
		 */
		if (xfs_bmse_can_merge(ip, whichfork, &got, &next,
				offset_shift_fsb))
			WARN_ON_ONCE(1);
	}

	error = xfs_bmap_shift_update_extent(tp, ip, whichfork, &icur, &got,
			cur, &logflags, new_startoff);
	if (error)
		goto del_cursor;

	if (!xfs_iext_prev_extent(ifp, &icur, &got) ||
	    stop_fsb >= got.br_startoff + got.br_blockcount) {
		*done = true;
		goto del_cursor;
	}

	*next_fsb = got.br_startoff;
del_cursor:
	if (cur)
		xfs_btree_del_cursor(cur, error);
	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	return error;
}

/*
 * Splits an extent into two extents at split_fsb block such that it is the
 * first block of the current_ext. @ext is a target extent to be split.
 * @split_fsb is a block where the extents is split.  If split_fsb lies in a
 * hole or the first block of extents, just return 0.
 */
int
xfs_bmap_split_extent(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	xfs_fileoff_t		split_fsb)
{
	int				whichfork = XFS_DATA_FORK;
	struct xfs_ifork		*ifp = xfs_ifork_ptr(ip, whichfork);
	struct xfs_btree_cur		*cur = NULL;
	struct xfs_bmbt_irec		got;
	struct xfs_bmbt_irec		new; /* split extent */
	struct xfs_mount		*mp = ip->i_mount;
	xfs_fsblock_t			gotblkcnt; /* new block count for got */
	struct xfs_iext_cursor		icur;
	int				error = 0;
	int				logflags = 0;
	int				i = 0;

	if (XFS_IS_CORRUPT(mp, !xfs_ifork_has_extents(ifp)) ||
	    XFS_TEST_ERROR(false, mp, XFS_ERRTAG_BMAPIFORMAT)) {
		xfs_bmap_mark_sick(ip, whichfork);
		return -EFSCORRUPTED;
	}

	if (xfs_is_shutdown(mp))
		return -EIO;

	/* Read in all the extents */
	error = xfs_iread_extents(tp, ip, whichfork);
	if (error)
		return error;

	/*
	 * If there are not extents, or split_fsb lies in a hole we are done.
	 */
	if (!xfs_iext_lookup_extent(ip, ifp, split_fsb, &icur, &got) ||
	    got.br_startoff >= split_fsb)
		return 0;

	gotblkcnt = split_fsb - got.br_startoff;
	new.br_startoff = split_fsb;
	new.br_startblock = got.br_startblock + gotblkcnt;
	new.br_blockcount = got.br_blockcount - gotblkcnt;
	new.br_state = got.br_state;

	if (ifp->if_format == XFS_DINODE_FMT_BTREE) {
		cur = xfs_bmbt_init_cursor(mp, tp, ip, whichfork);
		error = xfs_bmbt_lookup_eq(cur, &got, &i);
		if (error)
			goto del_cursor;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto del_cursor;
		}
	}

	got.br_blockcount = gotblkcnt;
	xfs_iext_update_extent(ip, xfs_bmap_fork_to_state(whichfork), &icur,
			&got);

	logflags = XFS_ILOG_CORE;
	if (cur) {
		error = xfs_bmbt_update(cur, &got);
		if (error)
			goto del_cursor;
	} else
		logflags |= XFS_ILOG_DEXT;

	/* Add new extent */
	xfs_iext_next(ifp, &icur);
	xfs_iext_insert(ip, &icur, &new, 0);
	ifp->if_nextents++;

	if (cur) {
		error = xfs_bmbt_lookup_eq(cur, &new, &i);
		if (error)
			goto del_cursor;
		if (XFS_IS_CORRUPT(mp, i != 0)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto del_cursor;
		}
		error = xfs_btree_insert(cur, &i);
		if (error)
			goto del_cursor;
		if (XFS_IS_CORRUPT(mp, i != 1)) {
			xfs_btree_mark_sick(cur);
			error = -EFSCORRUPTED;
			goto del_cursor;
		}
	}

	/*
	 * Convert to a btree if necessary.
	 */
	if (xfs_bmap_needs_btree(ip, whichfork)) {
		int tmp_logflags; /* partial log flag return val */

		ASSERT(cur == NULL);
		error = xfs_bmap_extents_to_btree(tp, ip, &cur, 0,
				&tmp_logflags, whichfork);
		logflags |= tmp_logflags;
	}

del_cursor:
	if (cur) {
		cur->bc_bmap.allocated = 0;
		xfs_btree_del_cursor(cur, error);
	}

	if (logflags)
		xfs_trans_log_inode(tp, ip, logflags);
	return error;
}

/* Record a bmap intent. */
static inline void
__xfs_bmap_add(
	struct xfs_trans		*tp,
	enum xfs_bmap_intent_type	type,
	struct xfs_inode		*ip,
	int				whichfork,
	struct xfs_bmbt_irec		*bmap)
{
	struct xfs_bmap_intent		*bi;

	if ((whichfork != XFS_DATA_FORK && whichfork != XFS_ATTR_FORK) ||
	    bmap->br_startblock == HOLESTARTBLOCK ||
	    bmap->br_startblock == DELAYSTARTBLOCK)
		return;

	bi = kmem_cache_alloc(xfs_bmap_intent_cache, GFP_KERNEL | __GFP_NOFAIL);
	INIT_LIST_HEAD(&bi->bi_list);
	bi->bi_type = type;
	bi->bi_owner = ip;
	bi->bi_whichfork = whichfork;
	bi->bi_bmap = *bmap;

	xfs_bmap_defer_add(tp, bi);
}

/* Map an extent into a file. */
void
xfs_bmap_map_extent(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*PREV)
{
	__xfs_bmap_add(tp, XFS_BMAP_MAP, ip, whichfork, PREV);
}

/* Unmap an extent out of a file. */
void
xfs_bmap_unmap_extent(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*PREV)
{
	__xfs_bmap_add(tp, XFS_BMAP_UNMAP, ip, whichfork, PREV);
}

/*
 * Process one of the deferred bmap operations.  We pass back the
 * btree cursor to maintain our lock on the bmapbt between calls.
 */
int
xfs_bmap_finish_one(
	struct xfs_trans		*tp,
	struct xfs_bmap_intent		*bi)
{
	struct xfs_bmbt_irec		*bmap = &bi->bi_bmap;
	int				error = 0;
	int				flags = 0;

	if (bi->bi_whichfork == XFS_ATTR_FORK)
		flags |= XFS_BMAPI_ATTRFORK;

	ASSERT(tp->t_highest_agno == NULLAGNUMBER);

	trace_xfs_bmap_deferred(bi);

	if (XFS_TEST_ERROR(false, tp->t_mountp, XFS_ERRTAG_BMAP_FINISH_ONE))
		return -EIO;

	switch (bi->bi_type) {
	case XFS_BMAP_MAP:
		if (bi->bi_bmap.br_state == XFS_EXT_UNWRITTEN)
			flags |= XFS_BMAPI_PREALLOC;
		error = xfs_bmapi_remap(tp, bi->bi_owner, bmap->br_startoff,
				bmap->br_blockcount, bmap->br_startblock,
				flags);
		bmap->br_blockcount = 0;
		break;
	case XFS_BMAP_UNMAP:
		error = __xfs_bunmapi(tp, bi->bi_owner, bmap->br_startoff,
				&bmap->br_blockcount, flags | XFS_BMAPI_REMAP,
				1);
		break;
	default:
		ASSERT(0);
		xfs_bmap_mark_sick(bi->bi_owner, bi->bi_whichfork);
		error = -EFSCORRUPTED;
	}

	return error;
}

/* Check that an extent does not have invalid flags or bad ranges. */
xfs_failaddr_t
xfs_bmap_validate_extent_raw(
	struct xfs_mount	*mp,
	bool			rtfile,
	int			whichfork,
	struct xfs_bmbt_irec	*irec)
{
	if (!xfs_verify_fileext(mp, irec->br_startoff, irec->br_blockcount))
		return __this_address;

	if (rtfile && whichfork == XFS_DATA_FORK) {
		if (!xfs_verify_rtbext(mp, irec->br_startblock,
					   irec->br_blockcount))
			return __this_address;
	} else {
		if (!xfs_verify_fsbext(mp, irec->br_startblock,
					   irec->br_blockcount))
			return __this_address;
	}
	if (irec->br_state != XFS_EXT_NORM && whichfork != XFS_DATA_FORK)
		return __this_address;
	return NULL;
}

int __init
xfs_bmap_intent_init_cache(void)
{
	xfs_bmap_intent_cache = kmem_cache_create("xfs_bmap_intent",
			sizeof(struct xfs_bmap_intent),
			0, 0, NULL);

	return xfs_bmap_intent_cache != NULL ? 0 : -ENOMEM;
}

void
xfs_bmap_intent_destroy_cache(void)
{
	kmem_cache_destroy(xfs_bmap_intent_cache);
	xfs_bmap_intent_cache = NULL;
}

/* Check that an inode's extent does not have invalid flags or bad ranges. */
xfs_failaddr_t
xfs_bmap_validate_extent(
	struct xfs_inode	*ip,
	int			whichfork,
	struct xfs_bmbt_irec	*irec)
{
	return xfs_bmap_validate_extent_raw(ip->i_mount,
			XFS_IS_REALTIME_INODE(ip), whichfork, irec);
}

/*
 * Used in xfs_itruncate_extents().  This is the maximum number of extents
 * freed from a file in a single transaction.
 */
#define	XFS_ITRUNC_MAX_EXTENTS	2

/*
 * Unmap every extent in part of an inode's fork.  We don't do any higher level
 * invalidation work at all.
 */
int
xfs_bunmapi_range(
	struct xfs_trans	**tpp,
	struct xfs_inode	*ip,
	uint32_t		flags,
	xfs_fileoff_t		startoff,
	xfs_fileoff_t		endoff)
{
	xfs_filblks_t		unmap_len = endoff - startoff + 1;
	int			error = 0;

	xfs_assert_ilocked(ip, XFS_ILOCK_EXCL);

	while (unmap_len > 0) {
		ASSERT((*tpp)->t_highest_agno == NULLAGNUMBER);
		error = __xfs_bunmapi(*tpp, ip, startoff, &unmap_len, flags,
				XFS_ITRUNC_MAX_EXTENTS);
		if (error)
			goto out;

		/* free the just unmapped extents */
		error = xfs_defer_finish(tpp);
		if (error)
			goto out;
		cond_resched();
	}
out:
	return error;
}

struct xfs_bmap_query_range {
	xfs_bmap_query_range_fn	fn;
	void			*priv;
};

/* Format btree record and pass to our callback. */
STATIC int
xfs_bmap_query_range_helper(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_rec	*rec,
	void				*priv)
{
	struct xfs_bmap_query_range	*query = priv;
	struct xfs_bmbt_irec		irec;
	xfs_failaddr_t			fa;

	xfs_bmbt_disk_get_all(&rec->bmbt, &irec);
	fa = xfs_bmap_validate_extent(cur->bc_ino.ip, cur->bc_ino.whichfork,
			&irec);
	if (fa) {
		xfs_btree_mark_sick(cur);
		return xfs_bmap_complain_bad_rec(cur->bc_ino.ip,
				cur->bc_ino.whichfork, fa, &irec);
	}

	return query->fn(cur, &irec, query->priv);
}

/* Find all bmaps. */
int
xfs_bmap_query_all(
	struct xfs_btree_cur		*cur,
	xfs_bmap_query_range_fn		fn,
	void				*priv)
{
	struct xfs_bmap_query_range	query = {
		.priv			= priv,
		.fn			= fn,
	};

	return xfs_btree_query_all(cur, xfs_bmap_query_range_helper, &query);
}

/* Helper function to extract extent size hint from inode */
xfs_extlen_t
xfs_get_extsz_hint(
	struct xfs_inode	*ip)
{
	/*
	 * No point in aligning allocations if we need to COW to actually
	 * write to them.
	 */
	if (!xfs_is_always_cow_inode(ip) &&
	    (ip->i_diflags & XFS_DIFLAG_EXTSIZE) && ip->i_extsize)
		return ip->i_extsize;
	if (XFS_IS_REALTIME_INODE(ip) &&
	    ip->i_mount->m_sb.sb_rextsize > 1)
		return ip->i_mount->m_sb.sb_rextsize;
	return 0;
}

/*
 * Helper function to extract CoW extent size hint from inode.
 * Between the extent size hint and the CoW extent size hint, we
 * return the greater of the two.  If the value is zero (automatic),
 * use the default size.
 */
xfs_extlen_t
xfs_get_cowextsz_hint(
	struct xfs_inode	*ip)
{
	xfs_extlen_t		a, b;

	a = 0;
	if (ip->i_diflags2 & XFS_DIFLAG2_COWEXTSIZE)
		a = ip->i_cowextsize;
	if (XFS_IS_REALTIME_INODE(ip)) {
		b = 0;
		if (ip->i_diflags & XFS_DIFLAG_EXTSIZE)
			b = ip->i_extsize;
	} else {
		b = xfs_get_extsz_hint(ip);
	}

	a = max(a, b);
	if (a == 0)
		return XFS_DEFAULT_COWEXTSZ_HINT;
	return a;
}
