// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_trans_resv.h"
#include "xfs_bit.h"
#include "xfs_sb.h"
#include "xfs_mount.h"
#include "xfs_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_rmap_btree.h"
#include "xfs_alloc.h"
#include "xfs_ialloc.h"
#include "xfs_rmap.h"
#include "xfs_ag.h"
#include "xfs_ag_resv.h"
#include "xfs_health.h"
#include "xfs_bmap.h"
#include "xfs_defer.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"
#include "xfs_trace.h"
#include "xfs_inode.h"
#include "xfs_rtgroup.h"
#include "xfs_rtbitmap.h"

int
xfs_rtgroup_alloc(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	xfs_rgnumber_t		rgcount,
	xfs_rtbxlen_t		rextents)
{
	struct xfs_rtgroup	*rtg;
	int			error;

	rtg = kzalloc(sizeof(struct xfs_rtgroup), GFP_KERNEL);
	if (!rtg)
		return -ENOMEM;

	error = xfs_group_insert(mp, rtg_group(rtg), rgno, XG_TYPE_RTG);
	if (error)
		goto out_free_rtg;
	return 0;

out_free_rtg:
	kfree(rtg);
	return error;
}

void
xfs_rtgroup_free(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	xfs_group_free(mp, rgno, XG_TYPE_RTG, NULL);
}

/* Free a range of incore rtgroup objects. */
void
xfs_free_rtgroups(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		first_rgno,
	xfs_rgnumber_t		end_rgno)
{
	xfs_rgnumber_t		rgno;

	for (rgno = first_rgno; rgno < end_rgno; rgno++)
		xfs_rtgroup_free(mp, rgno);
}

/* Initialize some range of incore rtgroup objects. */
int
xfs_initialize_rtgroups(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		first_rgno,
	xfs_rgnumber_t		end_rgno,
	xfs_rtbxlen_t		rextents)
{
	xfs_rgnumber_t		index;
	int			error;

	if (first_rgno >= end_rgno)
		return 0;

	for (index = first_rgno; index < end_rgno; index++) {
		error = xfs_rtgroup_alloc(mp, index, end_rgno, rextents);
		if (error)
			goto out_unwind_new_rtgs;
	}

	return 0;

out_unwind_new_rtgs:
	xfs_free_rtgroups(mp, first_rgno, index);
	return error;
}

/* Compute the number of rt extents in this realtime group. */
xfs_rtxnum_t
__xfs_rtgroup_extents(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno,
	xfs_rgnumber_t		rgcount,
	xfs_rtbxlen_t		rextents)
{
	ASSERT(rgno < rgcount);
	if (rgno == rgcount - 1)
		return rextents - ((xfs_rtxnum_t)rgno * mp->m_sb.sb_rgextents);

	ASSERT(xfs_has_rtgroups(mp));
	return mp->m_sb.sb_rgextents;
}

xfs_rtxnum_t
xfs_rtgroup_extents(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		rgno)
{
	return __xfs_rtgroup_extents(mp, rgno, mp->m_sb.sb_rgcount,
			mp->m_sb.sb_rextents);
}

/*
 * Update the rt extent count of the previous tail rtgroup if it changed during
 * recovery (i.e. recovery of a growfs).
 */
int
xfs_update_last_rtgroup_size(
	struct xfs_mount	*mp,
	xfs_rgnumber_t		prev_rgcount)
{
	struct xfs_rtgroup	*rtg;

	ASSERT(prev_rgcount > 0);

	rtg = xfs_rtgroup_grab(mp, prev_rgcount - 1);
	if (!rtg)
		return -EFSCORRUPTED;
	rtg->rtg_extents = __xfs_rtgroup_extents(mp, prev_rgcount - 1,
			mp->m_sb.sb_rgcount, mp->m_sb.sb_rextents);
	xfs_rtgroup_rele(rtg);
	return 0;
}
