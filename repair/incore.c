// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "btree.h"
#include "globals.h"
#include "incore.h"
#include "agheader.h"
#include "protos.h"
#include "err_protos.h"
#include "threads.h"

/*
 * The following manages the in-core bitmap of the entire filesystem
 * using extents in a btree.
 *
 * The btree items will point to one of the state values below,
 * rather than storing the value itself in the pointer.
 */
static int states[16] =
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

struct bmap {
	pthread_mutex_t		lock __attribute__((__aligned__(64)));
	struct btree_root	*root;
};
static struct bmap	*ag_bmaps;
static struct bmap	*rtg_bmaps;

static inline struct bmap *bmap_for_group(xfs_agnumber_t gno, bool isrt)
{
	if (isrt)
		return &rtg_bmaps[gno];
	return &ag_bmaps[gno];
}

void
lock_group(
	xfs_agnumber_t		gno,
	bool			isrt)
{
	pthread_mutex_lock(&bmap_for_group(gno, isrt)->lock);
}

void
unlock_group(
	xfs_agnumber_t		gno,
	bool			isrt)
{
	pthread_mutex_unlock(&bmap_for_group(gno, isrt)->lock);
}


void
set_bmap_ext(
	xfs_agnumber_t		gno,
	xfs_agblock_t		offset,
	xfs_extlen_t		blen,
	int			state,
	bool			isrt)
{
	struct btree_root	*bmap = bmap_for_group(gno, isrt)->root;
	void			*new_state = &states[state];
	unsigned long		end = offset + blen;
	int			*cur_state;
	unsigned long		cur_key;
	int			*next_state;
	unsigned long		next_key;
	int			*prev_state;

	cur_state = btree_find(bmap, offset, &cur_key);
	if (!cur_state)
		return;

	if (offset == cur_key) {
		/* if the start is the same as the "item" extent */
		if (cur_state == new_state)
			return;

		/*
		 * Note: this may be NULL if we are updating the map for
		 * the superblock.
		 */
		prev_state = btree_peek_prev(bmap, NULL);

		next_state = btree_peek_next(bmap, &next_key);
		if (next_key > end) {
			/* different end */
			if (new_state == prev_state) {
				/* #1: prev has same state, move offset up */
				btree_update_key(bmap, offset, end);
				return;
			}

			/* #4: insert new extent after, update current value */
			btree_update_value(bmap, offset, new_state);
			btree_insert(bmap, end, cur_state);
			return;
		}

		/* same end (and same start) */
		if (new_state == next_state) {
			/* next has same state */
			if (new_state == prev_state) {
				/* #3: merge prev & next */
				btree_delete(bmap, offset);
				btree_delete(bmap, end);
				return;
			}

			/* #8: merge next */
			btree_update_value(bmap, offset, new_state);
			btree_delete(bmap, end);
			return;
		}

		/* same start, same end, next has different state */
		if (new_state == prev_state) {
			/* #5: prev has same state */
			btree_delete(bmap, offset);
			return;
		}

		/* #6: update value only */
		btree_update_value(bmap, offset, new_state);
		return;
	}

	/* different start, offset is in the middle of "cur" */
	prev_state = btree_peek_prev(bmap, NULL);
	ASSERT(prev_state != NULL);
	if (prev_state == new_state)
		return;

	if (end == cur_key) {
		/* end is at the same point as the current extent */
		if (new_state == cur_state) {
			/* #7: move next extent down */
			btree_update_key(bmap, end, offset);
			return;
		}

		/* #9: different start, same end, add new extent */
		btree_insert(bmap, offset, new_state);
		return;
	}

	/* #2: insert an extent into the middle of another extent */
	btree_insert(bmap, offset, new_state);
	btree_insert(bmap, end, prev_state);
}

int
get_bmap_ext(
	xfs_agnumber_t		gno,
	xfs_agblock_t		agbno,
	xfs_agblock_t		maxbno,
	xfs_extlen_t		*blen,
	bool			isrt)
{
	struct btree_root	*bmap = bmap_for_group(gno, isrt)->root;
	int			*statep;
	unsigned long		key;

	statep = btree_find(bmap, agbno, &key);
	if (!statep)
		return -1;

	if (key == agbno) {
		if (blen) {
			if (!btree_peek_next(bmap, &key))
				return -1;
			*blen = min(maxbno, key) - agbno;
		}
		return *statep;
	}

	statep = btree_peek_prev(bmap, NULL);
	if (!statep)
		return -1;
	if (blen)
		*blen = min(maxbno, key) - agbno;

	return *statep;
}

static uint64_t		*rt_bmap;
static size_t		rt_bmap_size;
pthread_mutex_t		rt_lock;

/* block records fit into uint64_t's units */
#define XR_BB_UNIT	64			/* number of bits/unit */
#define XR_BB		4			/* bits per block record */
#define XR_BB_NUM	(XR_BB_UNIT/XR_BB)	/* number of records per unit */
#define XR_BB_MASK	0xF			/* block record mask */

/*
 * these work in real-time extents (e.g. fsbno == rt extent number)
 */
int
get_rtbmap(
	xfs_rtxnum_t	rtx)
{
	return (*(rt_bmap + rtx /  XR_BB_NUM) >>
		((rtx % XR_BB_NUM) * XR_BB)) & XR_BB_MASK;
}

void
set_rtbmap(
	xfs_rtxnum_t	rtx,
	int		state)
{
	*(rt_bmap + rtx / XR_BB_NUM) =
	 ((*(rt_bmap + rtx / XR_BB_NUM) &
	  (~((uint64_t) XR_BB_MASK << ((rtx % XR_BB_NUM) * XR_BB)))) |
	 (((uint64_t) state) << ((rtx % XR_BB_NUM) * XR_BB)));
}

static void
rtsb_init(
	struct xfs_mount	*mp)
{
	/* The first rtx of the realtime device contains the super */
	if (xfs_has_rtsb(mp) && rt_bmap)
		set_rtbmap(0, XR_E_INUSE_FS);
}

static void
reset_rt_bmap(void)
{
	if (rt_bmap)
		memset(rt_bmap, 0x22, rt_bmap_size);	/* XR_E_FREE */
}

static void
init_rt_bmap(
	xfs_mount_t	*mp)
{
	if (mp->m_sb.sb_rextents == 0)
		return;

	pthread_mutex_init(&rt_lock, NULL);
	rt_bmap_size = roundup(howmany(mp->m_sb.sb_rextents, (NBBY / XR_BB)),
			       sizeof(uint64_t));

	rt_bmap = memalign(sizeof(uint64_t), rt_bmap_size);
	if (!rt_bmap) {
		do_error(
	_("couldn't allocate realtime block map, size = %" PRIu64 "\n"),
			mp->m_sb.sb_rextents);
		return;
	}

	rtsb_init(mp);
}

static void
free_rt_bmap(xfs_mount_t *mp)
{
	free(rt_bmap);
	rt_bmap = NULL;
	pthread_mutex_destroy(&rt_lock);
}

static void
reset_ag_bmaps(
	struct xfs_mount	*mp)
{
	int			ag_hdr_block;
	xfs_agnumber_t		agno;
	xfs_agblock_t		ag_size;

	ag_hdr_block = howmany(4 * mp->m_sb.sb_sectsize, mp->m_sb.sb_blocksize);
	ag_size = mp->m_sb.sb_agblocks;

	for (agno = 0; agno < mp->m_sb.sb_agcount; agno++) {
		struct btree_root	*bmap = ag_bmaps[agno].root;

		if (agno == mp->m_sb.sb_agcount - 1)
			ag_size = (xfs_extlen_t)(mp->m_sb.sb_dblocks -
				   (xfs_rfsblock_t)mp->m_sb.sb_agblocks * agno);
#ifdef BTREE_STATS
		if (btree_find(bmap, 0, NULL)) {
			printf("ag_bmap[%d] btree stats:\n", i);
			btree_print_stats(bmap, stdout);
		}
#endif
		/*
		 * We always insert an item for the first block having a
		 * given state.  So the code below means:
		 *
		 *	block 0..ag_hdr_block-1:	XR_E_INUSE_FS
		 *	ag_hdr_block..ag_size:		XR_E_UNKNOWN
		 *	ag_size...			XR_E_BAD_STATE
		 */
		btree_clear(bmap);
		btree_insert(bmap, 0, &states[XR_E_INUSE_FS]);
		btree_insert(bmap, ag_hdr_block, &states[XR_E_UNKNOWN]);
		btree_insert(bmap, ag_size, &states[XR_E_BAD_STATE]);
	}
}

static void
reset_rtg_bmaps(
	struct xfs_mount	*mp)
{
	xfs_rgnumber_t		rgno;

	for (rgno = 0 ; rgno < mp->m_sb.sb_rgcount; rgno++) {
		struct btree_root	*bmap = rtg_bmaps[rgno].root;
		uint64_t		rblocks;

		btree_clear(bmap);
		if (rgno == 0 && xfs_has_rtsb(mp)) {
			btree_insert(bmap, 0, &states[XR_E_INUSE_FS]);
			btree_insert(bmap, mp->m_sb.sb_rextsize,
					&states[XR_E_FREE]);
		} else {
			btree_insert(bmap, 0, &states[XR_E_FREE]);
		}

		rblocks = xfs_rtbxlen_to_blen(mp,
				libxfs_rtgroup_extents(mp, rgno));
		btree_insert(bmap, rblocks, &states[XR_E_BAD_STATE]);
	}
}

void
reset_bmaps(
	struct xfs_mount	*mp)
{
	reset_ag_bmaps(mp);

	if (mp->m_sb.sb_logstart != 0) {
		set_bmap_ext(XFS_FSB_TO_AGNO(mp, mp->m_sb.sb_logstart),
			     XFS_FSB_TO_AGBNO(mp, mp->m_sb.sb_logstart),
			     mp->m_sb.sb_logblocks, XR_E_INUSE_FS, false);
	}

	if (xfs_has_rtgroups(mp)) {
		reset_rtg_bmaps(mp);
		rtsb_init(mp);
	} else {
		reset_rt_bmap();
	}
}

static struct bmap *
alloc_bmaps(
	unsigned int		nr_groups)
{
	struct bmap		*bmap;
	unsigned int		i;

	bmap = calloc(nr_groups, sizeof(*bmap));
	if (!bmap)
		return NULL;

	for (i = 0; i < nr_groups; i++)  {
		btree_init(&bmap[i].root);
		pthread_mutex_init(&bmap[i].lock, NULL);
	}

	return bmap;
}

static void
destroy_bmaps(
	struct bmap		*bmap,
	unsigned int		nr_groups)
{
	unsigned int		i;

	for (i = 0; i < nr_groups; i++) {
		btree_destroy(bmap[i].root);
		pthread_mutex_destroy(&bmap[i].lock);
	}

	free(bmap);
}

void
init_bmaps(
	struct xfs_mount	*mp)
{
	ag_bmaps = alloc_bmaps(mp->m_sb.sb_agcount);
	if (!ag_bmaps)
		do_error(_("couldn't allocate block map btree roots\n"));

	if (xfs_has_rtgroups(mp)) {
		rtg_bmaps = alloc_bmaps(mp->m_sb.sb_rgcount);
		if (!rtg_bmaps)
			do_error(_("couldn't allocate block map btree roots\n"));
	} else {
		init_rt_bmap(mp);
	}

	reset_bmaps(mp);
}

void
free_bmaps(
	struct xfs_mount	*mp)
{
	destroy_bmaps(ag_bmaps, mp->m_sb.sb_agcount);
	ag_bmaps = NULL;

	if (xfs_has_rtgroups(mp)) {
		destroy_bmaps(rtg_bmaps, mp->m_sb.sb_rgcount);
		rtg_bmaps = NULL;
	} else {
		free_rt_bmap(mp);
	}
}
