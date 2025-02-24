// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "threads.h"
#include "prefetch.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "bmap.h"
#include "versions.h"
#include "dir2.h"
#include "progress.h"
#include "slab.h"
#include "rmap.h"

bool collect_rmaps;

static inline void
quotino_check_one(
	struct xfs_mount	*mp,
	xfs_dqtype_t		type)
{
	struct ino_tree_node	*irec;
	xfs_ino_t		ino;

	if (!has_quota_inode(type))
		return;

	ino = get_quota_inode(type);
	if (!libxfs_verify_ino(mp, ino))
		goto bad;

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, ino),
			XFS_INO_TO_AGINO(mp, ino));
	if (!irec)
		goto bad;

	if (is_inode_free(irec, ino - irec->ino_startnum))
		goto bad;

	return;

bad:
	lose_quota_inode(type);
}

/*
 * null out quota inode fields in sb if they point to non-existent inodes.
 * this isn't as redundant as it looks since it's possible that the sb field
 * might be set but the imap and inode(s) agree that the inode is
 * free in which case they'd never be cleared so the fields wouldn't
 * be cleared by process_dinode().
 */
static void
quotino_check(
	struct xfs_mount	*mp)
{
	quotino_check_one(mp, XFS_DQTYPE_USER);
	quotino_check_one(mp, XFS_DQTYPE_GROUP);
	quotino_check_one(mp, XFS_DQTYPE_PROJ);
}

static void
quota_sb_check(xfs_mount_t *mp)
{
	if (xfs_has_metadir(mp)) {
		/*
		 * Metadir filesystems try to preserve the quota accounting
		 * and enforcement flags so that users don't have to remember
		 * to supply quota mount options.  Phase 1 discovered the
		 * QUOTABIT flag (fs_quotas) and phase 2 discovered the quota
		 * inodes from the metadir for us.
		 *
		 * If QUOTABIT wasn't set but we found quota inodes, signal
		 * phase 5 to add the feature bit for us.  We do not ever
		 * downgrade the filesystem.
		 */
		if (!fs_quotas &&
		    (has_quota_inode(XFS_DQTYPE_USER) ||
		     has_quota_inode(XFS_DQTYPE_GROUP) ||
		     has_quota_inode(XFS_DQTYPE_PROJ)))
			fs_quotas = 1;
		return;
	}

	/*
	 * if the sb says we have quotas and we lost both,
	 * signal a superblock downgrade.  that will cause
	 * the quota flags to get zeroed.  (if we only lost
	 * one quota inode, do nothing and complain later.)
	 *
	 * if the sb says we have quotas but we didn't start out
	 * with any quota inodes, signal a superblock downgrade.
	 *
	 * The sb downgrades are so that older systems can mount
	 * the filesystem.
	 *
	 * if the sb says we don't have quotas but it looks like
	 * we do have quota inodes, then signal a superblock upgrade.
	 *
	 * if the sb says we don't have quotas and we have no
	 * quota inodes, then leave will enough alone.
	 */

	if (fs_quotas &&
	    !has_quota_inode(XFS_DQTYPE_USER) &&
	    !has_quota_inode(XFS_DQTYPE_GROUP) &&
	    !has_quota_inode(XFS_DQTYPE_PROJ))  {
		lost_quotas = 1;
		fs_quotas = 0;
	} else if (libxfs_verify_ino(mp, get_quota_inode(XFS_DQTYPE_USER)) &&
		   libxfs_verify_ino(mp, get_quota_inode(XFS_DQTYPE_GROUP)) &&
		   libxfs_verify_ino(mp, get_quota_inode(XFS_DQTYPE_PROJ))) {
		fs_quotas = 1;
	}
}


static void
process_ag_func(
	struct workqueue	*wq,
	xfs_agnumber_t 		agno,
	void			*arg)
{
	wait_for_inode_prefetch(arg);
	do_log(_("        - agno = %d\n"), agno);
	process_aginodes(wq->wq_ctx, arg, agno, 0, 1, 0);
	blkmap_free_final();
	cleanup_inode_prefetch(arg);

	/*
	 * now recycle the per-AG duplicate extent records
	 */
	release_dup_extent_tree(agno);
}

static void
process_ags(
	xfs_mount_t		*mp)
{
	do_inode_prefetch(mp, ag_stride, process_ag_func, true, false);
}

static void
check_rmap_btrees(
	struct workqueue*wq,
	xfs_agnumber_t	agno,
	void		*arg)
{
	rmap_add_fixed_ag_rec(wq->wq_ctx, agno);
	rmaps_verify_btree(wq->wq_ctx, agno);
}

static void
check_rtrmap_btrees(
	struct workqueue *wq,
	xfs_agnumber_t	agno,
	void		*arg)
{
	rmap_add_fixed_rtgroup_rec(wq->wq_ctx, agno);
	rtrmaps_verify_btree(wq->wq_ctx, agno);
}

static void
compute_ag_refcounts(
	struct workqueue*wq,
	xfs_agnumber_t	agno,
	void		*arg)
{
	int		error;

	error = compute_refcounts(wq->wq_ctx, false, agno);
	if (error)
		do_error(
_("%s while computing reference count records.\n"),
			 strerror(error));
}

static void
compute_rt_refcounts(
	struct workqueue*wq,
	xfs_agnumber_t	rgno,
	void		*arg)
{
	int		error;

	error = compute_refcounts(wq->wq_ctx, true, rgno);
	if (error)
		do_error(
_("%s while computing realtime reference count records.\n"),
			 strerror(error));
}

static void
process_inode_reflink_flags(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	int			error;

	error = fix_inode_reflink_flags(wq->wq_ctx, agno);
	if (error)
		do_error(
_("%s while fixing inode reflink flags.\n"),
			 strerror(-error));
}

static void
check_refcount_btrees(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	check_refcounts(wq->wq_ctx, agno);
}

static void
check_rt_refcount_btrees(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	check_rtrefcounts(wq->wq_ctx, agno);
}

static void
process_rmap_data(
	struct xfs_mount	*mp)
{
	struct workqueue	wq;
	xfs_agnumber_t		i;

	if (!rmap_needs_work(mp))
		return;

	create_work_queue(&wq, mp, platform_nproc());
	for (i = 0; i < mp->m_sb.sb_agcount; i++)
		queue_work(&wq, check_rmap_btrees, i, NULL);
	if (xfs_has_rtrmapbt(mp)) {
		for (i = 0; i < mp->m_sb.sb_rgcount; i++)
			queue_work(&wq, check_rtrmap_btrees, i, NULL);
	}
	destroy_work_queue(&wq);

	if (!xfs_has_reflink(mp))
		return;

	create_work_queue(&wq, mp, platform_nproc());
	for (i = 0; i < mp->m_sb.sb_agcount; i++)
		queue_work(&wq, compute_ag_refcounts, i, NULL);
	if (xfs_has_rtreflink(mp)) {
		for (i = 0; i < mp->m_sb.sb_rgcount; i++)
			queue_work(&wq, compute_rt_refcounts, i, NULL);
	}
	destroy_work_queue(&wq);

	create_work_queue(&wq, mp, platform_nproc());
	for (i = 0; i < mp->m_sb.sb_agcount; i++) {
		queue_work(&wq, process_inode_reflink_flags, i, NULL);
		queue_work(&wq, check_refcount_btrees, i, NULL);
	}
	if (xfs_has_rtreflink(mp)) {
		for (i = 0; i < mp->m_sb.sb_rgcount; i++)
			queue_work(&wq, check_rt_refcount_btrees, i, NULL);
	}
	destroy_work_queue(&wq);
}

static void
process_dup_rt_extents(
	struct xfs_mount	*mp)
{
	xfs_rtxnum_t		rt_start = 0;
	xfs_rtxlen_t		rt_len = 0;
	xfs_rtxnum_t		rtx;

	for (rtx = 0; rtx < mp->m_sb.sb_rextents; rtx++)  {
		int state;

		state = get_rtbmap(rtx);
		switch (state) {
		case XR_E_BAD_STATE:
		default:
			do_warn(
	_("unknown rt extent state %d, extent %" PRIu64 "\n"),
				state, rtx);
			fallthrough;
		case XR_E_METADATA:
		case XR_E_UNKNOWN:
		case XR_E_FREE1:
		case XR_E_FREE:
		case XR_E_INUSE:
		case XR_E_INUSE_FS:
		case XR_E_INO:
		case XR_E_FS_MAP:
			if (rt_start == 0)
				continue;
			/*
			 * Add extent and reset extent state.
			 */
			add_rt_dup_extent(rt_start, rt_len);
			rt_start = 0;
			rt_len = 0;
			break;
		case XR_E_MULT:
			switch (rt_start)  {
			case 0:
				rt_start = rtx;
				rt_len = 1;
				break;
			case XFS_MAX_BMBT_EXTLEN:
				/*
				 * Large extent case.
				 */
				add_rt_dup_extent(rt_start, rt_len);
				rt_start = rtx;
				rt_len = 1;
				break;
			default:
				rt_len++;
				break;
			}
			break;
		}
	}

	/*
	 * Catch the tail case, extent hitting the end of the RTG.
	 */
	if (rt_start != 0)
		add_rt_dup_extent(rt_start, rt_len);
}

/*
 * Set up duplicate extent list for an AG or RTG.
 */
static void
process_dup_extents(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno,
	xfs_agblock_t		ag_end,
	bool			isrt)
{
	do {
		int		bstate;
		xfs_extlen_t	blen;

		bstate = get_bmap_ext(agno, agbno, ag_end, &blen, isrt);
		switch (bstate) {
		case XR_E_FREE1:
			if (no_modify)
				do_warn(
_("free space (%u,%u-%u) only seen by one free space btree\n"),
					agno, agbno, agbno + blen - 1);
			break;
		case XR_E_METADATA:
		case XR_E_UNKNOWN:
		case XR_E_FREE:
		case XR_E_INUSE:
		case XR_E_INUSE_FS:
		case XR_E_INO:
		case XR_E_FS_MAP:
			break;
		case XR_E_MULT:
			/*
			 * Nothing is searching for duplicate RT extents, so
			 * don't bother tracking them.
			 */
			if (!isrt)
				add_dup_extent(agno, agbno, blen);
			break;
		case XR_E_BAD_STATE:
		default:
			do_warn(
_("unknown block state, ag %d, blocks %u-%u\n"),
				agno, agbno, agbno + blen - 1);
			break;
		}

		agbno += blen;
	} while (agbno < ag_end);
}

void
phase4(xfs_mount_t *mp)
{
	ino_tree_node_t		*irec;
	xfs_agnumber_t		i;
	int			ag_hdr_len = 4 * mp->m_sb.sb_sectsize;
	int			ag_hdr_block;

	if (rmap_needs_work(mp))
		collect_rmaps = true;
	ag_hdr_block = howmany(ag_hdr_len, mp->m_sb.sb_blocksize);

	do_log(_("Phase 4 - check for duplicate blocks...\n"));
	do_log(_("        - setting up duplicate extent list...\n"));

	set_progress_msg(PROG_FMT_DUP_EXTENT, (uint64_t) glob_agcount);

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino),
				XFS_INO_TO_AGINO(mp, mp->m_sb.sb_rootino));

	/*
	 * we always have a root inode, even if it's free...
	 * if the root is free, forget it, lost+found is already gone
	 */
	if (is_inode_free(irec, 0) || !inode_isadir(irec, 0))  {
		need_root_inode = 1;
		if (no_modify)
			do_warn(_("root inode would be lost\n"));
		else
			do_warn(_("root inode lost\n"));
	}

	/*
	 * If metadata directory trees are enabled, the metadata root directory
	 * always comes immediately after the regular root directory, even if
	 * it's free.
	 */
	if (xfs_has_metadir(mp) &&
	    (is_inode_free(irec, 1) || !inode_isadir(irec, 1))) {
		need_metadir_inode = true;
		if (no_modify)
			do_warn(
	_("metadata directory root inode would be lost\n"));
		else
			do_warn(
	_("metadata directory root inode lost\n"));
	}

	for (i = 0; i < mp->m_sb.sb_agcount; i++)  {
		xfs_agblock_t		ag_end;

		ag_end = (i < mp->m_sb.sb_agcount - 1) ? mp->m_sb.sb_agblocks :
			mp->m_sb.sb_dblocks -
				(xfs_rfsblock_t) mp->m_sb.sb_agblocks * i;

		process_dup_extents(mp, i, ag_hdr_block, ag_end, false);

		PROG_RPT_INC(prog_rpt_done[i], 1);
	}
	print_final_rpt();

	if (xfs_has_rtgroups(mp)) {
		for (i = 0; i < mp->m_sb.sb_rgcount; i++)  {
			uint64_t	rblocks;

			rblocks = xfs_rtbxlen_to_blen(mp,
					libxfs_rtgroup_extents(mp, i));
			process_dup_extents(mp, i, 0, rblocks, true);
		}
	} else {
		process_dup_rt_extents(mp);
	}

	/*
	 * initialize bitmaps for all AGs
	 */
	reset_bmaps(mp);

	do_log(_("        - check for inodes claiming duplicate blocks...\n"));
	set_progress_msg(PROG_FMT_DUP_BLOCKS, (uint64_t) mp->m_sb.sb_icount);

	/*
	 * ok, now process the inodes -- signal 2-pass check per inode.
	 * first pass checks if the inode conflicts with a known
	 * duplicate extent.  if so, the inode is cleared and second
	 * pass is skipped.  second pass sets the block bitmap
	 * for all blocks claimed by the inode.  directory
	 * and attribute processing is turned OFF since we did that
	 * already in phase 3.
	 */
	process_ags(mp);

	/*
	 * Process all the reverse-mapping data that we collected.  This
	 * involves checking the rmap data against the btree, computing
	 * reference counts based on the rmap data, and checking the counts
	 * against the refcount btree.
	 */
	process_rmap_data(mp);

	print_final_rpt();

	/*
	 * free up memory used to track trealtime duplicate extents
	 */
	free_rt_dup_extent_tree(mp);

	/*
	 * ensure consistency of quota inode pointers in superblock,
	 * make sure they point to real inodes
	 */
	quotino_check(mp);
	quota_sb_check(mp);

	/* Check the rt metadata before we rebuild */
	if (mp->m_sb.sb_rblocks)  {
		do_log(
		_("        - generate realtime summary info and bitmap...\n"));
		check_rtmetadata(mp);
	}
}
