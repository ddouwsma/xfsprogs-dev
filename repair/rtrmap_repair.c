// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2019-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <libxfs.h>
#include "btree.h"
#include "err_protos.h"
#include "libxlog.h"
#include "incore.h"
#include "globals.h"
#include "dinode.h"
#include "slab.h"
#include "rmap.h"
#include "bulkload.h"

/* Ported routines from fs/xfs/scrub/rtrmap_repair.c */

/*
 * Realtime Reverse Mapping (RTRMAPBT) Repair
 * ==========================================
 *
 * Gather all the rmap records for the inode and fork we're fixing, reset the
 * incore fork, then recreate the btree.
 */
struct xrep_rtrmap {
	struct xfs_btree_cur	*btree_cursor;

	/* New fork. */
	struct bulkload		new_fork_info;
	struct xfs_btree_bload	rtrmap_bload;

	struct repair_ctx	*sc;
	struct xfs_rtgroup	*rtg;

	/* Estimated free space after building all rt btrees */
	xfs_filblks_t		est_fdblocks;
};

/* Retrieve rtrmapbt data for bulk load. */
STATIC int
xrep_rtrmap_get_records(
	struct xfs_btree_cur	*cur,
	unsigned int		idx,
	struct xfs_btree_block	*block,
	unsigned int		nr_wanted,
	void			*priv)
{
	struct xrep_rtrmap	*rr = priv;
	union xfs_btree_rec	*block_rec;
	unsigned int		loaded;
	int			ret;

	for (loaded = 0; loaded < nr_wanted; loaded++, idx++) {
		ret = rmap_get_mem_rec(rr->btree_cursor, &cur->bc_rec.r);
		if (ret < 0)
			return ret;
		if (ret == 0)
			do_error(
 _("ran out of records while rebuilding rt rmap btree\n"));

		block_rec = libxfs_btree_rec_addr(cur, idx, block);
		cur->bc_ops->init_rec_from_cur(cur, block_rec);
	}

	return loaded;
}

/* Feed one of the new btree blocks to the bulk loader. */
STATIC int
xrep_rtrmap_claim_block(
	struct xfs_btree_cur	*cur,
	union xfs_btree_ptr	*ptr,
	void			*priv)
{
	struct xrep_rtrmap	*rr = priv;

	return bulkload_claim_block(cur, &rr->new_fork_info, ptr);
}

/* Figure out how much space we need to create the incore btree root block. */
STATIC size_t
xrep_rtrmap_iroot_size(
	struct xfs_btree_cur	*cur,
	unsigned int		level,
	unsigned int		nr_this_level,
	void			*priv)
{
	return xfs_rtrmap_broot_space_calc(cur->bc_mp, level, nr_this_level);
}

/* Reserve new btree blocks and bulk load all the rtrmap records. */
STATIC int
xrep_rtrmap_btree_load(
	struct xrep_rtrmap	*rr,
	struct xfs_btree_cur	*rtrmap_cur)
{
	struct repair_ctx	*sc = rr->sc;
	int			error;

	rr->rtrmap_bload.get_records = xrep_rtrmap_get_records;
	rr->rtrmap_bload.claim_block = xrep_rtrmap_claim_block;
	rr->rtrmap_bload.iroot_size = xrep_rtrmap_iroot_size;
	bulkload_estimate_inode_slack(sc->mp, &rr->rtrmap_bload,
			rr->est_fdblocks);

	/* Compute how many blocks we'll need. */
	error = -libxfs_btree_bload_compute_geometry(rtrmap_cur,
			&rr->rtrmap_bload,
			rmap_record_count(sc->mp, true, rtg_rgno(rr->rtg)));
	if (error)
		return error;

	/*
	 * Guess how many blocks we're going to need to rebuild an entire rtrmap
	 * from the number of extents we found, and pump up our transaction to
	 * have sufficient block reservation.
	 */
	error = -libxfs_trans_reserve_more(sc->tp, rr->rtrmap_bload.nr_blocks,
			0);
	if (error)
		return error;

	/*
	 * Reserve the space we'll need for the new btree.  Drop the cursor
	 * while we do this because that can roll the transaction and cursors
	 * can't handle that.
	 */
	error = bulkload_alloc_file_blocks(&rr->new_fork_info,
			rr->rtrmap_bload.nr_blocks);
	if (error)
		return error;

	/* Add all observed rtrmap records. */
	error = rmap_init_mem_cursor(rr->sc->mp, sc->tp, true,
			rtg_rgno(rr->rtg), &rr->btree_cursor);
	if (error)
		return error;
	error = -libxfs_btree_bload(rtrmap_cur, &rr->rtrmap_bload, rr);
	libxfs_btree_del_cursor(rr->btree_cursor, error);
	return error;
}

static void
rtgroup_update_counters(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_inode	*rmapip = rtg->rtg_inodes[XFS_RTGI_RMAP];
	struct xfs_mount	*mp = rtg_mount(rtg);
	uint64_t		end =
		xfs_rtbxlen_to_blen(mp, rtg->rtg_extents);
	xfs_agblock_t		gbno = 0;
	uint64_t		used = 0;

	do {
		int		bstate;
		xfs_extlen_t	blen;

		bstate = get_bmap_ext(rtg_rgno(rtg), gbno, end, &blen, true);
		switch (bstate) {
		case XR_E_INUSE:
		case XR_E_INUSE_FS:
			used += blen;
			break;
		default:
			break;
		}

		gbno += blen;
	} while (gbno < end);

	rmapip->i_used_blocks = used;
}

/* Update the inode counters. */
STATIC int
xrep_rtrmap_reset_counters(
	struct xrep_rtrmap	*rr)
{
	struct repair_ctx	*sc = rr->sc;

	/*
	 * Update the inode block counts to reflect the btree we just
	 * generated.
	 */
	sc->ip->i_nblocks = rr->new_fork_info.ifake.if_blocks;
	if (xfs_has_zoned(sc->mp))
		rtgroup_update_counters(rr->rtg);
	libxfs_trans_log_inode(sc->tp, sc->ip, XFS_ILOG_CORE);

	/* Quotas don't exist so we're done. */
	return 0;
}

/*
 * Use the collected rmap information to stage a new rt rmap btree.  If this is
 * successful we'll return with the new btree root information logged to the
 * repair transaction but not yet committed.
 */
static int
xrep_rtrmap_build_new_tree(
	struct xrep_rtrmap	*rr)
{
	struct xfs_owner_info	oinfo;
	struct xfs_btree_cur	*cur;
	struct repair_ctx	*sc = rr->sc;
	struct xbtree_ifakeroot	*ifake = &rr->new_fork_info.ifake;
	int			error;

	/*
	 * Prepare to construct the new fork by initializing the new btree
	 * structure and creating a fake ifork in the ifakeroot structure.
	 */
	libxfs_rmap_ino_bmbt_owner(&oinfo, sc->ip->i_ino, XFS_DATA_FORK);
	bulkload_init_inode(&rr->new_fork_info, sc, XFS_DATA_FORK, &oinfo);
	cur = libxfs_rtrmapbt_init_cursor(NULL, rr->rtg);
	libxfs_btree_stage_ifakeroot(cur, ifake);

	/*
	 * Figure out the size and format of the new fork, then fill it with
	 * all the rtrmap records we've found.  Join the inode to the
	 * transaction so that we can roll the transaction while holding the
	 * inode locked.
	 */
	libxfs_trans_ijoin(sc->tp, sc->ip, 0);
	ifake->if_fork->if_format = XFS_DINODE_FMT_META_BTREE;
	error = xrep_rtrmap_btree_load(rr, cur);
	if (error)
		goto err_cur;

	/*
	 * Install the new fork in the inode.  After this point the old mapping
	 * data are no longer accessible and the new tree is live.  We delete
	 * the cursor immediately after committing the staged root because the
	 * staged fork might be in extents format.
	 */
	libxfs_rtrmapbt_commit_staged_btree(cur, sc->tp);
	libxfs_btree_del_cursor(cur, 0);

	/* Reset the inode counters now that we've changed the fork. */
	error = xrep_rtrmap_reset_counters(rr);
	if (error)
		goto err_newbt;

	/* Dispose of any unused blocks and the accounting infomation. */
	error = bulkload_commit(&rr->new_fork_info);
	if (error)
		return error;

	return -libxfs_trans_roll_inode(&sc->tp, sc->ip);
err_cur:
	if (cur)
		libxfs_btree_del_cursor(cur, error);
err_newbt:
	bulkload_cancel(&rr->new_fork_info);
	return error;
}

/* Store the realtime reverse-mappings in the rtrmapbt. */
void
populate_rtgroup_rmapbt(
	struct xfs_rtgroup	*rtg,
	xfs_filblks_t		est_fdblocks)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	struct xfs_inode	*ip = rtg_rmap(rtg);
	struct repair_ctx	sc = {
		.mp		= mp,
		.ip		= ip,
	};
	struct xrep_rtrmap	rr = {
		.sc		= &sc,
		.rtg		= rtg,
		.est_fdblocks	= est_fdblocks,
	};
	int			error;

	if (!xfs_has_rtrmapbt(mp))
		return;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate, 0, 0, 0,
			&sc.tp);
	if (error)
		goto out;

	error = xrep_rtrmap_build_new_tree(&rr);
	if (error) {
		libxfs_trans_cancel(sc.tp);
		goto out;
	}

	error = -libxfs_trans_commit(sc.tp);
out:
	if (error)
		do_error(
 _("rtgroup %u rmap btree could not be rebuilt, error %d\n"),
			rtg_rgno(rtg), error);
}
