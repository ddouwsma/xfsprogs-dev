// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
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
#include "progress.h"
#include "bmap.h"
#include "threads.h"
#include "rt.h"

static void
process_agi_unlinked(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno)
{
	struct xfs_buf		*bp;
	struct xfs_agi		*agip;
	xfs_agnumber_t		i;
	int			agi_dirty = 0;
	int			error;

	error = -libxfs_buf_read(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			mp->m_sb.sb_sectsize / BBSIZE, LIBXFS_READBUF_SALVAGE,
			&bp, &xfs_agi_buf_ops);
	if (error)
		do_error(_("cannot read agi block %" PRId64 " for ag %u\n"),
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)), agno);

	agip = bp->b_addr;

	ASSERT(be32_to_cpu(agip->agi_seqno) == agno);

	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)  {
		if (agip->agi_unlinked[i] != cpu_to_be32(NULLAGINO)) {
			agip->agi_unlinked[i] = cpu_to_be32(NULLAGINO);
			agi_dirty = 1;
		}
	}

	if (agi_dirty) {
		libxfs_buf_mark_dirty(bp);
		libxfs_buf_relse(bp);
	}
	else
		libxfs_buf_relse(bp);
}

static void
process_ag_func(
	struct workqueue	*wq,
	xfs_agnumber_t 		agno,
	void			*arg)
{
	/*
	 * turn on directory processing (inode discovery) and
	 * attribute processing (extra_attr_check)
	 */
	wait_for_inode_prefetch(arg);
	do_log(_("        - agno = %d\n"), agno);
	process_aginodes(wq->wq_ctx, arg, agno, 1, 0, 1);
	blkmap_free_final();
	cleanup_inode_prefetch(arg);
}

static void
process_ags(
	xfs_mount_t		*mp)
{
	do_inode_prefetch(mp, ag_stride, process_ag_func, false, false);
}

static void
do_uncertain_aginodes(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	int			*count = arg;

	*count = process_uncertain_aginodes(wq->wq_ctx, agno);

#ifdef XR_INODE_TRACE
	fprintf(stderr,
		"\t\t phase 3 - ag %d process_uncertain_inodes returns %d\n",
		*count, j);
#endif

	PROG_RPT_INC(prog_rpt_done[agno], 1);
}

void
phase3(
	struct xfs_mount *mp,
	int		scan_threads)
{
	int			i, j;
	int			*counts;
	struct workqueue	wq;

	do_log(_("Phase 3 - for each AG...\n"));
	if (!no_modify)
		do_log(_("        - scan and clear agi unlinked lists...\n"));
	else
		do_log(_("        - scan (but don't clear) agi unlinked lists...\n"));

	set_progress_msg(PROG_FMT_AGI_UNLINKED, (uint64_t) glob_agcount);

	if (xfs_has_rtsb(mp) && xfs_has_realtime(mp))
		check_rtsb(mp);

	/* first clear the agi unlinked AGI list */
	if (!no_modify) {
		for (i = 0; i < mp->m_sb.sb_agcount; i++)
			process_agi_unlinked(mp, i);
	}

	/* now look at possibly bogus inodes */
	for (i = 0; i < mp->m_sb.sb_agcount; i++)  {
		check_uncertain_aginodes(mp, i);
		PROG_RPT_INC(prog_rpt_done[i], 1);
	}
	print_final_rpt();

	/* ok, now that the tree's ok, let's take a good look */

	do_log(_(
	    "        - process known inodes and perform inode discovery...\n"));

	set_progress_msg(PROG_FMT_PROCESS_INO, (uint64_t) mp->m_sb.sb_icount);

	process_ags(mp);

	print_final_rpt();

	/*
	 * process newly discovered inode chunks
	 */
	do_log(_("        - process newly discovered inodes...\n"));
	set_progress_msg(PROG_FMT_NEW_INODES, (uint64_t) glob_agcount);

	counts = calloc(sizeof(*counts), mp->m_sb.sb_agcount);
	if (!counts) {
		do_abort(_("no memory for uncertain inode counts\n"));
		return;
	}

	do  {
		/*
		 * have to loop until no ag has any uncertain
		 * inodes
		 */
		j = 0;
		memset(counts, 0, mp->m_sb.sb_agcount * sizeof(*counts));

		create_work_queue(&wq, mp, scan_threads);

		for (i = 0; i < mp->m_sb.sb_agcount; i++)
			queue_work(&wq, do_uncertain_aginodes, i, &counts[i]);

		destroy_work_queue(&wq);

		/* tally up the counts */
		for (i = 0; i < mp->m_sb.sb_agcount; i++)
			j += counts[i];

	} while (j != 0);

	free(counts);

	print_final_rpt();
}
