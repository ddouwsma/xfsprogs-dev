// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "scan.h"
#include "versions.h"
#include "bmap.h"
#include "progress.h"
#include "threads.h"
#include "slab.h"
#include "rmap.h"

static xfs_mount_t	*mp = NULL;

/*
 * Variables to validate AG header values against the manual count
 * from the btree traversal.
 */
struct aghdr_cnts {
	xfs_agnumber_t	agno;
	xfs_extlen_t	agffreeblks;
	xfs_extlen_t	agflongest;
	uint64_t	agfbtreeblks;
	uint32_t	agicount;
	uint32_t	agifreecount;
	uint64_t	fdblocks;
	uint64_t	usedblocks;
	uint64_t	ifreecount;
	uint32_t	fibtfreecount;
};

void
set_mp(xfs_mount_t *mpp)
{
	libxfs_bcache_purge(mp);
	mp = mpp;
}

/*
 * Read a buffer into memory, even if it fails verifier checks.
 * If an IO error happens, return a zeroed buffer.
 */
static inline int
salvage_buffer(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks,
	struct xfs_buf		**bpp,
	const struct xfs_buf_ops *ops)
{
	int			error;

	error = -libxfs_buf_read(target, blkno, numblks,
			LIBXFS_READBUF_SALVAGE, bpp, ops);
	if (error != EIO)
		return error;

	/*
	 * If the read produced an IO error, grab the buffer (which will now
	 * be full of zeroes) and make it look like we read the data from the
	 * disk but it failed verification.
	 */
	error = -libxfs_buf_get(target, blkno, numblks, bpp);
	if (error)
		return error;

	(*bpp)->b_error = -EFSCORRUPTED;
	(*bpp)->b_ops = ops;
	return 0;
}

static void
scan_sbtree(
	xfs_agblock_t	root,
	int		nlevels,
	xfs_agnumber_t	agno,
	int		suspect,
	void		(*func)(struct xfs_btree_block	*block,
				int			level,
				xfs_agblock_t		bno,
				xfs_agnumber_t		agno,
				int			suspect,
				int			isroot,
				uint32_t		magic,
				void			*priv,
				const struct xfs_buf_ops *ops),
	int		isroot,
	uint32_t	magic,
	void		*priv,
	const struct xfs_buf_ops *ops)
{
	struct xfs_buf	*bp;
	int		error;

	error = salvage_buffer(mp->m_dev, XFS_AGB_TO_DADDR(mp, agno, root),
			XFS_FSB_TO_BB(mp, 1), &bp, ops);
	if (error) {
		do_error(_("can't read btree block %d/%d\n"), agno, root);
		return;
	}
	if (bp->b_error == -EFSBADCRC || bp->b_error == -EFSCORRUPTED) {
		do_warn(_("btree block %d/%d is suspect, error %d\n"),
			agno, root, bp->b_error);
		suspect = 1;
	}

	(*func)(XFS_BUF_TO_BLOCK(bp), nlevels - 1, root, agno, suspect,
			isroot, magic, priv, ops);
	libxfs_buf_relse(bp);
}

/*
 * returns 1 on bad news (inode needs to be cleared), 0 on good
 */
int
scan_lbtree(
	xfs_fsblock_t	root,
	int		nlevels,
	int		(*func)(struct xfs_btree_block	*block,
				int			level,
				int			type,
				int			whichfork,
				xfs_fsblock_t		bno,
				xfs_ino_t		ino,
				xfs_rfsblock_t		*tot,
				xfs_extnum_t		*nex,
				blkmap_t		**blkmapp,
				bmap_cursor_t		*bm_cursor,
				int			suspect,
				int			isroot,
				int			check_dups,
				int			*dirty,
				uint64_t		magic,
				void			*priv),
	int		type,
	int		whichfork,
	xfs_ino_t	ino,
	xfs_rfsblock_t	*tot,
	xfs_extnum_t	*nex,
	blkmap_t	**blkmapp,
	bmap_cursor_t	*bm_cursor,
	int		suspect,
	int		isroot,
	int		check_dups,
	uint64_t	magic,
	void		*priv,
	const struct xfs_buf_ops *ops)
{
	struct xfs_buf	*bp;
	int		err;
	int		dirty = 0;
	bool		badcrc = false;

	err = salvage_buffer(mp->m_dev, XFS_FSB_TO_DADDR(mp, root),
			XFS_FSB_TO_BB(mp, 1), &bp, ops);
	if (err) {
		do_error(_("can't read btree block %d/%d\n"),
			XFS_FSB_TO_AGNO(mp, root),
			XFS_FSB_TO_AGBNO(mp, root));
		return(1);
	}
	if (bp->b_error == -EFSBADCRC || bp->b_error == -EFSCORRUPTED) {
		do_warn(_("btree block %d/%d is suspect, error %d\n"),
			XFS_FSB_TO_AGNO(mp, root),
			XFS_FSB_TO_AGBNO(mp, root), bp->b_error);
		suspect++;
	}

	/*
	 * only check for bad CRC here - caller will determine if there
	 * is a corruption or not and whether it got corrected and so needs
	 * writing back. CRC errors always imply we need to write the block.
	 */
	if (bp->b_error == -EFSBADCRC) {
		do_warn(_("btree block %d/%d is suspect, error %d\n"),
			XFS_FSB_TO_AGNO(mp, root),
			XFS_FSB_TO_AGBNO(mp, root), bp->b_error);
		badcrc = true;
	}

	err = (*func)(XFS_BUF_TO_BLOCK(bp), nlevels - 1,
			type, whichfork, root, ino, tot, nex, blkmapp,
			bm_cursor, suspect, isroot, check_dups, &dirty,
			magic, priv);

	ASSERT(dirty == 0 || (dirty && !no_modify));

	if (!err && (dirty || badcrc) && !no_modify) {
		libxfs_buf_mark_dirty(bp);
		libxfs_buf_relse(bp);
	}
	else
		libxfs_buf_relse(bp);

	return(err);
}

int
scan_bmapbt(
	struct xfs_btree_block	*block,
	int			level,
	int			type,
	int			whichfork,
	xfs_fsblock_t		bno,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	xfs_extnum_t		*nex,
	blkmap_t		**blkmapp,
	bmap_cursor_t		*bm_cursor,
	int			suspect,
	int			isroot,
	int			check_dups,
	int			*dirty,
	uint64_t		magic,
	void			*priv)
{
	int			i;
	int			err;
	xfs_bmbt_ptr_t		*pp;
	xfs_bmbt_key_t		*pkey;
	xfs_bmbt_rec_t		*rp;
	xfs_fileoff_t		first_key;
	xfs_fileoff_t		last_key;
	char			*forkname = get_forkname(whichfork);
	xfs_extnum_t		numrecs;
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno;
	int			state;
	bool			zap_metadata = priv != NULL;

	/*
	 * unlike the ag freeblock btrees, if anything looks wrong
	 * in an inode bmap tree, just bail.  it's possible that
	 * we'll miss a case where the to-be-toasted inode and
	 * another inode are claiming the same block but that's
	 * highly unlikely.
	 */
	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(
_("bad magic # %#x in inode %" PRIu64 " (%s fork) bmbt block %" PRIu64 "\n"),
			be32_to_cpu(block->bb_magic), ino, forkname, bno);
		return(1);
	}
	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(
_("expected level %d got %d in inode %" PRIu64 ", (%s fork) bmbt block %" PRIu64 "\n"),
			level, be16_to_cpu(block->bb_level),
			ino, forkname, bno);
		return(1);
	}

	if (magic == XFS_BMAP_CRC_MAGIC) {
		/* verify owner */
		if (be64_to_cpu(block->bb_u.l.bb_owner) != ino) {
			do_warn(
_("expected owner inode %" PRIu64 ", got %llu, bmbt block %" PRIu64 "\n"),
				ino,
				(unsigned long long)be64_to_cpu(block->bb_u.l.bb_owner),
				bno);
			return 1;
		}
		/* verify block number */
		if (be64_to_cpu(block->bb_u.l.bb_blkno) !=
		    XFS_FSB_TO_DADDR(mp, bno)) {
			do_warn(
_("expected block %" PRIu64 ", got %llu, bmbt block %" PRIu64 "\n"),
				XFS_FSB_TO_DADDR(mp, bno),
				(unsigned long long)be64_to_cpu(block->bb_u.l.bb_blkno),
				bno);
			return 1;
		}
		/* verify uuid */
		if (platform_uuid_compare(&block->bb_u.l.bb_uuid,
					  &mp->m_sb.sb_meta_uuid) != 0) {
			do_warn(
_("wrong FS UUID, bmbt block %" PRIu64 "\n"),
				bno);
			return 1;
		}
	}

	if (check_dups == 0)  {
		/*
		 * check sibling pointers. if bad we have a conflict
		 * between the sibling pointers and the child pointers
		 * in the parent block.  blow out the inode if that happens
		 */
		if (bm_cursor->level[level].fsbno != NULLFSBLOCK)  {
			/*
			 * this is not the first block on this level
			 * so the cursor for this level has recorded the
			 * values for this's block left-sibling.
			 */
			if (bno != bm_cursor->level[level].right_fsbno)  {
				do_warn(
_("bad fwd (right) sibling pointer (saw %" PRIu64 " parent block says %" PRIu64 ")\n"
  "\tin inode %" PRIu64 " (%s fork) bmap btree block %" PRIu64 "\n"),
					bm_cursor->level[level].right_fsbno,
					bno, ino, forkname,
					bm_cursor->level[level].fsbno);
				return(1);
			}
			if (be64_to_cpu(block->bb_u.l.bb_leftsib) !=
					bm_cursor->level[level].fsbno)  {
				do_warn(
_("bad back (left) sibling pointer (saw %llu parent block says %" PRIu64 ")\n"
  "\tin inode %" PRIu64 " (%s fork) bmap btree block %" PRIu64 "\n"),
				       (unsigned long long)
					       be64_to_cpu(block->bb_u.l.bb_leftsib),
					bm_cursor->level[level].fsbno,
					ino, forkname, bno);
				return(1);
			}
		} else {
			/*
			 * This is the first or only block on this level.
			 * Check that the left sibling pointer is NULL
			 */
			if (be64_to_cpu(block->bb_u.l.bb_leftsib) != NULLFSBLOCK) {
				do_warn(
_("bad back (left) sibling pointer (saw %llu should be NULL (0))\n"
  "\tin inode %" PRIu64 " (%s fork) bmap btree block %" PRIu64 "\n"),
				       (unsigned long long)
					       be64_to_cpu(block->bb_u.l.bb_leftsib),
					ino, forkname, bno);
				return(1);
			}
		}

		/*
		 * update cursor block pointers to reflect this block
		 */
		bm_cursor->level[level].fsbno = bno;
		bm_cursor->level[level].left_fsbno =
					be64_to_cpu(block->bb_u.l.bb_leftsib);
		bm_cursor->level[level].right_fsbno =
					be64_to_cpu(block->bb_u.l.bb_rightsib);

		agno = XFS_FSB_TO_AGNO(mp, bno);
		agbno = XFS_FSB_TO_AGBNO(mp, bno);

		lock_ag(agno);
		state = get_bmap(agno, agbno);
		switch (state) {
		case XR_E_INUSE1:
			/*
			 * block was claimed as in use data by the rmap
			 * btree, but has not been found in the data extent
			 * map for the inode. That means this bmbt block hasn't
			 * yet been claimed as in use, which means -it's ours-
			 */
		case XR_E_UNKNOWN:
		case XR_E_FREE1:
		case XR_E_FREE:
			set_bmap(agno, agbno, zap_metadata ? XR_E_METADATA :
							     XR_E_INUSE);
			break;
		case XR_E_METADATA:
			/*
			 * bmbt block already claimed by a metadata file.  We
			 * always reconstruct the entire metadata tree, so if
			 * this is a regular file we mark it owned by the file.
			 */
			do_warn(
_("inode 0x%" PRIx64 "bmap block 0x%" PRIx64 " claimed by metadata file\n"),
				ino, bno);
			if (!zap_metadata)
				set_bmap(agno, agbno, XR_E_INUSE);
			break;
		case XR_E_FS_MAP:
		case XR_E_INUSE:
			/*
			 * we'll try and continue searching here since
			 * the block looks like it's been claimed by file
			 * to store user data, a directory to store directory
			 * data, or the space allocation btrees but since
			 * we made it here, the block probably
			 * contains btree data.
			 */
			if (!zap_metadata)
				set_bmap(agno, agbno, XR_E_MULT);
			do_warn(
_("inode 0x%" PRIx64 "bmap block 0x%" PRIx64 " claimed, state is %d\n"),
				ino, bno, state);
			break;
		case XR_E_MULT:
		case XR_E_INUSE_FS:
			set_bmap(agno, agbno, XR_E_MULT);
			do_warn(
_("inode 0x%" PRIx64 " bmap block 0x%" PRIx64 " claimed, state is %d\n"),
				ino, bno, state);
			/*
			 * if we made it to here, this is probably a bmap block
			 * that is being used by *another* file as a bmap block
			 * so the block will be valid.  Both files should be
			 * trashed along with any other file that impinges on
			 * any blocks referenced by either file.  So we
			 * continue searching down this btree to mark all
			 * blocks duplicate
			 */
			break;
		case XR_E_BAD_STATE:
		default:
			do_warn(
_("bad state %d, inode %" PRIu64 " bmap block 0x%" PRIx64 "\n"),
				state, ino, bno);
			break;
		}
		unlock_ag(agno);
	} else {
		if (search_dup_extent(XFS_FSB_TO_AGNO(mp, bno),
				XFS_FSB_TO_AGBNO(mp, bno),
				XFS_FSB_TO_AGBNO(mp, bno) + 1))
			return 1;
	}
	(*tot)++;
	numrecs = be16_to_cpu(block->bb_numrecs);

	/* Record BMBT blocks in the reverse-mapping data. */
	if (check_dups && collect_rmaps && !zap_metadata) {
		agno = XFS_FSB_TO_AGNO(mp, bno);
		lock_ag(agno);
		rmap_add_bmbt_rec(mp, ino, whichfork, bno);
		unlock_ag(agno);
	}

	if (level == 0) {
		if (numrecs > mp->m_bmap_dmxr[0] || (isroot == 0 && numrecs <
							mp->m_bmap_dmnr[0])) {
				do_warn(
_("inode %" PRIu64 " bad # of bmap records (%" PRIu64 ", min - %u, max - %u)\n"),
					ino, numrecs, mp->m_bmap_dmnr[0],
					mp->m_bmap_dmxr[0]);
			return(1);
		}
		rp = xfs_bmbt_rec_addr(mp, block, 1);
		*nex += numrecs;
		/*
		 * XXX - if we were going to fix up the btree record,
		 * we'd do it right here.  For now, if there's a problem,
		 * we'll bail out and presumably clear the inode.
		 */
		if (check_dups == 0)  {
			err = process_bmbt_reclist(mp, rp, &numrecs, type, ino,
						   tot, blkmapp, &first_key,
						   &last_key, whichfork,
						   zap_metadata);
			if (err)
				return 1;

			/*
			 * check that key ordering is monotonically increasing.
			 * if the last_key value in the cursor is set to
			 * NULLFILEOFF, then we know this is the first block
			 * on the leaf level and we shouldn't check the
			 * last_key value.
			 */
			if (first_key <= bm_cursor->level[level].last_key &&
					bm_cursor->level[level].last_key !=
					NULLFILEOFF)  {
				do_warn(
_("out-of-order bmap key (file offset) in inode %" PRIu64 ", %s fork, fsbno %" PRIu64 "\n"),
					ino, forkname, bno);
				return(1);
			}
			/*
			 * update cursor keys to reflect this block.
			 * don't have to check if last_key is > first_key
			 * since that gets checked by process_bmbt_reclist.
			 */
			bm_cursor->level[level].first_key = first_key;
			bm_cursor->level[level].last_key = last_key;

			return 0;
		} else {
			return scan_bmbt_reclist(mp, rp, &numrecs, type, ino,
					tot, whichfork, zap_metadata);
		}
	}
	if (numrecs > mp->m_bmap_dmxr[1] || (isroot == 0 && numrecs <
							mp->m_bmap_dmnr[1])) {
		do_warn(
_("inode %" PRIu64 " bad # of bmap records (%" PRIu64 ", min - %u, max - %u)\n"),
			ino, numrecs, mp->m_bmap_dmnr[1], mp->m_bmap_dmxr[1]);
		return(1);
	}
	pp = xfs_bmbt_ptr_addr(mp, block, 1, mp->m_bmap_dmxr[1]);
	pkey = xfs_bmbt_key_addr(mp, block, 1);

	last_key = NULLFILEOFF;

	for (i = 0, err = 0; i < numrecs; i++)  {
		/*
		 * XXX - if we were going to fix up the interior btree nodes,
		 * we'd do it right here.  For now, if there's a problem,
		 * we'll bail out and presumably clear the inode.
		 */
		if (!libxfs_verify_fsbno(mp, be64_to_cpu(pp[i])))  {
			do_warn(
_("bad bmap btree ptr 0x%llx in ino %" PRIu64 "\n"),
			       (unsigned long long) be64_to_cpu(pp[i]), ino);
			return(1);
		}

		err = scan_lbtree(be64_to_cpu(pp[i]), level, scan_bmapbt,
				type, whichfork, ino, tot, nex, blkmapp,
				bm_cursor, suspect, 0, check_dups, magic, priv,
				&xfs_bmbt_buf_ops);
		if (err)
			return(1);

		/*
		 * fix key (offset) mismatches between the first key
		 * in the child block (as recorded in the cursor) and the
		 * key in the interior node referencing the child block.
		 *
		 * fixes cases where entries have been shifted between
		 * child blocks but the parent hasn't been updated.  We
		 * don't have to worry about the key values in the cursor
		 * not being set since we only look at the key values of
		 * our child and those are guaranteed to be set by the
		 * call to scan_lbtree() above.
		 */
		if (check_dups == 0 && be64_to_cpu(pkey[i].br_startoff) !=
					bm_cursor->level[level-1].first_key)  {
			if (!no_modify)  {
				do_warn(
_("correcting bt key (was %llu, now %" PRIu64 ") in inode %" PRIu64 "\n"
  "\t\t%s fork, btree block %" PRIu64 "\n"),
				       (unsigned long long)
					       be64_to_cpu(pkey[i].br_startoff),
					bm_cursor->level[level-1].first_key,
					ino,
					forkname, bno);
				*dirty = 1;
				pkey[i].br_startoff = cpu_to_be64(
					bm_cursor->level[level-1].first_key);
			} else  {
				do_warn(
_("bad btree key (is %llu, should be %" PRIu64 ") in inode %" PRIu64 "\n"
  "\t\t%s fork, btree block %" PRIu64 "\n"),
				       (unsigned long long)
					       be64_to_cpu(pkey[i].br_startoff),
					bm_cursor->level[level-1].first_key,
					ino, forkname, bno);
			}
		}
	}

	/*
	 * If we're the last node at our level, check that the last child
	 * block's forward sibling pointer is NULL.
	 */
	if (check_dups == 0 &&
			bm_cursor->level[level].right_fsbno == NULLFSBLOCK &&
			bm_cursor->level[level - 1].right_fsbno != NULLFSBLOCK) {
		do_warn(
_("bad fwd (right) sibling pointer (saw %" PRIu64 " should be NULLFSBLOCK)\n"
  "\tin inode %" PRIu64 " (%s fork) bmap btree block %" PRIu64 "\n"),
			bm_cursor->level[level - 1].right_fsbno,
			ino, forkname, bm_cursor->level[level - 1].fsbno);
		return(1);
	}

	/*
	 * update cursor keys to reflect this block
	 */
	if (check_dups == 0)  {
		bm_cursor->level[level].first_key =
				be64_to_cpu(pkey[0].br_startoff);
		bm_cursor->level[level].last_key =
				be64_to_cpu(pkey[numrecs - 1].br_startoff);
	}

	return suspect > 0 ? 1 : 0;
}

static void
scan_allocbt(
	struct xfs_btree_block	*block,
	int			level,
	xfs_agblock_t		bno,
	xfs_agnumber_t		agno,
	int			suspect,
	int			isroot,
	uint32_t		magic,
	void			*priv,
	const struct xfs_buf_ops *ops)
{
	struct aghdr_cnts	*agcnts = priv;
	struct xfs_perag	*pag;
	const char 		*name;
	int			i;
	xfs_alloc_ptr_t		*pp;
	xfs_alloc_rec_t		*rp;
	int			hdr_errors = 0;
	int			numrecs;
	int			state;
	xfs_extlen_t		lastcount = 0;
	xfs_agblock_t		lastblock = 0;

	switch (magic) {
	case XFS_ABTB_CRC_MAGIC:
	case XFS_ABTB_MAGIC:
		name = "bno";
		break;
	case XFS_ABTC_CRC_MAGIC:
	case XFS_ABTC_MAGIC:
		name = "cnt";
		break;
	default:
		name = "(unknown)";
		assert(0);
		break;
	}

	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(_("bad magic # %#x in bt%s block %d/%d\n"),
			be32_to_cpu(block->bb_magic), name, agno, bno);
		hdr_errors++;
		if (suspect)
			return;
	}

	/*
	 * All freespace btree blocks except the roots are freed for a
	 * fully used filesystem, thus they are counted towards the
	 * free data block counter.
	 */
	if (!isroot) {
		agcnts->agfbtreeblks++;
		agcnts->fdblocks++;
	}

	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(_("expected level %d got %d in bt%s block %d/%d\n"),
			level, be16_to_cpu(block->bb_level), name, agno, bno);
		hdr_errors++;
		if (suspect)
			return;
	}

	/*
	 * check for btree blocks multiply claimed
	 */
	state = get_bmap(agno, bno);
	if (state != XR_E_UNKNOWN)  {
		set_bmap(agno, bno, XR_E_MULT);
		do_warn(
_("%s freespace btree block claimed (state %d), agno %d, bno %d, suspect %d\n"),
				name, state, agno, bno, suspect);
		return;
	}
	set_bmap(agno, bno, XR_E_FS_MAP);

	numrecs = be16_to_cpu(block->bb_numrecs);

	if (level == 0) {
		if (numrecs > mp->m_alloc_mxr[0])  {
			numrecs = mp->m_alloc_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < mp->m_alloc_mnr[0])  {
			numrecs = mp->m_alloc_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors) {
			do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
				be16_to_cpu(block->bb_numrecs),
				mp->m_alloc_mnr[0], mp->m_alloc_mxr[0],
				name, agno, bno);
			suspect++;
		}

		rp = XFS_ALLOC_REC_ADDR(mp, block, 1);
		pag = libxfs_perag_get(mp, agno);

		for (i = 0; i < numrecs; i++) {
			xfs_agblock_t		b, end;
			xfs_extlen_t		len, blen;

			b = be32_to_cpu(rp[i].ar_startblock);
			len = be32_to_cpu(rp[i].ar_blockcount);
			end = b + len;

			if (!libxfs_verify_agbno(pag, b)) {
				do_warn(
	_("invalid start block %u in record %u of %s btree block %u/%u\n"),
					b, i, name, agno, bno);
				continue;
			}
			if (len == 0 || end <= b ||
			    !libxfs_verify_agbno(pag, end - 1)) {
				do_warn(
	_("invalid length %u in record %u of %s btree block %u/%u\n"),
					len, i, name, agno, bno);
				continue;
			}

			if (magic == XFS_ABTB_MAGIC ||
			    magic == XFS_ABTB_CRC_MAGIC) {
				if (b <= lastblock) {
					do_warn(_(
	"out-of-order bno btree record %d (%u %u) block %u/%u\n"),
						i, b, len, agno, bno);
				} else {
					lastblock = end - 1;
				}
			} else {
				agcnts->fdblocks += len;
				agcnts->agffreeblks += len;
				if (len > agcnts->agflongest)
					agcnts->agflongest = len;
				if (len < lastcount) {
					do_warn(_(
	"out-of-order cnt btree record %d (%u %u) block %u/%u\n"),
						i, b, len, agno, bno);
				} else {
					lastcount = len;
				}
			}

			for ( ; b < end; b += blen)  {
				state = get_bmap_ext(agno, b, end, &blen, false);
				switch (state) {
				case XR_E_UNKNOWN:
					set_bmap_ext(agno, b, blen, XR_E_FREE1,
							false);
					break;
				case XR_E_FREE1:
					/*
					 * no warning messages -- we'll catch
					 * FREE1 blocks later
					 */
					if (magic == XFS_ABTC_MAGIC ||
					    magic == XFS_ABTC_CRC_MAGIC) {
						set_bmap_ext(agno, b, blen,
							     XR_E_FREE, false);
						break;
					}
					fallthrough;
				default:
					do_warn(
	_("block (%d,%d-%d) multiply claimed by %s space tree, state - %d\n"),
						agno, b, b + blen - 1,
						name, state);
					break;
				}
			}
		}
		libxfs_perag_put(pag);
		return;
	}

	/*
	 * interior record
	 */
	pp = XFS_ALLOC_PTR_ADDR(mp, block, 1, mp->m_alloc_mxr[1]);

	if (numrecs > mp->m_alloc_mxr[1])  {
		numrecs = mp->m_alloc_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < mp->m_alloc_mnr[1])  {
		numrecs = mp->m_alloc_mnr[1];
		hdr_errors++;
	}

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */
	if (hdr_errors)  {
		do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
			be16_to_cpu(block->bb_numrecs),
			mp->m_alloc_mnr[1], mp->m_alloc_mxr[1],
			name, agno, bno);
		if (suspect)
			return;
		suspect++;
	} else if (suspect) {
		suspect = 0;
	}

	pag = libxfs_perag_get(mp, agno);
	for (i = 0; i < numrecs; i++)  {
		xfs_agblock_t		agbno = be32_to_cpu(pp[i]);

		if (!libxfs_verify_agbno(pag, agbno)) {
			do_warn(
	_("bad btree pointer (%u) in %sbt block %u/%u\n"),
				agbno, name, agno, bno);
			suspect++;
			libxfs_perag_put(pag);
			return;
		}

		/*
		 * XXX - put sibling detection right here.
		 * we know our sibling chain is good.  So as we go,
		 * we check the entry before and after each entry.
		 * If either of the entries references a different block,
		 * check the sibling pointer.  If there's a sibling
		 * pointer mismatch, try and extract as much data
		 * as possible.
		 */
		scan_sbtree(agbno, level, agno, suspect, scan_allocbt, 0,
				magic, priv, ops);
	}
	libxfs_perag_put(pag);
}

static bool
ino_issparse(
	struct xfs_inobt_rec	*rp,
	int			offset)
{
	if (!xfs_has_sparseinodes(mp))
		return false;

	return xfs_inobt_is_sparse_disk(rp, offset);
}

/* See if the rmapbt owners agree with our observations. */
static void
process_rmap_rec(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		b,
	xfs_agblock_t		end,
	xfs_extlen_t		blen,
	int64_t			owner,
	int			state,
	const char		*name)
{
	switch (state) {
	case XR_E_UNKNOWN:
		switch (owner) {
		case XFS_RMAP_OWN_FS:
		case XFS_RMAP_OWN_LOG:
			set_bmap_ext(agno, b, blen, XR_E_INUSE_FS1, false);
			break;
		case XFS_RMAP_OWN_AG:
		case XFS_RMAP_OWN_INOBT:
			set_bmap_ext(agno, b, blen, XR_E_FS_MAP1, false);
			break;
		case XFS_RMAP_OWN_INODES:
			set_bmap_ext(agno, b, blen, XR_E_INO1, false);
			break;
		case XFS_RMAP_OWN_REFC:
			set_bmap_ext(agno, b, blen, XR_E_REFC, false);
			break;
		case XFS_RMAP_OWN_COW:
			set_bmap_ext(agno, b, blen, XR_E_COW, false);
			break;
		case XFS_RMAP_OWN_NULL:
			/* still unknown */
			break;
		default:
			/* file data */
			set_bmap_ext(agno, b, blen, XR_E_INUSE1, false);
			break;
		}
		break;
	case XR_E_METADATA:
		do_warn(
_("Metadata file block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_INUSE_FS:
		if (owner == XFS_RMAP_OWN_FS ||
		    owner == XFS_RMAP_OWN_LOG)
			break;
		do_warn(
_("Static meta block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_FS_MAP:
		if (owner == XFS_RMAP_OWN_AG ||
		    owner == XFS_RMAP_OWN_INOBT)
			break;
		do_warn(
_("AG meta block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_INO:
		if (owner == XFS_RMAP_OWN_INODES)
			break;
		do_warn(
_("inode block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_REFC:
		if (owner == XFS_RMAP_OWN_REFC)
			break;
		do_warn(
_("AG refcount block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_INUSE:
		if (owner >= 0 &&
		    owner < mp->m_sb.sb_dblocks)
			break;
		do_warn(
_("in use block (%d,%d-%d) mismatch in %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	case XR_E_FREE1:
	case XR_E_FREE:
		/*
		 * May be on the AGFL. If not, they'll
		 * be caught later.
		 */
		break;
	case XR_E_INUSE1:
		/*
		 * multiple inode owners are ok with
		 * reflink enabled
		 */
		if (xfs_has_reflink(mp) &&
		    !XFS_RMAP_NON_INODE_OWNER(owner))
			break;
		fallthrough;
	default:
		do_warn(
_("unknown block (%d,%d-%d) mismatch on %s tree, state - %d,%" PRIx64 "\n"),
			agno, b, b + blen - 1,
			name, state, owner);
		break;
	}
}

static bool
rmap_in_order(
	xfs_agblock_t	b,
	xfs_agblock_t	laststartblock,
	uint64_t	owner,
	uint64_t	lastowner,
	uint64_t	offset,
	uint64_t	lastoffset)
{
	if (b > laststartblock)
		return true;
	else if (b < laststartblock)
		return false;

	if (owner > lastowner)
		return true;
	else if (owner < lastowner)
		return false;

	return offset > lastoffset;
}

static inline bool
verify_rmap_agbno(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agblock_t		agbno)
{
	return agbno < libxfs_ag_block_count(mp, agno);
}

static inline void
warn_rmap_unwritten_key(
	xfs_agblock_t		agno)
{
	static bool		warned = false;
	static pthread_mutex_t	lock = PTHREAD_MUTEX_INITIALIZER;

	if (warned)
		return;

	pthread_mutex_lock(&lock);
	if (!warned) {
		if (no_modify)
			do_log(
 _("would clear unwritten flag on rmapbt key in agno 0x%x\n"),
			       agno);
		else
			do_warn(
 _("clearing unwritten flag on rmapbt key in agno 0x%x\n"),
			       agno);
		warned = true;
	}
	pthread_mutex_unlock(&lock);
}

static void
scan_rmapbt(
	struct xfs_btree_block	*block,
	int			level,
	xfs_agblock_t		bno,
	xfs_agnumber_t		agno,
	int			suspect,
	int			isroot,
	uint32_t		magic,
	void			*priv,
	const struct xfs_buf_ops *ops)
{
	const char		*name = "rmap";
	int			i;
	xfs_rmap_ptr_t		*pp;
	struct xfs_rmap_rec	*rp;
	struct rmap_priv	*rmap_priv = priv;
	int			hdr_errors = 0;
	int			numrecs;
	int			state;
	xfs_agblock_t		laststartblock = 0;
	xfs_agblock_t		lastblock = 0;
	uint64_t		lastowner = 0;
	uint64_t		lastoffset = 0;
	struct xfs_rmap_key	*kp;
	struct xfs_rmap_irec	oldkey;
	struct xfs_rmap_irec	key = {0};
	struct xfs_perag	*pag;

	if (magic != XFS_RMAP_CRC_MAGIC) {
		name = "(unknown)";
		hdr_errors++;
		suspect++;
		goto out;
	}

	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(_("bad magic # %#x in bt%s block %d/%d\n"),
			be32_to_cpu(block->bb_magic), name, agno, bno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	/*
	 * All RMAP btree blocks except the roots are freed for a
	 * fully empty filesystem, thus they are counted towards the
	 * free data block counter.
	 */
	if (!isroot) {
		rmap_priv->agcnts->agfbtreeblks++;
		rmap_priv->agcnts->fdblocks++;
	}
	rmap_priv->nr_blocks++;

	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(_("expected level %d got %d in bt%s block %d/%d\n"),
			level, be16_to_cpu(block->bb_level), name, agno, bno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	/* check for btree blocks multiply claimed */
	state = get_bmap(agno, bno);
	if (!(state == XR_E_UNKNOWN || state == XR_E_FS_MAP1))  {
		set_bmap(agno, bno, XR_E_MULT);
		do_warn(
_("%s rmap btree block claimed (state %d), agno %d, bno %d, suspect %d\n"),
				name, state, agno, bno, suspect);
		goto out;
	}
	set_bmap(agno, bno, XR_E_FS_MAP);

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (level == 0) {
		if (numrecs > mp->m_rmap_mxr[0])  {
			numrecs = mp->m_rmap_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < mp->m_rmap_mnr[0])  {
			numrecs = mp->m_rmap_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors) {
			do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
				be16_to_cpu(block->bb_numrecs),
				mp->m_rmap_mnr[0], mp->m_rmap_mxr[0],
				name, agno, bno);
			suspect++;
		}

		rp = XFS_RMAP_REC_ADDR(block, 1);
		for (i = 0; i < numrecs; i++) {
			xfs_agblock_t		b, end;
			xfs_extlen_t		len, blen;
			int64_t			owner, offset;

			b = be32_to_cpu(rp[i].rm_startblock);
			len = be32_to_cpu(rp[i].rm_blockcount);
			owner = be64_to_cpu(rp[i].rm_owner);
			offset = be64_to_cpu(rp[i].rm_offset);

			key.rm_flags = 0;
			key.rm_startblock = b;
			key.rm_blockcount = len;
			key.rm_owner = owner;
			if (libxfs_rmap_irec_offset_unpack(offset, &key)) {
				/* Look for impossible flags. */
				do_warn(
	_("invalid flags in record %u of %s btree block %u/%u\n"),
					i, name, agno, bno);
				continue;
			}

			end = key.rm_startblock + key.rm_blockcount;

			/* Make sure agbno & len make sense. */
			if (!verify_rmap_agbno(mp, agno, b)) {
				do_warn(
	_("invalid start block %u in record %u of %s btree block %u/%u\n"),
					b, i, name, agno, bno);
				continue;
			}
			if (len == 0 || end <= b ||
			    !verify_rmap_agbno(mp, agno, end - 1)) {
				do_warn(
	_("invalid length %u in record %u of %s btree block %u/%u\n"),
					len, i, name, agno, bno);
				continue;
			}

			/* Look for impossible owners. */
			if (!((owner > XFS_RMAP_OWN_MIN &&
			       owner <= XFS_RMAP_OWN_FS) ||
			      (XFS_INO_TO_AGNO(mp, owner) < mp->m_sb.sb_agcount &&
			       XFS_AGINO_TO_AGBNO(mp,
					XFS_INO_TO_AGINO(mp, owner)) <
					mp->m_sb.sb_agblocks)))
				do_warn(
	_("invalid owner in rmap btree record %d (%"PRId64" %u) block %u/%u\n"),
						i, owner, len, agno, bno);

			/* Look for impossible record field combinations. */
			if (XFS_RMAP_NON_INODE_OWNER(key.rm_owner)) {
				if (key.rm_flags)
					do_warn(
	_("record %d of block (%u/%u) in %s btree cannot have non-inode owner with flags\n"),
						i, agno, bno, name);
				if (key.rm_offset)
					do_warn(
	_("record %d of block (%u/%u) in %s btree cannot have non-inode owner with offset\n"),
						i, agno, bno, name);
			}

			/* Check for out of order records. */
			if (i == 0) {
advance:
				laststartblock = b;
				lastblock = end - 1;
				lastowner = owner;
				lastoffset = offset;
			} else {
				bool bad;

				if (xfs_has_reflink(mp))
					bad = !rmap_in_order(b, laststartblock,
							owner, lastowner,
							offset, lastoffset);
				else
					bad = b <= lastblock;
				if (bad)
					do_warn(
	_("out-of-order rmap btree record %d (%u %"PRId64" %"PRIx64" %u) block %u/%u\n"),
					i, b, owner, offset, len, agno, bno);
				else
					goto advance;
			}

			/* Is this mergeable with the previous record? */
			if (rmaps_are_mergeable(&rmap_priv->last_rec, &key)) {
				do_warn(
	_("record %d in block (%u/%u) of %s tree should be merged with previous record\n"),
					i, agno, bno, name);
				rmap_priv->last_rec.rm_blockcount +=
						key.rm_blockcount;
			} else
				rmap_priv->last_rec = key;

			/* Check that we don't go past the high key. */
			key.rm_startblock += key.rm_blockcount - 1;
			if (!XFS_RMAP_NON_INODE_OWNER(key.rm_owner) &&
			    !(key.rm_flags & XFS_RMAP_BMBT_BLOCK))
				key.rm_offset += key.rm_blockcount - 1;
			key.rm_blockcount = 0;
			if (rmap_diffkeys(&key, &rmap_priv->high_key) > 0) {
				do_warn(
	_("record %d greater than high key of block (%u/%u) in %s tree\n"),
					i, agno, bno, name);
			}

			/* Check for block owner collisions. */
			for ( ; b < end; b += blen)  {
				state = get_bmap_ext(agno, b, end, &blen,
						false);
				process_rmap_rec(mp, agno, b, end, blen, owner,
						state, name);
			}
		}
		goto out;
	}

	/*
	 * interior record
	 */
	pp = XFS_RMAP_PTR_ADDR(block, 1, mp->m_rmap_mxr[1]);

	if (numrecs > mp->m_rmap_mxr[1])  {
		numrecs = mp->m_rmap_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < mp->m_rmap_mnr[1])  {
		numrecs = mp->m_rmap_mnr[1];
		hdr_errors++;
	}

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */
	if (hdr_errors)  {
		do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
			be16_to_cpu(block->bb_numrecs),
			mp->m_rmap_mnr[1], mp->m_rmap_mxr[1],
			name, agno, bno);
		if (suspect)
			goto out;
		suspect++;
	} else if (suspect) {
		suspect = 0;
	}

	/* check the node's high keys */
	for (i = 0; i < numrecs; i++) {
		kp = XFS_RMAP_HIGH_KEY_ADDR(block, i + 1);

		key.rm_flags = 0;
		key.rm_startblock = be32_to_cpu(kp->rm_startblock);
		key.rm_owner = be64_to_cpu(kp->rm_owner);
		if (kp->rm_offset & cpu_to_be64(XFS_RMAP_OFF_UNWRITTEN))
			warn_rmap_unwritten_key(agno);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&key)) {
			/* Look for impossible flags. */
			do_warn(
	_("invalid flags in key %u of %s btree block %u/%u\n"),
				i, name, agno, bno);
			continue;
		}
		if (rmap_diffkeys(&key, &rmap_priv->high_key) > 0)
			do_warn(
	_("key %d greater than high key of block (%u/%u) in %s tree\n"),
				i, agno, bno, name);
	}

	/* check for in-order keys */
	for (i = 0; i < numrecs; i++)  {
		kp = XFS_RMAP_KEY_ADDR(block, i + 1);

		key.rm_flags = 0;
		key.rm_startblock = be32_to_cpu(kp->rm_startblock);
		key.rm_owner = be64_to_cpu(kp->rm_owner);
		if (kp->rm_offset & cpu_to_be64(XFS_RMAP_OFF_UNWRITTEN))
			warn_rmap_unwritten_key(agno);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in key %u of %s btree block %u/%u\n"),
				i, name, agno, bno);
			suspect++;
			continue;
		}
		if (i == 0) {
			oldkey = key;
			continue;
		}
		if (rmap_diffkeys(&oldkey, &key) > 0) {
			do_warn(
_("out of order key %u in %s btree block (%u/%u)\n"),
				i, name, agno, bno);
			suspect++;
		}
		oldkey = key;
	}

	pag = libxfs_perag_get(mp, agno);
	for (i = 0; i < numrecs; i++)  {
		xfs_agblock_t		agbno = be32_to_cpu(pp[i]);

		/*
		 * XXX - put sibling detection right here.
		 * we know our sibling chain is good.  So as we go,
		 * we check the entry before and after each entry.
		 * If either of the entries references a different block,
		 * check the sibling pointer.  If there's a sibling
		 * pointer mismatch, try and extract as much data
		 * as possible.
		 */
		kp = XFS_RMAP_HIGH_KEY_ADDR(block, i + 1);
		rmap_priv->high_key.rm_flags = 0;
		rmap_priv->high_key.rm_startblock =
				be32_to_cpu(kp->rm_startblock);
		rmap_priv->high_key.rm_owner =
				be64_to_cpu(kp->rm_owner);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&rmap_priv->high_key)) {
			/* Look for impossible flags. */
			do_warn(
	_("invalid flags in high key %u of %s btree block %u/%u\n"),
				i, name, agno, agbno);
			continue;
		}

		if (!libxfs_verify_agbno(pag, agbno)) {
			do_warn(
	_("bad btree pointer (%u) in %sbt block %u/%u\n"),
				agbno, name, agno, bno);
			suspect++;
			libxfs_perag_put(pag);
			return;
		}

		scan_sbtree(agbno, level, agno, suspect, scan_rmapbt, 0, magic,
				priv, ops);
	}
	libxfs_perag_put(pag);

out:
	if (suspect)
		rmap_avoid_check(mp);
}

int
process_rtrmap_reclist(
	struct xfs_mount	*mp,
	struct xfs_rmap_rec	*rp,
	int			numrecs,
	struct xfs_rmap_irec	*last_rec,
	struct xfs_rmap_irec	*high_key,
	const char		*name)
{
	int			suspect = 0;
	int			i;
	struct xfs_rmap_irec	oldkey;
	struct xfs_rmap_irec	key;

	for (i = 0; i < numrecs; i++) {
		xfs_rgblock_t		b, end;
		xfs_extlen_t		len;
		uint64_t		owner, offset;

		b = be32_to_cpu(rp[i].rm_startblock);
		len = be32_to_cpu(rp[i].rm_blockcount);
		owner = be64_to_cpu(rp[i].rm_owner);
		offset = be64_to_cpu(rp[i].rm_offset);

		key.rm_flags = 0;
		key.rm_startblock = b;
		key.rm_blockcount = len;
		key.rm_owner = owner;
		if (libxfs_rmap_irec_offset_unpack(offset, &key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in record %u of %s\n"),
				i, name);
			suspect++;
			continue;
		}


		end = key.rm_startblock + key.rm_blockcount;

		/* Make sure startblock & len make sense. */
		if (b >= mp->m_groups[XG_TYPE_RTG].blocks) {
			do_warn(
_("invalid start block %llu in record %u of %s\n"),
				(unsigned long long)b, i, name);
			suspect++;
			continue;
		}
		if (len == 0 || end - 1 >= mp->m_groups[XG_TYPE_RTG].blocks) {
			do_warn(
_("invalid length %llu in record %u of %s\n"),
				(unsigned long long)len, i, name);
			suspect++;
			continue;
		}

		/*
		 * We only store file data, COW data, and superblocks in the
		 * rtrmap.
		 */
		if (owner == XFS_RMAP_OWN_COW) {
			if (!xfs_has_reflink(mp)) {
				do_warn(
_("invalid CoW staging extent in record %u of %s\n"),
						i, name);
				suspect++;
				continue;
			}
		} else if (XFS_RMAP_NON_INODE_OWNER(owner) &&
			   owner != XFS_RMAP_OWN_FS) {
			do_warn(
_("invalid owner %lld in record %u of %s\n"),
				(long long int)owner, i, name);
			suspect++;
			continue;
		}

		/* Look for impossible record field combinations. */
		if (key.rm_flags & XFS_RMAP_KEY_FLAGS) {
			do_warn(
_("record %d cannot have attr fork/key flags in %s\n"),
					i, name);
			suspect++;
			continue;
		}

		/* Check for out of order records. */
		if (i == 0)
			oldkey = key;
		else {
			if (rmap_diffkeys(&oldkey, &key) > 0)
				do_warn(
_("out-of-order record %d (%llu %"PRId64" %"PRIu64" %llu) in %s\n"),
				i, (unsigned long long)b, owner, offset,
				(unsigned long long)len, name);
			else
				oldkey = key;
		}

		/* Is this mergeable with the previous record? */
		if (rmaps_are_mergeable(last_rec, &key)) {
			do_warn(
_("record %d in %s should be merged with previous record\n"),
				i, name);
			last_rec->rm_blockcount += key.rm_blockcount;
		} else
			*last_rec = key;

		/* Check that we don't go past the high key. */
		key.rm_startblock += key.rm_blockcount - 1;
		key.rm_offset += key.rm_blockcount - 1;
		key.rm_blockcount = 0;
		if (high_key && rmap_diffkeys(&key, high_key) > 0) {
			do_warn(
_("record %d greater than high key of %s\n"),
				i, name);
			suspect++;
		}
	}

	return suspect;
}

int
scan_rtrmapbt(
	struct xfs_btree_block	*block,
	int			level,
	int			type,
	int			whichfork,
	xfs_fsblock_t		fsbno,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	uint64_t		*nex,
	blkmap_t		**blkmapp,
	bmap_cursor_t		*bm_cursor,
	int			suspect,
	int			isroot,
	int			check_dups,
	int			*dirty,
	uint64_t		magic,
	void			*priv)
{
	const char		*name = "rtrmap";
	char			rootname[256];
	int			i;
	xfs_rtrmap_ptr_t	*pp;
	struct xfs_rmap_rec	*rp;
	struct rmap_priv	*rmap_priv = priv;
	int			hdr_errors = 0;
	int			numrecs;
	int			state;
	struct xfs_rmap_key	*kp;
	struct xfs_rmap_irec	oldkey;
	struct xfs_rmap_irec	key;
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno;
	int			error;

	agno = XFS_FSB_TO_AGNO(mp, fsbno);
	agbno = XFS_FSB_TO_AGBNO(mp, fsbno);

	/* If anything here is bad, just bail. */
	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(
_("bad magic # %#x in inode %" PRIu64 " %s block %" PRIu64 "\n"),
			be32_to_cpu(block->bb_magic), ino, name, fsbno);
		return 1;
	}
	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(
_("expected level %d got %d in inode %" PRIu64 ", %s block %" PRIu64 "\n"),
			level, be16_to_cpu(block->bb_level),
			ino, name, fsbno);
		return(1);
	}

	/* verify owner */
	if (be64_to_cpu(block->bb_u.l.bb_owner) != ino) {
		do_warn(
_("expected owner inode %" PRIu64 ", got %llu, %s block %" PRIu64 "\n"),
			ino,
			(unsigned long long)be64_to_cpu(block->bb_u.l.bb_owner),
			name, fsbno);
		return 1;
	}
	/* verify block number */
	if (be64_to_cpu(block->bb_u.l.bb_blkno) !=
	    XFS_FSB_TO_DADDR(mp, fsbno)) {
		do_warn(
_("expected block %" PRIu64 ", got %llu, %s block %" PRIu64 "\n"),
			XFS_FSB_TO_DADDR(mp, fsbno),
			(unsigned long long)be64_to_cpu(block->bb_u.l.bb_blkno),
			name, fsbno);
		return 1;
	}
	/* verify uuid */
	if (platform_uuid_compare(&block->bb_u.l.bb_uuid,
				  &mp->m_sb.sb_meta_uuid) != 0) {
		do_warn(
_("wrong FS UUID, %s block %" PRIu64 "\n"),
			name, fsbno);
		return 1;
	}

	/*
	 * Check for btree blocks multiply claimed.  We're going to regenerate
	 * the rtrmap anyway, so mark the blocks as metadata so they get freed.
	 */
	state = get_bmap(agno, agbno);
	if (!(state == XR_E_UNKNOWN || state == XR_E_INUSE1))  {
		do_warn(
_("%s btree block claimed (state %d), agno %d, bno %d, suspect %d\n"),
				name, state, agno, agbno, suspect);
		suspect++;
		goto out;
	}
	set_bmap(agno, agbno, XR_E_METADATA);

	numrecs = be16_to_cpu(block->bb_numrecs);

	/*
	 * All realtime rmap btree blocks are freed for a fully empty
	 * filesystem, thus they are counted towards the free data
	 * block counter.  The root lives in an inode and is thus not
	 * counted.
	 */
	(*tot)++;

	if (level == 0) {
		if (numrecs > mp->m_rtrmap_mxr[0])  {
			numrecs = mp->m_rtrmap_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < mp->m_rtrmap_mnr[0])  {
			numrecs = mp->m_rtrmap_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors) {
			do_warn(
_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
				be16_to_cpu(block->bb_numrecs),
				mp->m_rtrmap_mnr[0], mp->m_rtrmap_mxr[0],
				name, agno, agbno);
			suspect++;
		}

		rp = xfs_rtrmap_rec_addr(block, 1);
		snprintf(rootname, 256, "%s btree block %u/%u", name, agno, agbno);
		error = process_rtrmap_reclist(mp, rp, numrecs,
				&rmap_priv->last_rec, &rmap_priv->high_key,
				rootname);
		if (error)
			suspect++;
		goto out;
	}

	/*
	 * interior record
	 */
	pp = xfs_rtrmap_ptr_addr(block, 1, mp->m_rtrmap_mxr[1]);

	if (numrecs > mp->m_rtrmap_mxr[1])  {
		numrecs = mp->m_rtrmap_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < mp->m_rtrmap_mnr[1])  {
		numrecs = mp->m_rtrmap_mnr[1];
		hdr_errors++;
	}

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */
	if (hdr_errors)  {
		do_warn(
_("bad btree nrecs (%u, min=%u, max=%u) in bt%s block %u/%u\n"),
			be16_to_cpu(block->bb_numrecs),
			mp->m_rtrmap_mnr[1], mp->m_rtrmap_mxr[1],
			name, agno, agbno);
		if (suspect)
			goto out;
		suspect++;
	} else if (suspect) {
		suspect = 0;
	}

	/* check the node's high keys */
	for (i = 0; !isroot && i < numrecs; i++) {
		kp = xfs_rtrmap_high_key_addr(block, i + 1);

		key.rm_flags = 0;
		key.rm_startblock = be32_to_cpu(kp->rm_startblock);
		key.rm_owner = be64_to_cpu(kp->rm_owner);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in key %u of %s btree block %u/%u\n"),
				i, name, agno, agbno);
			suspect++;
			continue;
		}
		if (rmap_diffkeys(&key, &rmap_priv->high_key) > 0) {
			do_warn(
_("key %d greater than high key of block (%u/%u) in %s tree\n"),
				i, agno, agbno, name);
			suspect++;
		}
	}

	/* check for in-order keys */
	for (i = 0; i < numrecs; i++)  {
		kp = xfs_rtrmap_key_addr(block, i + 1);

		key.rm_flags = 0;
		key.rm_startblock = be32_to_cpu(kp->rm_startblock);
		key.rm_owner = be64_to_cpu(kp->rm_owner);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in key %u of %s btree block %u/%u\n"),
				i, name, agno, agbno);
			suspect++;
			continue;
		}
		if (i == 0) {
			oldkey = key;
			continue;
		}
		if (rmap_diffkeys(&oldkey, &key) > 0) {
			do_warn(
_("out of order key %u in %s btree block (%u/%u)\n"),
				i, name, agno, agbno);
			suspect++;
		}
		oldkey = key;
	}

	for (i = 0; i < numrecs; i++)  {
		xfs_fsblock_t		pbno = be64_to_cpu(pp[i]);

		/*
		 * XXX - put sibling detection right here.
		 * we know our sibling chain is good.  So as we go,
		 * we check the entry before and after each entry.
		 * If either of the entries references a different block,
		 * check the sibling pointer.  If there's a sibling
		 * pointer mismatch, try and extract as much data
		 * as possible.
		 */
		kp = xfs_rtrmap_high_key_addr(block, i + 1);
		rmap_priv->high_key.rm_flags = 0;
		rmap_priv->high_key.rm_startblock =
				be32_to_cpu(kp->rm_startblock);
		rmap_priv->high_key.rm_owner =
				be64_to_cpu(kp->rm_owner);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&rmap_priv->high_key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in high key %u of %s btree block %u/%u\n"),
				i, name, agno, agbno);
			suspect++;
			continue;
		}

		if (!libxfs_verify_fsbno(mp, pbno)) {
			do_warn(
_("bad %s btree ptr 0x%llx in ino %" PRIu64 "\n"),
			       name, (unsigned long long)pbno, ino);
			return 1;
		}

		error = scan_lbtree(pbno, level, scan_rtrmapbt,
				type, whichfork, ino, tot, nex, blkmapp,
				bm_cursor, suspect, 0, check_dups, magic,
				rmap_priv, &xfs_rtrmapbt_buf_ops);
		if (error) {
			suspect++;
			goto out;
		}
	}

out:
	if (hdr_errors || suspect) {
		rmap_avoid_check(mp);
		return 1;
	}
	return 0;
}

static void
scan_refcbt(
	struct xfs_btree_block	*block,
	int			level,
	xfs_agblock_t		bno,
	xfs_agnumber_t		agno,
	int			suspect,
	int			isroot,
	uint32_t		magic,
	void			*priv,
	const struct xfs_buf_ops *ops)
{
	const char		*name = "refcount";
	int			i;
	xfs_refcount_ptr_t	*pp;
	struct xfs_refcount_rec	*rp;
	int			hdr_errors = 0;
	int			numrecs;
	int			state;
	xfs_agblock_t		lastblock = 0;
	struct refc_priv	*refc_priv = priv;
	struct xfs_perag	*pag;

	if (magic != XFS_REFC_CRC_MAGIC) {
		name = "(unknown)";
		hdr_errors++;
		suspect++;
		goto out;
	}

	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(_("bad magic # %#x in %s btree block %d/%d\n"),
			be32_to_cpu(block->bb_magic), name, agno, bno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(_("expected level %d got %d in %s btree block %d/%d\n"),
			level, be16_to_cpu(block->bb_level), name, agno, bno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	refc_priv->nr_blocks++;

	/* check for btree blocks multiply claimed */
	state = get_bmap(agno, bno);
	if (!(state == XR_E_UNKNOWN || state == XR_E_REFC))  {
		set_bmap(agno, bno, XR_E_MULT);
		do_warn(
_("%s btree block claimed (state %d), agno %d, bno %d, suspect %d\n"),
				name, state, agno, bno, suspect);
		goto out;
	}
	set_bmap(agno, bno, XR_E_FS_MAP);

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (level == 0) {
		if (numrecs > mp->m_refc_mxr[0])  {
			numrecs = mp->m_refc_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < mp->m_refc_mnr[0])  {
			numrecs = mp->m_refc_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors) {
			do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in %s btree block %u/%u\n"),
				be16_to_cpu(block->bb_numrecs),
				mp->m_refc_mnr[0], mp->m_refc_mxr[0],
				name, agno, bno);
			suspect++;
		}

		rp = XFS_REFCOUNT_REC_ADDR(block, 1);
		pag = libxfs_perag_get(mp, agno);

		for (i = 0; i < numrecs; i++) {
			enum xfs_refc_domain	domain;
			xfs_agblock_t		b, agb, end;
			xfs_extlen_t		len;
			xfs_nlink_t		nr;

			b = agb = be32_to_cpu(rp[i].rc_startblock);
			len = be32_to_cpu(rp[i].rc_blockcount);
			nr = be32_to_cpu(rp[i].rc_refcount);

			if (b & XFS_REFC_COWFLAG) {
				domain = XFS_REFC_DOMAIN_COW;
				agb &= ~XFS_REFC_COWFLAG;
			} else {
				domain = XFS_REFC_DOMAIN_SHARED;
			}

			if (domain == XFS_REFC_DOMAIN_COW && nr != 1)
				do_warn(
_("leftover CoW extent has incorrect refcount in record %u of %s btree block %u/%u\n"),
					i, name, agno, bno);
			if (nr == 1) {
				if (domain != XFS_REFC_DOMAIN_COW)
					do_warn(
_("leftover CoW extent has invalid startblock in record %u of %s btree block %u/%u\n"),
						i, name, agno, bno);
			}
			end = agb + len;

			if (!libxfs_verify_agbno(pag, agb)) {
				do_warn(
	_("invalid start block %u in record %u of %s btree block %u/%u\n"),
					b, i, name, agno, bno);
				continue;
			}
			if (len == 0 || end <= agb ||
			    !libxfs_verify_agbno(pag, end - 1)) {
				do_warn(
	_("invalid length %u in record %u of %s btree block %u/%u\n"),
					len, i, name, agno, bno);
				continue;
			}

			if (nr == 1) {
				xfs_agblock_t	c;
				xfs_extlen_t	cnr;

				for (c = agb; c < end; c += cnr) {
					state = get_bmap_ext(agno, c, end, &cnr,
							false);
					switch (state) {
					case XR_E_UNKNOWN:
					case XR_E_COW:
						do_warn(
_("leftover CoW extent (%u/%u) len %u\n"),
						agno, c, cnr);
						set_bmap_ext(agno, c, cnr,
							XR_E_FREE, false);
						break;
					default:
						do_warn(
_("extent (%u/%u) len %u claimed, state is %d\n"),
						agno, c, cnr, state);
						break;
					}
				}
			} else if (nr < 2 || nr > XFS_REFC_REFCOUNT_MAX) {
				do_warn(
	_("invalid reference count %u in record %u of %s btree block %u/%u\n"),
					nr, i, name, agno, bno);
				continue;
			}

			if (b && b <= lastblock) {
				do_warn(_(
	"out-of-order %s btree record %d (%u %u) block %u/%u\n"),
					name, i, b, len, agno, bno);
			} else {
				lastblock = end - 1;
			}

			/* Is this record mergeable with the last one? */
			if (refc_priv->last_rec.rc_domain == domain &&
			    refc_priv->last_rec.rc_startblock +
			    refc_priv->last_rec.rc_blockcount == agb &&
			    refc_priv->last_rec.rc_refcount == nr) {
				do_warn(
	_("record %d in block (%u/%u) of %s tree should be merged with previous record\n"),
					i, agno, bno, name);
				refc_priv->last_rec.rc_blockcount += len;
			} else {
				refc_priv->last_rec.rc_domain = domain;
				refc_priv->last_rec.rc_startblock = agb;
				refc_priv->last_rec.rc_blockcount = len;
				refc_priv->last_rec.rc_refcount = nr;
			}

			/* XXX: probably want to mark the reflinked areas? */
		}
		libxfs_perag_put(pag);
		goto out;
	}

	/*
	 * interior record
	 */
	pp = XFS_REFCOUNT_PTR_ADDR(block, 1, mp->m_refc_mxr[1]);

	if (numrecs > mp->m_refc_mxr[1])  {
		numrecs = mp->m_refc_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < mp->m_refc_mnr[1])  {
		numrecs = mp->m_refc_mnr[1];
		hdr_errors++;
	}

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */
	if (hdr_errors)  {
		do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in %s btree block %u/%u\n"),
			be16_to_cpu(block->bb_numrecs),
			mp->m_refc_mnr[1], mp->m_refc_mxr[1],
			name, agno, bno);
		if (suspect)
			goto out;
		suspect++;
	} else if (suspect) {
		suspect = 0;
	}

	pag = libxfs_perag_get(mp, agno);
	for (i = 0; i < numrecs; i++)  {
		xfs_agblock_t		agbno = be32_to_cpu(pp[i]);

		if (!libxfs_verify_agbno(pag, agbno)) {
			do_warn(
	_("bad btree pointer (%u) in %sbt block %u/%u\n"),
				agbno, name, agno, bno);
			suspect++;
			libxfs_perag_put(pag);
			return;
		}

		scan_sbtree(agbno, level, agno, suspect, scan_refcbt, 0, magic,
				priv, ops);
	}
	libxfs_perag_put(pag);
out:
	if (suspect)
		refcount_avoid_check(mp);
	return;
}


int
process_rtrefc_reclist(
	struct xfs_mount	*mp,
	struct xfs_refcount_rec	*rp,
	int			numrecs,
	struct refc_priv	*refc_priv,
	const char		*name)
{
	struct xfs_rtgroup	*rtg;
	xfs_rgnumber_t		rgno = refc_priv->rgno;
	xfs_rtblock_t		lastblock = 0;
	int			state;
	int			suspect = 0;
	int			i;

	rtg = libxfs_rtgroup_get(mp, rgno);
	if (!rtg) {
		if (numrecs) {
			do_warn(
_("no rt group 0x%x but %d rtrefcount records\n"),
					rgno, numrecs);
			suspect++;
		}

		return suspect;
	}

	for (i = 0; i < numrecs; i++) {
		enum xfs_refc_domain	domain;
		xfs_rgblock_t		b, rgbno, end;
		xfs_extlen_t		len;
		xfs_nlink_t		nr;

		b = rgbno = be32_to_cpu(rp[i].rc_startblock);
		len = be32_to_cpu(rp[i].rc_blockcount);
		nr = be32_to_cpu(rp[i].rc_refcount);

		if (b & XFS_REFC_COWFLAG) {
			domain = XFS_REFC_DOMAIN_COW;
			rgbno &= ~XFS_REFC_COWFLAG;
		} else {
			domain = XFS_REFC_DOMAIN_SHARED;
		}

		if (domain == XFS_REFC_DOMAIN_COW && nr != 1) {
			do_warn(
_("leftover rt CoW extent has incorrect refcount in record %u of %s\n"),
					i, name);
			suspect++;
		}
		if (nr == 1) {
			if (domain != XFS_REFC_DOMAIN_COW) {
				do_warn(
_("leftover rt CoW extent has invalid startblock in record %u of %s\n"),
					i, name);
				suspect++;
			}
		}
		end = rgbno + len;

		if (!libxfs_verify_rgbno(rtg, rgbno)) {
			do_warn(
_("invalid start block %llu in record %u of %s\n"),
					(unsigned long long)b, i, name);
			suspect++;
			continue;
		}

		if (len == 0 || end <= rgbno ||
		    !libxfs_verify_rgbno(rtg, end - 1)) {
			do_warn(
_("invalid length %llu in record %u of %s\n"),
					(unsigned long long)len, i, name);
			suspect++;
			continue;
		}

		if (nr < 2 || nr > XFS_REFC_REFCOUNT_MAX) {
			do_warn(
_("invalid rt reference count %u in record %u of %s\n"),
					nr, i, name);
			suspect++;
			continue;
		}

		if (nr == 1) {
			xfs_rgblock_t		b;
			xfs_extlen_t		blen;

			for (b = rgbno; b < end; b += len) {
				state = get_bmap_ext(rgno, b, end, &blen, true);
				blen = min(blen, len);

				switch (state) {
				case XR_E_UNKNOWN:
				case XR_E_COW:
					do_warn(
_("leftover rt CoW rtextent (%llu)\n"),
						(unsigned long long)rgbno);
					set_bmap_ext(rgno, b, len, XR_E_FREE,
							true);
					break;
				default:
					do_warn(
_("rtextent (%llu) claimed, state is %d\n"),
						(unsigned long long)rgbno, state);
					break;
				}
				suspect++;
			}
		}

		if (b && b <= lastblock) {
			do_warn(_(
"out-of-order %s btree record %d (%llu %llu) in %s\n"),
					name, i, (unsigned long long)b,
					(unsigned long long)len, name);
			suspect++;
		} else {
			lastblock = end - 1;
		}

		/* Is this record mergeable with the last one? */
		if (refc_priv->last_rec.rc_domain == domain &&
		    refc_priv->last_rec.rc_startblock +
		    refc_priv->last_rec.rc_blockcount == rgbno &&
		    refc_priv->last_rec.rc_refcount == nr) {
			do_warn(
_("record %d of %s tree should be merged with previous record\n"),
					i, name);
			suspect++;
			refc_priv->last_rec.rc_blockcount += len;
		} else {
			refc_priv->last_rec.rc_domain = domain;
			refc_priv->last_rec.rc_startblock = rgbno;
			refc_priv->last_rec.rc_blockcount = len;
			refc_priv->last_rec.rc_refcount = nr;
		}

		/* XXX: probably want to mark the reflinked areas? */
	}

	libxfs_rtgroup_put(rtg);
	return suspect;
}

int
scan_rtrefcbt(
	struct xfs_btree_block		*block,
	int				level,
	int				type,
	int				whichfork,
	xfs_fsblock_t			fsbno,
	xfs_ino_t			ino,
	xfs_rfsblock_t			*tot,
	uint64_t			*nex,
	struct blkmap			**blkmapp,
	bmap_cursor_t			*bm_cursor,
	int				suspect,
	int				isroot,
	int				check_dups,
	int				*dirty,
	uint64_t			magic,
	void				*priv)
{
	const char			*name = "rtrefcount";
	char				rootname[256];
	int				i;
	xfs_rtrefcount_ptr_t		*pp;
	struct xfs_refcount_rec	*rp;
	struct refc_priv		*refc_priv = priv;
	int				hdr_errors = 0;
	int				numrecs;
	int				state;
	xfs_agnumber_t			agno;
	xfs_agblock_t			agbno;
	int				error;

	agno = XFS_FSB_TO_AGNO(mp, fsbno);
	agbno = XFS_FSB_TO_AGBNO(mp, fsbno);

	if (magic != XFS_RTREFC_CRC_MAGIC) {
		name = "(unknown)";
		hdr_errors++;
		suspect++;
		goto out;
	}

	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(_("bad magic # %#x in %s btree block %d/%d\n"),
				be32_to_cpu(block->bb_magic), name, agno,
				agbno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(_("expected level %d got %d in %s btree block %d/%d\n"),
				level, be16_to_cpu(block->bb_level), name,
				agno, agbno);
		hdr_errors++;
		if (suspect)
			goto out;
	}

	refc_priv->nr_blocks++;

	/*
	 * Check for btree blocks multiply claimed.  We're going to regenerate
	 * the btree anyway, so mark the blocks as metadata so they get freed.
	 */
	state = get_bmap(agno, agbno);
	if (!(state == XR_E_UNKNOWN || state == XR_E_INUSE1))  {
		do_warn(
_("%s btree block claimed (state %d), agno %d, agbno %d, suspect %d\n"),
				name, state, agno, agbno, suspect);
		goto out;
	}
	set_bmap(agno, agbno, XR_E_METADATA);

	numrecs = be16_to_cpu(block->bb_numrecs);
	if (level == 0) {
		if (numrecs > mp->m_rtrefc_mxr[0])  {
			numrecs = mp->m_rtrefc_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < mp->m_rtrefc_mnr[0])  {
			numrecs = mp->m_rtrefc_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors) {
			do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in %s btree block %u/%u\n"),
					be16_to_cpu(block->bb_numrecs),
					mp->m_rtrefc_mnr[0],
					mp->m_rtrefc_mxr[0], name, agno, agbno);
			suspect++;
		}

		rp = xfs_rtrefcount_rec_addr(block, 1);
		snprintf(rootname, 256, "%s btree block %u/%u", name, agno,
				agbno);
		error = process_rtrefc_reclist(mp, rp, numrecs, refc_priv,
				rootname);
		if (error)
			suspect++;
		goto out;
	}

	/*
	 * interior record
	 */
	pp = xfs_rtrefcount_ptr_addr(block, 1, mp->m_rtrefc_mxr[1]);

	if (numrecs > mp->m_rtrefc_mxr[1])  {
		numrecs = mp->m_rtrefc_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < mp->m_rtrefc_mnr[1])  {
		numrecs = mp->m_rtrefc_mnr[1];
		hdr_errors++;
	}

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */
	if (hdr_errors)  {
		do_warn(
	_("bad btree nrecs (%u, min=%u, max=%u) in %s btree block %u/%u\n"),
				be16_to_cpu(block->bb_numrecs),
				mp->m_rtrefc_mnr[1], mp->m_rtrefc_mxr[1], name,
				agno, agbno);
		if (suspect)
			goto out;
		suspect++;
	} else if (suspect) {
		suspect = 0;
	}

	for (i = 0; i < numrecs; i++)  {
		xfs_fsblock_t		pbno = be64_to_cpu(pp[i]);

		if (!libxfs_verify_fsbno(mp, pbno)) {
			do_warn(
	_("bad btree pointer (%u) in %sbt block %u/%u\n"),
					agbno, name, agno, agbno);
			suspect++;
			return 0;
		}

		scan_lbtree(pbno, level, scan_rtrefcbt, type, whichfork, ino,
				tot, nex, blkmapp, bm_cursor, suspect, 0,
				check_dups, magic, refc_priv,
				&xfs_rtrefcountbt_buf_ops);
	}
out:
	if (suspect) {
		refcount_avoid_check(mp);
		return 1;
	}

	return 0;
}

/*
 * The following helpers are to help process and validate individual on-disk
 * inode btree records. We have two possible inode btrees with slightly
 * different semantics. Many of the validations and actions are equivalent, such
 * as record alignment constraints, etc. Other validations differ, such as the
 * fact that the inode chunk block allocation state is set by the content of the
 * core inobt and verified by the content of the finobt.
 *
 * The following structures are used to facilitate common validation routines
 * where the only difference between validation of the inobt or finobt might be
 * the error messages that results in the event of failure.
 */

enum inobt_type {
	INOBT,
	FINOBT
};
static const char *inobt_names[] = {
	"inobt",
	"finobt"
};

static int
verify_single_ino_chunk_align(
	xfs_agnumber_t		agno,
	enum inobt_type		type,
	struct xfs_inobt_rec	*rp,
	int			suspect,
	bool			*skip)
{
	const char		*inobt_name = inobt_names[type];
	xfs_ino_t		lino;
	xfs_agino_t		ino;
	xfs_agblock_t		agbno;
	int			off;
	struct xfs_perag	*pag;

	*skip = false;
	ino = be32_to_cpu(rp->ir_startino);
	off = XFS_AGINO_TO_OFFSET(mp, ino);
	agbno = XFS_AGINO_TO_AGBNO(mp, ino);
	lino = XFS_AGINO_TO_INO(mp, agno, ino);

	/*
	 * on multi-block block chunks, all chunks start at the beginning of the
	 * block. with multi-chunk blocks, all chunks must start on 64-inode
	 * boundaries since each block can hold N complete chunks. if fs has
	 * aligned inodes, all chunks must start at a fs_ino_alignment*N'th
	 * agbno. skip recs with badly aligned starting inodes.
	 */
	if (ino == 0 ||
	    (inodes_per_block <= XFS_INODES_PER_CHUNK && off !=  0) ||
	    (inodes_per_block > XFS_INODES_PER_CHUNK &&
	     off % XFS_INODES_PER_CHUNK != 0) ||
	    (fs_aligned_inodes && fs_ino_alignment &&
	     agbno % fs_ino_alignment != 0)) {
		do_warn(
	_("badly aligned %s rec (starting inode = %" PRIu64 ")\n"),
			inobt_name, lino);
		suspect++;
	}

	/*
	 * verify numeric validity of inode chunk first before inserting into a
	 * tree. don't have to worry about the overflow case because the
	 * starting ino number of a chunk can only get within 255 inodes of max
	 * (NULLAGINO). if it gets closer, the agino number will be illegal as
	 * the agbno will be too large.
	 */
	pag = libxfs_perag_get(mp, agno);
	if (!libxfs_verify_agino(pag, ino)) {
		do_warn(
_("bad starting inode # (%" PRIu64 " (0x%x 0x%x)) in %s rec, skipping rec\n"),
			lino, agno, ino, inobt_name);
		*skip = true;
		libxfs_perag_put(pag);
		return ++suspect;
	}

	if (!libxfs_verify_agino(pag, ino + XFS_INODES_PER_CHUNK - 1)) {
		do_warn(
_("bad ending inode # (%" PRIu64 " (0x%x 0x%zx)) in %s rec, skipping rec\n"),
			lino + XFS_INODES_PER_CHUNK - 1,
			agno,
			ino + XFS_INODES_PER_CHUNK - 1,
			inobt_name);
		*skip = true;
		libxfs_perag_put(pag);
		return ++suspect;
	}

	libxfs_perag_put(pag);
	return suspect;
}

/*
 * Process the state of individual inodes in an on-disk inobt record and import
 * into the appropriate in-core tree based on whether the on-disk tree is
 * suspect. Return the total and free inode counts based on the record free and
 * hole masks.
 */
static int
import_single_ino_chunk(
	xfs_agnumber_t		agno,
	enum inobt_type		type,
	struct xfs_inobt_rec	*rp,
	int			suspect,
	int			*p_nfree,
	int			*p_ninodes)
{
	struct ino_tree_node	*ino_rec = NULL;
	const char		*inobt_name = inobt_names[type];
	xfs_agino_t		ino;
	int			j;
	int			nfree;
	int			ninodes;

	ino = be32_to_cpu(rp->ir_startino);

	if (!suspect) {
		if (XFS_INOBT_IS_FREE_DISK(rp, 0))
			ino_rec = set_inode_free_alloc(mp, agno, ino);
		else
			ino_rec = set_inode_used_alloc(mp, agno, ino);
		for (j = 1; j < XFS_INODES_PER_CHUNK; j++) {
			if (XFS_INOBT_IS_FREE_DISK(rp, j))
				set_inode_free(ino_rec, j);
			else
				set_inode_used(ino_rec, j);
		}
	} else {
		for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
			if (XFS_INOBT_IS_FREE_DISK(rp, j))
				add_aginode_uncertain(mp, agno, ino + j, 1);
			else
				add_aginode_uncertain(mp, agno, ino + j, 0);
		}
	}

	/*
	 * Mark sparse inodes as such in the in-core tree. Verify that sparse
	 * inodes are free and that freecount is consistent with the free mask.
	 */
	nfree = ninodes = 0;
	for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
		if (ino_issparse(rp, j)) {
			if (!suspect && !XFS_INOBT_IS_FREE_DISK(rp, j)) {
				do_warn(
_("ir_holemask/ir_free mismatch, %s chunk %d/%u, holemask 0x%x free 0x%llx\n"),
					inobt_name, agno, ino,
					be16_to_cpu(rp->ir_u.sp.ir_holemask),
					(unsigned long long)be64_to_cpu(rp->ir_free));
				suspect++;
			}
			if (!suspect && ino_rec)
				set_inode_sparse(ino_rec, j);
		} else {
			/* count fields track non-sparse inos */
			if (XFS_INOBT_IS_FREE_DISK(rp, j))
				nfree++;
			ninodes++;
		}
	}

	*p_nfree = nfree;
	*p_ninodes = ninodes;

	return suspect;
}

static int
scan_single_ino_chunk(
	xfs_agnumber_t		agno,
	xfs_inobt_rec_t		*rp,
	int			suspect)
{
	xfs_ino_t		lino;
	xfs_agino_t		ino;
	xfs_agblock_t		agbno;
	int			j;
	int			nfree;
	int			ninodes;
	int			off;
	int			state;
	ino_tree_node_t		*first_rec, *last_rec;
	int			freecount;
	bool			skip = false;

	ino = be32_to_cpu(rp->ir_startino);
	off = XFS_AGINO_TO_OFFSET(mp, ino);
	agbno = XFS_AGINO_TO_AGBNO(mp, ino);
	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	freecount = inorec_get_freecount(mp, rp);

	/*
	 * Verify record alignment, start/end inode numbers, etc.
	 */
	suspect = verify_single_ino_chunk_align(agno, INOBT, rp, suspect,
						&skip);
	if (skip)
		return suspect;

	/*
	 * set state of each block containing inodes
	 */
	if (off == 0 && !suspect)  {
		for (j = 0;
		     j < XFS_INODES_PER_CHUNK;
		     j += mp->m_sb.sb_inopblock)  {

			/* inodes in sparse chunks don't use blocks */
			if (ino_issparse(rp, j))
				continue;

			agbno = XFS_AGINO_TO_AGBNO(mp, ino + j);
			state = get_bmap(agno, agbno);
			switch (state) {
			case XR_E_INO:
				break;
			case XR_E_UNKNOWN:
			case XR_E_INO1:	/* seen by rmap */
				set_bmap(agno, agbno, XR_E_INO);
				break;
			default:
				/* XXX - maybe should mark block a duplicate */
				do_warn(
_("inode chunk claims used block, inobt block - agno %d, bno %d, inopb %d\n"),
					agno, agbno, mp->m_sb.sb_inopblock);
				return ++suspect;
			}
		}
	}

	/*
	 * ensure only one avl entry per chunk
	 */
	find_inode_rec_range(mp, agno, ino, ino + XFS_INODES_PER_CHUNK,
			     &first_rec, &last_rec);
	if (first_rec != NULL)  {
		/*
		 * this chunk overlaps with one (or more)
		 * already in the tree
		 */
		do_warn(
_("inode rec for ino %" PRIu64 " (%d/%d) overlaps existing rec (start %d/%d)\n"),
			lino, agno, ino, agno, first_rec->ino_startnum);
		suspect++;

		/*
		 * if the 2 chunks start at the same place,
		 * then we don't have to put this one
		 * in the uncertain list.  go to the next one.
		 */
		if (first_rec->ino_startnum == ino)
			return suspect;
	}

	/*
	 * Import the state of individual inodes into the appropriate in-core
	 * trees, mark them free or used, and get the resulting total and free
	 * inode counts.
	 */
	nfree = ninodes = 0;
	suspect = import_single_ino_chunk(agno, INOBT, rp, suspect, &nfree,
					 &ninodes);

	if (nfree != freecount) {
		do_warn(
_("ir_freecount/free mismatch, inode chunk %d/%u, freecount %d nfree %d\n"),
			agno, ino, freecount, nfree);
	}

	/* verify sparse record formats have a valid inode count */
	if (xfs_has_sparseinodes(mp) &&
	    ninodes != rp->ir_u.sp.ir_count) {
		do_warn(
_("invalid inode count, inode chunk %d/%u, count %d ninodes %d\n"),
			agno, ino, rp->ir_u.sp.ir_count, ninodes);
	}

	return suspect;
}

static int
scan_single_finobt_chunk(
	xfs_agnumber_t		agno,
	xfs_inobt_rec_t		*rp,
	int			suspect)
{
	xfs_ino_t		lino;
	xfs_agino_t		ino;
	xfs_agblock_t		agbno;
	int			j;
	int			nfree;
	int			ninodes;
	int			off;
	int			state;
	ino_tree_node_t		*first_rec, *last_rec;
	int			freecount;
	bool			skip = false;

	ino = be32_to_cpu(rp->ir_startino);
	off = XFS_AGINO_TO_OFFSET(mp, ino);
	agbno = XFS_AGINO_TO_AGBNO(mp, ino);
	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	freecount = inorec_get_freecount(mp, rp);

	/*
	 * Verify record alignment, start/end inode numbers, etc.
	 */
	suspect = verify_single_ino_chunk_align(agno, FINOBT, rp, suspect,
						&skip);
	if (skip)
		return suspect;

	/*
	 * cross check state of each block containing inodes referenced by the
	 * finobt against what we have already scanned from the alloc inobt.
	 */
	if (off == 0 && !suspect) {
		for (j = 0;
		     j < XFS_INODES_PER_CHUNK;
		     j += mp->m_sb.sb_inopblock) {
			agbno = XFS_AGINO_TO_AGBNO(mp, ino + j);
			state = get_bmap(agno, agbno);

			/* sparse inodes should not refer to inode blocks */
			if (ino_issparse(rp, j)) {
				if (state == XR_E_INO) {
					do_warn(
_("sparse inode chunk claims inode block, finobt block - agno %d, bno %d, inopb %d\n"),
						agno, agbno, mp->m_sb.sb_inopblock);
					suspect++;
				}
				continue;
			}

			switch (state) {
			case XR_E_INO:
				break;
			case XR_E_INO1:	/* seen by rmap */
				set_bmap(agno, agbno, XR_E_INO);
				break;
			case XR_E_UNKNOWN:
				do_warn(
_("inode chunk claims untracked block, finobt block - agno %d, bno %d, inopb %d\n"),
					agno, agbno, mp->m_sb.sb_inopblock);

				set_bmap(agno, agbno, XR_E_INO);
				suspect++;
				break;
			default:
				do_warn(
_("inode chunk claims used block, finobt block - agno %d, bno %d, inopb %d\n"),
					agno, agbno, mp->m_sb.sb_inopblock);
				return ++suspect;
			}
		}
	}

	/*
	 * ensure we have an incore entry for each chunk
	 */
	find_inode_rec_range(mp, agno, ino, ino + XFS_INODES_PER_CHUNK,
			     &first_rec, &last_rec);

	if (first_rec) {
		if (suspect)
			return suspect;

		/*
		 * verify consistency between finobt record and incore state
		 */
		if (first_rec->ino_startnum != ino) {
			do_warn(
_("finobt rec for ino %" PRIu64 " (%d/%u) does not match existing rec (%d/%d)\n"),
				lino, agno, ino, agno, first_rec->ino_startnum);
			return ++suspect;
		}

		nfree = ninodes = 0;
		for (j = 0; j < XFS_INODES_PER_CHUNK; j++) {
			int isfree = XFS_INOBT_IS_FREE_DISK(rp, j);
			int issparse = ino_issparse(rp, j);

			if (!issparse)
				ninodes++;
			if (isfree && !issparse)
				nfree++;

			/*
			 * inode allocation state should be consistent between
			 * the inobt and finobt
			 */
			if (!suspect &&
			    isfree != is_inode_free(first_rec, j))
				suspect++;

			if (!suspect &&
			    issparse != is_inode_sparse(first_rec, j))
				suspect++;
		}

		goto check_freecount;
	}

	/*
	 * The finobt contains a record that the previous inobt scan never
	 * found. Warn about it and import the inodes into the appropriate
	 * trees.
	 *
	 * Note that this should do the right thing if the previous inobt scan
	 * had added these inodes to the uncertain tree. If the finobt is not
	 * suspect, these inodes should supercede the uncertain ones. Otherwise,
	 * the uncertain tree helpers handle the case where uncertain inodes
	 * already exist.
	 */
	do_warn(_("undiscovered finobt record, ino %" PRIu64 " (%d/%u)\n"),
		lino, agno, ino);

	nfree = ninodes = 0;
	suspect = import_single_ino_chunk(agno, FINOBT, rp, suspect, &nfree,
					 &ninodes);

check_freecount:

	/*
	 * Verify that the record freecount matches the actual number of free
	 * inodes counted in the record. Don't increment 'suspect' here, since
	 * we have already verified the allocation state of the individual
	 * inodes against the in-core state. This will have already incremented
	 * 'suspect' if something is wrong. If suspect hasn't been set at this
	 * point, these warnings mean that we have a simple freecount
	 * inconsistency or a stray finobt record (as opposed to a broader tree
	 * corruption). Issue a warning and continue the scan. The final btree
	 * reconstruction will correct this naturally.
	 */
	if (nfree != freecount) {
		do_warn(
_("finobt ir_freecount/free mismatch, inode chunk %d/%u, freecount %d nfree %d\n"),
			agno, ino, freecount, nfree);
	}

	if (!nfree) {
		do_warn(
_("finobt record with no free inodes, inode chunk %d/%u\n"), agno, ino);
	}

	/* verify sparse record formats have a valid inode count */
	if (xfs_has_sparseinodes(mp) &&
	    ninodes != rp->ir_u.sp.ir_count) {
		do_warn(
_("invalid inode count, inode chunk %d/%u, count %d ninodes %d\n"),
			agno, ino, rp->ir_u.sp.ir_count, ninodes);
	}

	return suspect;
}

struct ino_priv {
	struct aghdr_cnts	*agcnts;
	uint32_t		ino_blocks;
	uint32_t		fino_blocks;
};

/*
 * this one walks the inode btrees sucking the info there into
 * the incore avl tree.  We try and rescue corrupted btree records
 * to minimize our chances of losing inodes.  Inode info from potentially
 * corrupt sources could be bogus so rather than put the info straight
 * into the tree, instead we put it on a list and try and verify the
 * info in the next phase by examining what's on disk.  At that point,
 * we'll be able to figure out what's what and stick the corrected info
 * into the tree.  We do bail out at some point and give up on a subtree
 * so as to avoid walking randomly all over the ag.
 *
 * Note that it's also ok if the free/inuse info wrong, we can correct
 * that when we examine the on-disk inode.  The important thing is to
 * get the start and alignment of the inode chunks right.  Those chunks
 * that we aren't sure about go into the uncertain list.
 */
static void
scan_inobt(
	struct xfs_btree_block	*block,
	int			level,
	xfs_agblock_t		bno,
	xfs_agnumber_t		agno,
	int			suspect,
	int			isroot,
	uint32_t		magic,
	void			*priv,
	const struct xfs_buf_ops *ops)
{
	struct ino_priv		*ipriv = priv;
	struct aghdr_cnts	*agcnts = ipriv->agcnts;
	char			*name;
	xfs_agino_t		lastino = 0;
	int			i;
	int			numrecs;
	int			state;
	xfs_inobt_ptr_t		*pp;
	xfs_inobt_rec_t		*rp;
	int			hdr_errors;
	int			freecount;
	struct xfs_ino_geometry *igeo = M_IGEO(mp);
	struct xfs_perag	*pag;

	hdr_errors = 0;

	switch (magic) {
	case XFS_FIBT_MAGIC:
	case XFS_FIBT_CRC_MAGIC:
		name = "fino";
		ipriv->fino_blocks++;
		break;
	case XFS_IBT_MAGIC:
	case XFS_IBT_CRC_MAGIC:
		name = "ino";
		ipriv->ino_blocks++;
		break;
	default:
		name = "(unknown)";
		assert(0);
		break;
	}

	if (be32_to_cpu(block->bb_magic) != magic) {
		do_warn(_("bad magic # %#x in %sbt block %d/%d\n"),
			be32_to_cpu(block->bb_magic), name, agno, bno);
		hdr_errors++;
		bad_ino_btree = 1;
		if (suspect)
			return;
	}
	if (be16_to_cpu(block->bb_level) != level) {
		do_warn(_("expected level %d got %d in %sbt block %d/%d\n"),
			level, be16_to_cpu(block->bb_level), name, agno, bno);
		hdr_errors++;
		bad_ino_btree = 1;
		if (suspect)
			return;
	}

	/*
	 * check for btree blocks multiply claimed, any unknown/free state
	 * is ok in the bitmap block.
	 */
	state = get_bmap(agno, bno);
	switch (state)  {
	case XR_E_FS_MAP1: /* already been seen by an rmap scan */
	case XR_E_UNKNOWN:
	case XR_E_FREE1:
	case XR_E_FREE:
		set_bmap(agno, bno, XR_E_FS_MAP);
		break;
	default:
		set_bmap(agno, bno, XR_E_MULT);
		do_warn(
_("%sbt btree block claimed (state %d), agno %d, bno %d, suspect %d\n"),
			name, state, agno, bno, suspect);
	}

	numrecs = be16_to_cpu(block->bb_numrecs);

	/*
	 * leaf record in btree
	 */
	if (level == 0) {
		/* check for trashed btree block */

		if (numrecs > igeo->inobt_mxr[0])  {
			numrecs = igeo->inobt_mxr[0];
			hdr_errors++;
		}
		if (isroot == 0 && numrecs < igeo->inobt_mnr[0])  {
			numrecs = igeo->inobt_mnr[0];
			hdr_errors++;
		}

		if (hdr_errors)  {
			bad_ino_btree = 1;
			do_warn(_("dubious %sbt btree block header %d/%d\n"),
				name, agno, bno);
			suspect++;
		}

		rp = XFS_INOBT_REC_ADDR(mp, block, 1);

		/*
		 * step through the records, each record points to
		 * a chunk of inodes.  The start of inode chunks should
		 * be block-aligned.  Each inode btree rec should point
		 * to the start of a block of inodes or the start of a group
		 * of INODES_PER_CHUNK (64) inodes.  off is the offset into
		 * the block.  skip processing of bogus records.
		 */
		for (i = 0; i < numrecs; i++) {
			xfs_agino_t	startino;

			freecount = inorec_get_freecount(mp, &rp[i]);
			startino = be32_to_cpu(rp[i].ir_startino);
			if (i > 0 && startino <= lastino)
				do_warn(_(
	"out-of-order %s btree record %d (%u) block %u/%u\n"),
						name, i, startino, agno, bno);
			else
				lastino = startino + XFS_INODES_PER_CHUNK - 1;

			if (magic == XFS_IBT_MAGIC ||
			    magic == XFS_IBT_CRC_MAGIC) {
				int icount = XFS_INODES_PER_CHUNK;

				/*
				 * ir_count holds the inode count for all
				 * records on fs' with sparse inode support
				 */
				if (xfs_has_sparseinodes(mp))
					icount = rp[i].ir_u.sp.ir_count;

				agcnts->agicount += icount;
				agcnts->agifreecount += freecount;
				agcnts->ifreecount += freecount;

				suspect = scan_single_ino_chunk(agno, &rp[i],
						suspect);
			} else {
				/*
				 * the finobt tracks records with free inodes,
				 * so only the free inode count is expected to be
				 * consistent with the agi
				 */
				agcnts->fibtfreecount += freecount;

				suspect = scan_single_finobt_chunk(agno, &rp[i],
						suspect);
			}
		}

		if (suspect)
			bad_ino_btree = 1;

		return;
	}

	/*
	 * interior record, continue on
	 */
	if (numrecs > igeo->inobt_mxr[1])  {
		numrecs = igeo->inobt_mxr[1];
		hdr_errors++;
	}
	if (isroot == 0 && numrecs < igeo->inobt_mnr[1])  {
		numrecs = igeo->inobt_mnr[1];
		hdr_errors++;
	}

	pp = XFS_INOBT_PTR_ADDR(mp, block, 1, igeo->inobt_mxr[1]);

	/*
	 * don't pass bogus tree flag down further if this block
	 * looked ok.  bail out if two levels in a row look bad.
	 */

	if (suspect && !hdr_errors)
		suspect = 0;

	if (hdr_errors)  {
		bad_ino_btree = 1;
		if (suspect)
			return;
		else suspect++;
	}

	pag = libxfs_perag_get(mp, agno);
	for (i = 0; i < numrecs; i++)  {
		xfs_agblock_t	agbno = be32_to_cpu(pp[i]);

		if (!libxfs_verify_agbno(pag, agbno)) {
			do_warn(
	_("bad btree pointer (%u) in %sbt block %u/%u\n"),
				agbno, name, agno, bno);
			suspect++;
			libxfs_perag_put(pag);
			return;
		}

		scan_sbtree(be32_to_cpu(pp[i]), level, agno, suspect,
				scan_inobt, 0, magic, priv, ops);
	}
	libxfs_perag_put(pag);
}

struct agfl_state {
	unsigned int	count;
	xfs_agnumber_t	agno;
};

static int
scan_agfl(
	struct xfs_mount	*mp,
	xfs_agblock_t		bno,
	void			*priv)
{
	struct agfl_state	*as = priv;
	struct xfs_perag	*pag;

	pag = libxfs_perag_get(mp, as->agno);
	if (libxfs_verify_agbno(pag, bno))
		set_bmap(as->agno, bno, XR_E_FREE);
	else
		do_warn(_("bad agbno %u in agfl, agno %d\n"),
			bno, as->agno);

	libxfs_perag_put(pag);
	as->count++;
	return 0;
}

static void
scan_freelist(
	xfs_agf_t		*agf,
	struct aghdr_cnts	*agcnts)
{
	struct xfs_buf		*agflbuf;
	xfs_agnumber_t		agno;
	struct agfl_state	state;
	int			error;

	agno = be32_to_cpu(agf->agf_seqno);

	if (XFS_SB_BLOCK(mp) != XFS_AGFL_BLOCK(mp) &&
	    XFS_AGF_BLOCK(mp) != XFS_AGFL_BLOCK(mp) &&
	    XFS_AGI_BLOCK(mp) != XFS_AGFL_BLOCK(mp))
		set_bmap(agno, XFS_AGFL_BLOCK(mp), XR_E_INUSE_FS);

	if (be32_to_cpu(agf->agf_flcount) == 0)
		return;

	error = salvage_buffer(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGFL_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), &agflbuf, &xfs_agfl_buf_ops);
	if (error) {
		do_abort(_("can't read agfl block for ag %d\n"), agno);
		return;
	}
	if (agflbuf->b_error == -EFSBADCRC)
		do_warn(_("agfl has bad CRC for ag %d\n"), agno);

	if (no_modify) {
		/* agf values not fixed in verify_set_agf, so recheck */
		if (be32_to_cpu(agf->agf_flfirst) >= libxfs_agfl_size(mp) ||
		    be32_to_cpu(agf->agf_fllast) >= libxfs_agfl_size(mp)) {
			do_warn(_("agf %d freelist blocks bad, skipping "
				  "freelist scan\n"), agno);
			return;
		}
	}

	state.count = 0;
	state.agno = agno;
	libxfs_agfl_walk(mp, agf, agflbuf, scan_agfl, &state);
	if (state.count != be32_to_cpu(agf->agf_flcount)) {
		do_warn(_("freeblk count %d != flcount %d in ag %d\n"),
				state.count, be32_to_cpu(agf->agf_flcount),
				agno);
	}

	agcnts->fdblocks += state.count;

	libxfs_buf_relse(agflbuf);
}

static void
validate_agf(
	struct xfs_agf		*agf,
	xfs_agnumber_t		agno,
	struct aghdr_cnts	*agcnts)
{
	xfs_agblock_t		bno;
	uint32_t		magic;
	unsigned int		levels;
	struct xfs_perag	*pag = libxfs_perag_get(mp, agno);

	levels = be32_to_cpu(agf->agf_bno_level);
	if (levels == 0 || levels > mp->m_alloc_maxlevels) {
		do_warn(_("bad levels %u for btbno root, agno %d\n"),
			levels, agno);
	}

	bno = be32_to_cpu(agf->agf_bno_root);
	if (libxfs_verify_agbno(pag, bno)) {
		magic = xfs_has_crc(mp) ? XFS_ABTB_CRC_MAGIC
							 : XFS_ABTB_MAGIC;
		scan_sbtree(bno, be32_to_cpu(agf->agf_bno_level),
			    agno, 0, scan_allocbt, 1, magic, agcnts,
			    &xfs_bnobt_buf_ops);
	} else {
		do_warn(_("bad agbno %u for btbno root, agno %d\n"),
			bno, agno);
	}

	levels = be32_to_cpu(agf->agf_cnt_level);
	if (levels == 0 || levels > mp->m_alloc_maxlevels) {
		do_warn(_("bad levels %u for btbcnt root, agno %d\n"),
			levels, agno);
	}

	bno = be32_to_cpu(agf->agf_cnt_root);
	if (libxfs_verify_agbno(pag, bno)) {
		magic = xfs_has_crc(mp) ? XFS_ABTC_CRC_MAGIC
							 : XFS_ABTC_MAGIC;
		scan_sbtree(bno, be32_to_cpu(agf->agf_cnt_level),
			    agno, 0, scan_allocbt, 1, magic, agcnts,
			    &xfs_cntbt_buf_ops);
	} else  {
		do_warn(_("bad agbno %u for btbcnt root, agno %d\n"),
			bno, agno);
	}

	if (xfs_has_rmapbt(mp)) {
		struct rmap_priv	priv;

		memset(&priv.high_key, 0xFF, sizeof(priv.high_key));
		priv.high_key.rm_blockcount = 0;
		priv.agcnts = agcnts;
		priv.last_rec.rm_owner = XFS_RMAP_OWN_UNKNOWN;
		priv.nr_blocks = 0;

		levels = be32_to_cpu(agf->agf_rmap_level);
		if (levels == 0 || levels > mp->m_rmap_maxlevels) {
			do_warn(_("bad levels %u for rmapbt root, agno %d\n"),
				levels, agno);
			rmap_avoid_check(mp);
		}

		bno = be32_to_cpu(agf->agf_rmap_root);
		if (libxfs_verify_agbno(pag, bno)) {
			scan_sbtree(bno, levels, agno, 0, scan_rmapbt, 1,
					XFS_RMAP_CRC_MAGIC, &priv,
					&xfs_rmapbt_buf_ops);
			if (be32_to_cpu(agf->agf_rmap_blocks) != priv.nr_blocks)
				do_warn(_("bad rmapbt block count %u, saw %u\n"),
					priv.nr_blocks,
					be32_to_cpu(agf->agf_rmap_blocks));
		} else {
			do_warn(_("bad agbno %u for rmapbt root, agno %d\n"),
				bno, agno);
			rmap_avoid_check(mp);
		}
	}

	if (xfs_has_reflink(mp)) {
		levels = be32_to_cpu(agf->agf_refcount_level);
		if (levels == 0 || levels > mp->m_refc_maxlevels) {
			do_warn(_("bad levels %u for refcountbt root, agno %d\n"),
				levels, agno);
			refcount_avoid_check(mp);
		}

		bno = be32_to_cpu(agf->agf_refcount_root);
		if (libxfs_verify_agbno(pag, bno)) {
			struct refc_priv	priv;

			memset(&priv, 0, sizeof(priv));
			scan_sbtree(bno, levels, agno, 0, scan_refcbt, 1,
					XFS_REFC_CRC_MAGIC, &priv,
					&xfs_refcountbt_buf_ops);
			if (be32_to_cpu(agf->agf_refcount_blocks) != priv.nr_blocks)
				do_warn(_("bad refcountbt block count %u, saw %u\n"),
					priv.nr_blocks,
					be32_to_cpu(agf->agf_refcount_blocks));
		} else {
			do_warn(_("bad agbno %u for refcntbt root, agno %d\n"),
				bno, agno);
			refcount_avoid_check(mp);
		}
	}

	if (be32_to_cpu(agf->agf_freeblks) != agcnts->agffreeblks) {
		do_warn(_("agf_freeblks %u, counted %u in ag %u\n"),
			be32_to_cpu(agf->agf_freeblks), agcnts->agffreeblks, agno);
	}

	if (be32_to_cpu(agf->agf_longest) != agcnts->agflongest) {
		do_warn(_("agf_longest %u, counted %u in ag %u\n"),
			be32_to_cpu(agf->agf_longest), agcnts->agflongest, agno);
	}

	if (xfs_has_lazysbcount(mp) &&
	    be32_to_cpu(agf->agf_btreeblks) != agcnts->agfbtreeblks) {
		do_warn(_("agf_btreeblks %u, counted %" PRIu64 " in ag %u\n"),
			be32_to_cpu(agf->agf_btreeblks), agcnts->agfbtreeblks, agno);
	}
	libxfs_perag_put(pag);

}

static void
validate_agi(
	struct xfs_agi		*agi,
	xfs_agnumber_t		agno,
	struct aghdr_cnts	*agcnts)
{
	struct ino_priv		priv = {
		.agcnts = agcnts,
	};
	xfs_agblock_t		bno;
	int			i;
	uint32_t		magic;
	unsigned int		levels;
	struct xfs_perag	*pag = libxfs_perag_get(mp, agno);

	levels = be32_to_cpu(agi->agi_level);
	if (levels == 0 || levels > M_IGEO(mp)->inobt_maxlevels) {
		do_warn(_("bad levels %u for inobt root, agno %d\n"),
			levels, agno);
	}

	bno = be32_to_cpu(agi->agi_root);
	if (libxfs_verify_agbno(pag, bno)) {
		magic = xfs_has_crc(mp) ? XFS_IBT_CRC_MAGIC
							 : XFS_IBT_MAGIC;
		scan_sbtree(bno, be32_to_cpu(agi->agi_level),
			    agno, 0, scan_inobt, 1, magic, &priv,
			    &xfs_inobt_buf_ops);
	} else {
		do_warn(_("bad agbno %u for inobt root, agno %d\n"),
			be32_to_cpu(agi->agi_root), agno);
	}

	if (xfs_has_finobt(mp)) {
		levels = be32_to_cpu(agi->agi_free_level);
		if (levels == 0 || levels > M_IGEO(mp)->inobt_maxlevels) {
			do_warn(_("bad levels %u for finobt root, agno %d\n"),
				levels, agno);
		}

		bno = be32_to_cpu(agi->agi_free_root);
		if (libxfs_verify_agbno(pag, bno)) {
			magic = xfs_has_crc(mp) ?
					XFS_FIBT_CRC_MAGIC : XFS_FIBT_MAGIC;
			scan_sbtree(bno, be32_to_cpu(agi->agi_free_level),
				    agno, 0, scan_inobt, 1, magic, &priv,
				    &xfs_finobt_buf_ops);
		} else {
			do_warn(_("bad agbno %u for finobt root, agno %d\n"),
				be32_to_cpu(agi->agi_free_root), agno);
		}
	}

	if (xfs_has_inobtcounts(mp)) {
		if (be32_to_cpu(agi->agi_iblocks) != priv.ino_blocks)
			do_warn(_("bad inobt block count %u, saw %u\n"),
					be32_to_cpu(agi->agi_iblocks),
					priv.ino_blocks);
		if (be32_to_cpu(agi->agi_fblocks) != priv.fino_blocks)
			do_warn(_("bad finobt block count %u, saw %u\n"),
					be32_to_cpu(agi->agi_fblocks),
					priv.fino_blocks);
	}

	if (be32_to_cpu(agi->agi_count) != agcnts->agicount) {
		do_warn(_("agi_count %u, counted %u in ag %u\n"),
			 be32_to_cpu(agi->agi_count), agcnts->agicount, agno);
	}

	if (be32_to_cpu(agi->agi_freecount) != agcnts->agifreecount) {
		do_warn(_("agi_freecount %u, counted %u in ag %u\n"),
			be32_to_cpu(agi->agi_freecount), agcnts->agifreecount, agno);
	}

	if (xfs_has_finobt(mp) &&
	    be32_to_cpu(agi->agi_freecount) != agcnts->fibtfreecount) {
		do_warn(_("agi_freecount %u, counted %u in ag %u finobt\n"),
			be32_to_cpu(agi->agi_freecount), agcnts->fibtfreecount,
			agno);
	}

	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++) {
		xfs_agino_t	agino = be32_to_cpu(agi->agi_unlinked[i]);

		if (agino != NULLAGINO) {
			do_warn(
	_("agi unlinked bucket %d is %u in ag %u (inode=%" PRIu64 ")\n"),
				i, agino, agno,
				XFS_AGINO_TO_INO(mp, agno, agino));
		}
	}
	libxfs_perag_put(pag);
}

/*
 * Scan an AG for obvious corruption.
 */
static void
scan_ag(
	struct workqueue*wq,
	xfs_agnumber_t	agno,
	void		*arg)
{
	struct aghdr_cnts *agcnts = arg;
	struct xfs_agf	*agf;
	struct xfs_buf	*agfbuf = NULL;
	int		agf_dirty = 0;
	struct xfs_agi	*agi;
	struct xfs_buf	*agibuf = NULL;
	int		agi_dirty = 0;
	struct xfs_sb	*sb = NULL;
	struct xfs_buf	*sbbuf = NULL;
	int		sb_dirty = 0;
	int		status;
	char		*objname = NULL;
	int		error;

	sb = (struct xfs_sb *)calloc(BBTOB(XFS_FSS_TO_BB(mp, 1)), 1);
	if (!sb) {
		do_error(_("can't allocate memory for superblock\n"));
		return;
	}

	error = salvage_buffer(mp->m_dev, XFS_AG_DADDR(mp, agno, XFS_SB_DADDR),
			XFS_FSS_TO_BB(mp, 1), &sbbuf, &xfs_sb_buf_ops);
	if (error) {
		objname = _("root superblock");
		goto out_free_sb;
	}
	if (sbbuf->b_error == -EFSBADCRC)
		do_warn(_("superblock has bad CRC for ag %d\n"), agno);
	libxfs_sb_from_disk(sb, sbbuf->b_addr);

	error = salvage_buffer(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGF_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), &agfbuf, &xfs_agf_buf_ops);
	if (error) {
		objname = _("agf block");
		goto out_free_sbbuf;
	}
	if (agfbuf->b_error == -EFSBADCRC)
		do_warn(_("agf has bad CRC for ag %d\n"), agno);
	agf = agfbuf->b_addr;

	error = salvage_buffer(mp->m_dev,
			XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
			XFS_FSS_TO_BB(mp, 1), &agibuf, &xfs_agi_buf_ops);
	if (error) {
		objname = _("agi block");
		goto out_free_agfbuf;
	}
	if (agibuf->b_error == -EFSBADCRC)
		do_warn(_("agi has bad CRC for ag %d\n"), agno);
	agi = agibuf->b_addr;

	/* fix up bad ag headers */

	status = verify_set_agheader(mp, sbbuf, sb, agf, agi, agno);

	if (status & XR_AG_SB_SEC)  {
		if (!no_modify)
			sb_dirty = 1;
		/*
		 * clear bad sector bit because we don't want
		 * to skip further processing.  we just want to
		 * ensure that we write out the modified sb buffer.
		 */
		status &= ~XR_AG_SB_SEC;
	}
	if (status & XR_AG_SB)  {
		if (!no_modify) {
			do_warn(_("reset bad sb for ag %d\n"), agno);
			sb_dirty = 1;
		} else {
			do_warn(_("would reset bad sb for ag %d\n"), agno);
		}
	}
	if (status & XR_AG_AGF)  {
		if (!no_modify) {
			do_warn(_("reset bad agf for ag %d\n"), agno);
			agf_dirty = 1;
		} else {
			do_warn(_("would reset bad agf for ag %d\n"), agno);
		}
	}
	if (status & XR_AG_AGI)  {
		if (!no_modify) {
			do_warn(_("reset bad agi for ag %d\n"), agno);
			agi_dirty = 1;
		} else {
			do_warn(_("would reset bad agi for ag %d\n"), agno);
		}
	}

	if (status && no_modify)  {
		do_warn(_("bad uncorrected agheader %d, skipping ag...\n"),
			agno);
		goto out_free_agibuf;
	}

	scan_freelist(agf, agcnts);

	validate_agf(agf, agno, agcnts);
	validate_agi(agi, agno, agcnts);

	ASSERT(agi_dirty == 0 || (agi_dirty && !no_modify));
	ASSERT(agf_dirty == 0 || (agf_dirty && !no_modify));
	ASSERT(sb_dirty == 0 || (sb_dirty && !no_modify));

	/*
	 * Only pay attention to CRC/verifier errors if we can correct them.
	 * Note that we can get uncorrected EFSCORRUPTED errors here because
	 * the verifier will flag on out of range values that we can't correct
	 * until phase 5 when we have all the information necessary to rebuild
	 * the freespace/inode btrees. We can correct bad CRC errors
	 * immediately, though.
	 */
	if (!no_modify) {
		agi_dirty += (agibuf->b_error == -EFSBADCRC);
		agf_dirty += (agfbuf->b_error == -EFSBADCRC);
		sb_dirty += (sbbuf->b_error == -EFSBADCRC);
	}

	if (agi_dirty && !no_modify) {
		libxfs_buf_mark_dirty(agibuf);
		libxfs_buf_relse(agibuf);
	}
	else
		libxfs_buf_relse(agibuf);

	if (agf_dirty && !no_modify) {
		libxfs_buf_mark_dirty(agfbuf);
		libxfs_buf_relse(agfbuf);
	}
	else
		libxfs_buf_relse(agfbuf);

	if (sb_dirty && !no_modify) {
		if (agno == 0)
			memcpy(&mp->m_sb, sb, sizeof(xfs_sb_t));
		libxfs_sb_to_disk(sbbuf->b_addr, sb);
		libxfs_buf_mark_dirty(sbbuf);
		libxfs_buf_relse(sbbuf);
	} else
		libxfs_buf_relse(sbbuf);
	free(sb);
	PROG_RPT_INC(prog_rpt_done[agno], 1);

#ifdef XR_INODE_TRACE
	print_inode_list(i);
#endif
	return;

out_free_agibuf:
	libxfs_buf_relse(agibuf);
out_free_agfbuf:
	libxfs_buf_relse(agfbuf);
out_free_sbbuf:
	libxfs_buf_relse(sbbuf);
out_free_sb:
	free(sb);

	if (objname)
		do_error(_("can't get %s for ag %d\n"), objname, agno);
}

void
scan_ags(
	struct xfs_mount	*mp,
	int			scan_threads)
{
	struct aghdr_cnts	*agcnts;
	uint64_t		fdblocks = 0;
	uint64_t		icount = 0;
	uint64_t		ifreecount = 0;
	uint64_t		usedblocks = 0;
	xfs_agnumber_t		i;
	struct workqueue	wq;

	agcnts = malloc(mp->m_sb.sb_agcount * sizeof(*agcnts));
	if (!agcnts) {
		do_abort(_("no memory for ag header counts\n"));
		return;
	}
	memset(agcnts, 0, mp->m_sb.sb_agcount * sizeof(*agcnts));

	create_work_queue(&wq, mp, scan_threads);

	for (i = 0; i < mp->m_sb.sb_agcount; i++)
		queue_work(&wq, scan_ag, i, &agcnts[i]);

	destroy_work_queue(&wq);

	/* tally up the counts */
	for (i = 0; i < mp->m_sb.sb_agcount; i++) {
		fdblocks += agcnts[i].fdblocks;
		icount += agcnts[i].agicount;
		ifreecount += agcnts[i].ifreecount;
		usedblocks += agcnts[i].usedblocks;
	}

	free(agcnts);

	/*
	 * Validate that our manual counts match the superblock.
	 */
	if (mp->m_sb.sb_icount != icount) {
		do_warn(_("sb_icount %" PRIu64 ", counted %" PRIu64 "\n"),
			mp->m_sb.sb_icount, icount);
	}

	if (mp->m_sb.sb_ifree != ifreecount) {
		do_warn(_("sb_ifree %" PRIu64 ", counted %" PRIu64 "\n"),
			mp->m_sb.sb_ifree, ifreecount);
	}

	if (mp->m_sb.sb_fdblocks != fdblocks) {
		do_warn(_("sb_fdblocks %" PRIu64 ", counted %" PRIu64 "\n"),
			mp->m_sb.sb_fdblocks, fdblocks);
	}

	if (usedblocks &&
	    usedblocks != mp->m_sb.sb_dblocks - fdblocks) {
		do_warn(_("used blocks %" PRIu64 ", counted %" PRIu64 "\n"),
			mp->m_sb.sb_dblocks - fdblocks, usedblocks);
	}
}
