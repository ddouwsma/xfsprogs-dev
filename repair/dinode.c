// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "protos.h"
#include "err_protos.h"
#include "dir2.h"
#include "dinode.h"
#include "scan.h"
#include "versions.h"
#include "attr_repair.h"
#include "bmap.h"
#include "threads.h"
#include "slab.h"
#include "rmap.h"
#include "bmap_repair.h"
#include "rt.h"

/*
 * gettext lookups for translations of strings use mutexes internally to
 * the library. Hence when we come through here doing parallel scans in
 * multiple AGs, then all do concurrent text conversions and serialise
 * on the translation string lookups. Let's avoid doing repeated lookups
 * by making them static variables and only assigning the translation
 * once.
 */
static char	*forkname_data;
static char	*forkname_attr;
static char	*ftype_real_time;
static char	*ftype_regular;

void
dinode_bmbt_translation_init(void)
{
	forkname_data = _("data");
	forkname_attr = _("attr");
	ftype_real_time = _("real-time");
	ftype_regular = _("regular");
}

char *
get_forkname(int whichfork)
{

	if (whichfork == XFS_DATA_FORK)
		return forkname_data;
	return forkname_attr;
}

/*
 * inode clearing routines
 */

static int
clear_dinode_attr(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		ino_num)
{
	ASSERT(dino->di_forkoff != 0);

	if (!no_modify)
		fprintf(stderr,
_("clearing inode %" PRIu64 " attributes\n"), ino_num);
	else
		fprintf(stderr,
_("would have cleared inode %" PRIu64 " attributes\n"), ino_num);

	if (xfs_dfork_attr_extents(dino) != 0)  {
		if (no_modify)
			return(1);

		if (xfs_dinode_has_large_extent_counts(dino))
			dino->di_big_anextents = 0;
		else
			dino->di_anextents = 0;
	}

	if (dino->di_aformat != XFS_DINODE_FMT_EXTENTS)  {
		if (no_modify)
			return(1);
		dino->di_aformat = XFS_DINODE_FMT_EXTENTS;
	}

	/* get rid of the fork by clearing forkoff */

	/* Originally, when the attr repair code was added, the fork was cleared
	 * by turning it into shortform status.  This meant clearing the
	 * hdr.totsize/count fields and also changing aformat to LOCAL
	 * (vs EXTENTS).  Over various fixes, the aformat and forkoff have
	 * been updated to not show an attribute fork at all, however.
	 * It could be possible that resetting totsize/count are not needed,
	 * but just to be safe, leave it in for now.
	 */

	if (!no_modify) {
		struct xfs_attr_sf_hdr	*hdr = XFS_DFORK_APTR(dino);

		hdr->totsize = cpu_to_be16(sizeof(struct xfs_attr_sf_hdr));
		hdr->count = 0;
		dino->di_forkoff = 0;  /* got to do this after asf is set */
	}

	/*
	 * always returns 1 since the fork gets zapped
	 */
	return(1);
}

static void
clear_dinode_core(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dinoc,
	xfs_ino_t		ino_num)
{
	memset(dinoc, 0, sizeof(*dinoc));
	dinoc->di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
	if (xfs_has_crc(mp))
		dinoc->di_version = 3;
	else
		dinoc->di_version = 2;
	dinoc->di_gen = cpu_to_be32(random());
	dinoc->di_format = XFS_DINODE_FMT_EXTENTS;
	dinoc->di_aformat = XFS_DINODE_FMT_EXTENTS;
	/* we are done for version 1/2 inodes */
	if (dinoc->di_version < 3)
		return;
	dinoc->di_ino = cpu_to_be64(ino_num);
	platform_uuid_copy(&dinoc->di_uuid, &mp->m_sb.sb_meta_uuid);
	return;
}

static void
clear_dinode_unlinked(xfs_mount_t *mp, struct xfs_dinode *dino)
{

	dino->di_next_unlinked = cpu_to_be32(NULLAGINO);
}

/*
 * this clears the unlinked list too so it should not be called
 * until after the agi unlinked lists are walked in phase 3.
 */
static void
zero_dinode(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		ino_num)
{
	clear_dinode_core(mp, dino, ino_num);
	clear_dinode_unlinked(mp, dino);

	/* and clear the forks */
	memset(XFS_DFORK_DPTR(dino), 0, XFS_LITINO(mp));
}

/*
 * clear the inode core and, if this is a metadata inode, prevent subsequent
 * phases from checking the (obviously bad) data in the file.
 */
static void
clear_dinode(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		ino_num)
{
	zero_dinode(mp, dino, ino_num);

	if (is_rtbitmap_inode(ino_num))
		mark_rtgroup_inodes_bad(mp, XFS_RTGI_BITMAP);

	if (is_rtsummary_inode(ino_num))
		mark_rtgroup_inodes_bad(mp, XFS_RTGI_SUMMARY);

	if (is_rtrmap_inode(ino_num))
		rmap_avoid_check(mp);

	if (is_rtrefcount_inode(ino_num))
		refcount_avoid_check(mp);
}

/*
 * misc. inode-related utility routines
 */

#define XR_DFSBNORANGE_VALID	0
#define XR_DFSBNORANGE_BADSTART	1
#define XR_DFSBNORANGE_BADEND	2
#define XR_DFSBNORANGE_OVERFLOW	3

static __inline int
verify_dfsbno_range(
	struct xfs_mount	*mp,
	struct xfs_bmbt_irec	*irec,
	bool			isrt)
{
	xfs_fsblock_t		end =
		irec->br_startblock + irec->br_blockcount - 1;

	/* the start and end blocks better be in the same allocation group */
	if (isrt) {
		if (xfs_rtb_to_rgno(mp, irec->br_startblock) !=
		    xfs_rtb_to_rgno(mp, end))
			return XR_DFSBNORANGE_OVERFLOW;

		if (!libxfs_verify_rtbno(mp, irec->br_startblock))
			return XR_DFSBNORANGE_BADSTART;
		if (!libxfs_verify_rtbno(mp, end))
			return XR_DFSBNORANGE_BADEND;
	} else {
		if (XFS_FSB_TO_AGNO(mp, irec->br_startblock) !=
		    XFS_FSB_TO_AGNO(mp, end))
			return XR_DFSBNORANGE_OVERFLOW;

		if (!libxfs_verify_fsbno(mp, irec->br_startblock))
			return XR_DFSBNORANGE_BADSTART;
		if (!libxfs_verify_fsbno(mp, end))
			return XR_DFSBNORANGE_BADEND;
	}

	return XR_DFSBNORANGE_VALID;
}

static int
process_rt_rec_dups(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_bmbt_irec	*irec)
{
	xfs_rtblock_t		b;
	xfs_rtxnum_t		ext;

	for (b = irec->br_startblock;
	     b < irec->br_startblock + irec->br_blockcount;
	     b += mp->m_sb.sb_rextsize) {
		ext = xfs_rtb_to_rtx(mp, b);
		if (search_rt_dup_extent(mp, ext))  {
			do_warn(
_("data fork in rt ino %" PRIu64 " claims dup rt extent,"
"off - %" PRIu64 ", start - %" PRIu64 ", count %" PRIu64 "\n"),
				ino,
				irec->br_startoff,
				irec->br_startblock,
				irec->br_blockcount);
			return 1;
		}
	}

	return 0;
}

static void
process_rt_rec_state(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	bool			zap_metadata,
	struct xfs_bmbt_irec	*irec)
{
	xfs_fsblock_t		b = irec->br_startblock;
	xfs_rtxnum_t		ext;
	int			state;

	do {
		xfs_extlen_t	mod;

		ext = xfs_rtb_to_rtx(mp, b);
		state = get_rtbmap(ext);

		mod = xfs_rtb_to_rtxoff(mp, b);
		if (mod) {
			/*
			 * We are midway through a partially written extent.
			 * If we don't find the state that gets set in the
			 * other clause of this loop body, then we have a
			 * partially *mapped* rt extent and should complain.
			 */
			if (state != XR_E_INUSE)
				do_error(
_("data fork in rt inode %" PRIu64 " found invalid rt extent %"PRIu64" state %d at rt block %"PRIu64"\n"),
					ino, ext, state, b);
			b += mp->m_sb.sb_rextsize - mod;
			continue;
		}

		/*
		 * This is the start of an rt extent.  Set the extent state if
		 * nobody else has claimed the extent, or complain if there are
		 * conflicting states.
		 */
		switch (state)  {
		case XR_E_FREE:
		case XR_E_UNKNOWN:
			set_rtbmap(ext, zap_metadata ? XR_E_METADATA :
						       XR_E_INUSE);
			break;
		case XR_E_BAD_STATE:
			do_error(
_("bad state in rt extent map %" PRIu64 "\n"),
				ext);
		case XR_E_METADATA:
		case XR_E_FS_MAP:
		case XR_E_INO:
		case XR_E_INUSE_FS:
			break;
		case XR_E_INUSE:
		case XR_E_MULT:
			if (xfs_has_rtreflink(mp) &&
			    irec->br_state == XFS_EXT_NORM)
				break;
			set_rtbmap(ext, XR_E_MULT);
			break;
		case XR_E_FREE1:
		default:
			do_error(
_("illegal state %d in rt extent %" PRIu64 "\n"),
				state, ext);
		}
		b += mp->m_sb.sb_rextsize;
	} while (b < irec->br_startblock + irec->br_blockcount);
}

/*
 * Checks the realtime file's data mapping against in-core extent info, and
 * complains if there are discrepancies.  Returns 0 if good, 1 if bad.
 */
static int
check_rt_rec_state(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_bmbt_irec	*irec)
{
	xfs_fsblock_t		b = irec->br_startblock;
	xfs_rtblock_t		ext;
	int			state;

	do {
		ext = (xfs_rtblock_t)b / mp->m_sb.sb_rextsize;
		state = get_rtbmap(ext);

		if ((b % mp->m_sb.sb_rextsize) != 0) {
			/*
			 * We are midway through a partially written extent.
			 * If we don't find the state that gets set in the
			 * other clause of this loop body, then we have a
			 * partially *mapped* rt extent and should complain.
			 */
			if (state != XR_E_INUSE && state != XR_E_FREE) {
				do_warn(
_("data fork in rt inode %" PRIu64 " found invalid rt extent %"PRIu64" state %d at rt block %"PRIu64"\n"),
					ino, ext, state, b);
				return 1;
			}

			b = roundup(b, mp->m_sb.sb_rextsize);
			continue;
		}

		/*
		 * This is the start of an rt extent.  Complain if there are
		 * conflicting states.  We'll set the state elsewhere.
		 */
		switch (state)  {
		case XR_E_FREE:
		case XR_E_UNKNOWN:
			break;
		case XR_E_METADATA:
			do_warn(
_("data fork in rt inode %" PRIu64 " found metadata file block %" PRIu64 " in rt bmap\n"),
				ino, ext);
			return 1;
		case XR_E_BAD_STATE:
			do_error(
_("bad state in rt extent map %" PRIu64 "\n"),
				ext);
		case XR_E_FS_MAP:
		case XR_E_INO:
		case XR_E_INUSE_FS:
			do_warn(
_("data fork in rt inode %" PRIu64 " found rt metadata extent %" PRIu64 " in rt bmap\n"),
				ino, ext);
			return 1;
		case XR_E_INUSE:
		case XR_E_MULT:
			if (xfs_has_rtreflink(mp)) {
				if (irec->br_state == XFS_EXT_NORM)
					break;
				do_warn(
_("data fork in rt inode %" PRIu64 " claims shared unwritten rt extent %" PRIu64 "\n"),
					ino, b);
				return 1;
			}
			do_warn(
_("data fork in rt inode %" PRIu64 " claims used rt extent %" PRIu64 "\n"),
				ino, b);
			return 1;
		case XR_E_FREE1:
		default:
			do_error(
_("illegal state %d in rt extent %" PRIu64 "\n"),
				state, ext);
		}
		b += mp->m_sb.sb_rextsize;
	} while (b < irec->br_startblock + irec->br_blockcount);

	return 0;
}

static int
process_rt_rec(
	struct xfs_mount	*mp,
	struct xfs_bmbt_irec	*irec,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	int			check_dups,
	bool			zap_metadata)
{
	xfs_fsblock_t		lastb;
	int			bad = 0;

	/*
	 * check numeric validity of the extent
	 */
	if (!libxfs_verify_rtbno(mp, irec->br_startblock)) {
		do_warn(
_("inode %" PRIu64 " - bad rt extent start block number %" PRIu64 ", offset %" PRIu64 "\n"),
			ino,
			irec->br_startblock,
			irec->br_startoff);
		return 1;
	}

	lastb = irec->br_startblock + irec->br_blockcount - 1;
	if (!libxfs_verify_rtbno(mp, lastb)) {
		do_warn(
_("inode %" PRIu64 " - bad rt extent last block number %" PRIu64 ", offset %" PRIu64 "\n"),
			ino,
			lastb,
			irec->br_startoff);
		return 1;
	}
	if (lastb < irec->br_startblock) {
		do_warn(
_("inode %" PRIu64 " - bad rt extent overflows - start %" PRIu64 ", "
  "end %" PRIu64 ", offset %" PRIu64 "\n"),
			ino,
			irec->br_startblock,
			lastb,
			irec->br_startoff);
		return 1;
	}

	pthread_mutex_lock(&rt_lock);
	bad = check_rt_rec_state(mp, ino, irec);
	if (bad)
		goto out_unlock;

	if (check_dups) {
		bad = process_rt_rec_dups(mp, ino, irec);
		if (bad)
			goto out_unlock;
	} else {
		process_rt_rec_state(mp, ino, zap_metadata, irec);
	}

	/*
	 * bump up the block counter
	 */
	*tot += irec->br_blockcount;

out_unlock:
	pthread_mutex_unlock(&rt_lock);
	return bad;
}

static inline bool
is_reflink_type(
	struct xfs_mount	*mp,
	int			type)
{
	if (type == XR_INO_DATA && xfs_has_reflink(mp))
		return true;
	if (type == XR_INO_RTDATA && xfs_has_rtreflink(mp))
		return true;
	return false;
}

/*
 * return 1 if inode should be cleared, 0 otherwise
 * if check_dups should be set to 1, that implies that
 * the primary purpose of this call is to see if the
 * file overlaps with any duplicate extents (in the
 * duplicate extent list).
 */
static int
process_bmbt_reclist_int(
	xfs_mount_t		*mp,
	xfs_bmbt_rec_t		*rp,
	xfs_extnum_t		*numrecs,
	int			type,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	blkmap_t		**blkmapp,
	xfs_fileoff_t		*first_key,
	xfs_fileoff_t		*last_key,
	int			check_dups,
	int			whichfork,
	bool			zap_metadata)
{
	xfs_bmbt_irec_t		irec;
	xfs_filblks_t		cp = 0;		/* prev count */
	xfs_fsblock_t		sp = 0;		/* prev start */
	xfs_fileoff_t		op = 0;		/* prev offset */
	xfs_fsblock_t		b;
	char			*ftype;
	char			*forkname = get_forkname(whichfork);
	xfs_extnum_t		i;
	int			state;
	xfs_agnumber_t		agno;
	xfs_agblock_t		agbno, first_agbno;
	xfs_agblock_t		ebno;
	xfs_extlen_t		blen;
	xfs_agnumber_t		locked_agno = -1;
	int			error = 1;
	int			error2;
	bool			isrt = false;

	if (type == XR_INO_RTDATA) {
		if (whichfork == XFS_DATA_FORK)
			isrt = true;
		ftype = ftype_real_time;
	} else {
		ftype = ftype_regular;
	}

	for (i = 0; i < *numrecs; i++) {
		libxfs_bmbt_disk_get_all((rp +i), &irec);
		if (i == 0)
			*last_key = *first_key = irec.br_startoff;
		else
			*last_key = irec.br_startoff;
		if (i > 0 && op + cp > irec.br_startoff)  {
			do_warn(
_("bmap rec out of order, inode %" PRIu64" entry %" PRIu64 " "
  "[o s c] [%" PRIu64 " %" PRIu64 " %" PRIu64 "], "
  "%" PRIu64 " [%" PRIu64 " %" PRIu64 " %" PRIu64 "]\n"),
				ino, i, irec.br_startoff, irec.br_startblock,
				irec.br_blockcount, i - 1, op, sp, cp);
			goto done;
		}
		op = irec.br_startoff;
		cp = irec.br_blockcount;
		sp = irec.br_startblock;

		if (irec.br_state != XFS_EXT_NORM) {
			/* No unwritten extents in the attr fork */
			if (whichfork == XFS_ATTR_FORK) {
				do_warn(
_("unwritten extent (off = %" PRIu64 ", fsbno = %" PRIu64 ") in ino %" PRIu64 " attr fork\n"),
					irec.br_startoff,
					irec.br_startblock,
					ino);
				goto done;
			}

			/* No unwritten extents in non-regular files */
			if (type != XR_INO_DATA && type != XR_INO_RTDATA) {
				do_warn(
_("unwritten extent (off = %" PRIu64 ", fsbno = %" PRIu64 ") in non-regular file ino %" PRIu64 "\n"),
					irec.br_startoff,
					irec.br_startblock,
					ino);
				goto done;
			}
		}

		/*
		 * check numeric validity of the extent
		 */
		if (irec.br_blockcount == 0)  {
			do_warn(
_("zero length extent (off = %" PRIu64 ", fsbno = %" PRIu64 ") in ino %" PRIu64 "\n"),
				irec.br_startoff,
				irec.br_startblock,
				ino);
			goto done;
		}

		if (isrt && !xfs_has_rtgroups(mp)) {
			error2 = process_rt_rec(mp, &irec, ino, tot, check_dups,
					zap_metadata);
			if (error2)
				return error2;

			/*
			 * skip rest of loop processing since the rest is
			 * all for regular file forks and attr forks
			 */
			continue;
		}

		/*
		 * regular file data fork or attribute fork
		 */
		switch (verify_dfsbno_range(mp, &irec, isrt)) {
			case XR_DFSBNORANGE_VALID:
				break;

			case XR_DFSBNORANGE_BADSTART:
				do_warn(
_("inode %" PRIu64 " - bad extent starting block number %" PRIu64 ", offset %" PRIu64 "\n"),
					ino,
					irec.br_startblock,
					irec.br_startoff);
				goto done;

			case XR_DFSBNORANGE_BADEND:
				do_warn(
_("inode %" PRIu64 " - bad extent last block number %" PRIu64 ", offset %" PRIu64 "\n"),
					ino,
					irec.br_startblock + irec.br_blockcount - 1,
					irec.br_startoff);
				goto done;

			case XR_DFSBNORANGE_OVERFLOW:
				do_warn(
_("inode %" PRIu64 " - bad extent overflows - start %" PRIu64 ", "
  "end %" PRIu64 ", offset %" PRIu64 "\n"),
					ino,
					irec.br_startblock,
					irec.br_startblock + irec.br_blockcount - 1,
					irec.br_startoff);
				goto done;
		}
		/* Ensure this extent does not extend beyond the max offset */
		if (irec.br_startoff + irec.br_blockcount - 1 >
							XFS_MAX_FILEOFF) {
			do_warn(
_("inode %" PRIu64 " - extent exceeds max offset - start %" PRIu64 ", "
  "count %" PRIu64 ", physical block %" PRIu64 "\n"),
				ino, irec.br_startoff, irec.br_blockcount,
				irec.br_startblock);
			goto done;
		}

		if (blkmapp && *blkmapp) {
			error2 = blkmap_set_ext(blkmapp, irec.br_startoff,
					irec.br_startblock, irec.br_blockcount);
			if (error2) {
				/*
				 * we don't want to clear the inode due to an
				 * internal bmap tracking error, but if we've
				 * run out of memory then we simply can't
				 * validate that the filesystem is consistent.
				 * Hence just abort at this point with an ENOMEM
				 * error.
				 */
				do_abort(
_("Fatal error: inode %" PRIu64 " - blkmap_set_ext(): %s\n"
  "\t%s fork, off - %" PRIu64 ", start - %" PRIu64 ", cnt %" PRIu64 "\n"),
					ino, strerror(error2), forkname,
					irec.br_startoff, irec.br_startblock,
					irec.br_blockcount);
			}
		}

		if (isrt) {
			agno = xfs_rtb_to_rgno(mp, irec.br_startblock);
			first_agbno = xfs_rtb_to_rgbno(mp, irec.br_startblock);
		} else {
			agno = XFS_FSB_TO_AGNO(mp, irec.br_startblock);
			first_agbno = XFS_FSB_TO_AGBNO(mp, irec.br_startblock);
		}
		agbno = first_agbno;
		ebno = first_agbno + irec.br_blockcount;
		if (agno != locked_agno) {
			if (locked_agno != -1)
				unlock_group(locked_agno, isrt);
			locked_agno = agno;
			lock_group(locked_agno, isrt);
		}

		/*
		 * Profiling shows that the following loop takes the most time
		 * in all of xfs_repair.
		 */
		for (b = irec.br_startblock;
		     agbno < ebno;
		     b += blen, agbno += blen) {
			state = get_bmap_ext(agno, agbno, ebno, &blen, isrt);
			switch (state)  {
			case XR_E_FREE:
				/*
				 * We never do a scan pass of the rt bitmap, so unknown
				 * blocks are marked as free.
				 */
				if (isrt)
					break;
				fallthrough;
			case XR_E_FREE1:
				do_warn(
_("%s fork in ino %" PRIu64 " claims free block %" PRIu64 "\n"),
					forkname, ino, (uint64_t) b);
				fallthrough;
			case XR_E_INUSE1:	/* seen by rmap */
			case XR_E_UNKNOWN:
				break;

			case XR_E_BAD_STATE:
				do_error(_("bad state in block map %" PRIu64 "\n"), b);

			case XR_E_FS_MAP1:
			case XR_E_INO1:
			case XR_E_INUSE_FS1:
				do_warn(_("rmap claims metadata use!\n"));
				fallthrough;
			case XR_E_METADATA:
				do_warn(
_("%s fork in inode %" PRIu64 " claims metadata file block %" PRIu64 "\n"),
					forkname, ino, b);
				break;
			case XR_E_FS_MAP:
			case XR_E_INO:
			case XR_E_INUSE_FS:
			case XR_E_REFC:
				do_warn(
_("%s fork in inode %" PRIu64 " claims metadata block %" PRIu64 "\n"),
					forkname, ino, b);
				goto done;

			case XR_E_INUSE:
			case XR_E_MULT:
				if (is_reflink_type(mp, type)) {
					if (irec.br_state == XFS_EXT_NORM)
						break;
					do_warn(
_("%s fork in %s inode %" PRIu64 " claims shared unwritten block %" PRIu64 "\n"),
						forkname, ftype, ino, b);
					goto done;
				}
				do_warn(
_("%s fork in %s inode %" PRIu64 " claims used block %" PRIu64 "\n"),
					forkname, ftype, ino, b);
				goto done;

			case XR_E_COW:
				do_warn(
_("%s fork in %s inode %" PRIu64 " claims CoW block %" PRIu64 "\n"),
					forkname, ftype, ino, b);
				goto done;

			default:
				do_error(
_("illegal state %d in block map %" PRIu64 "\n"),
					state, b);
				goto done;
			}
		}

		if (check_dups) {
			/*
			 * If we're just checking the bmap for dups and we
			 * didn't find any non-reflink collisions, update our
			 * inode's block count and move on to the next extent.
			 * We're not yet updating the block usage information.
			 */
			*tot += irec.br_blockcount;
			continue;
		}

		/*
		 * Update the internal extent map only after we've checked
		 * every block in this extent.  The first time we reject this
		 * data fork we'll try to rebuild the bmbt from rmap data.
		 * After a successful rebuild we'll try this scan again.
		 * (If the rebuild fails we won't come back here.)
		 */
		agbno = first_agbno;
		ebno = first_agbno + irec.br_blockcount;
		for (; agbno < ebno; agbno += blen) {
			state = get_bmap_ext(agno, agbno, ebno, &blen, isrt);
			switch (state)  {
			case XR_E_METADATA:
				/*
				 * The entire metadata directory tree is rebuilt
				 * every time, so we can let regular files take
				 * ownership of this block.
				 */
				if (zap_metadata)
					break;
				fallthrough;
			case XR_E_FREE:
			case XR_E_FREE1:
			case XR_E_INUSE1:
			case XR_E_UNKNOWN:
				set_bmap_ext(agno, agbno, blen,
					zap_metadata ?
					XR_E_METADATA : XR_E_INUSE, isrt);
				break;

			case XR_E_INUSE:
			case XR_E_MULT:
				if (!zap_metadata)
					set_bmap_ext(agno, agbno, blen,
							XR_E_MULT, isrt);
				break;
			default:
				break;
			}
		}
		if (collect_rmaps && !zap_metadata) /* && !check_dups */
			rmap_add_rec(mp, ino, whichfork, &irec, isrt);
		*tot += irec.br_blockcount;
	}
	error = 0;
done:
	if (locked_agno != -1)
		unlock_group(locked_agno, isrt);

	if (i != *numrecs) {
		ASSERT(i < *numrecs);
		do_warn(_("correcting nextents for inode %" PRIu64 "\n"), ino);
		*numrecs = i;
	}

	return error;
}

/*
 * return 1 if inode should be cleared, 0 otherwise, sets block bitmap
 * as a side-effect
 */
int
process_bmbt_reclist(
	xfs_mount_t		*mp,
	xfs_bmbt_rec_t		*rp,
	xfs_extnum_t		*numrecs,
	int			type,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	blkmap_t		**blkmapp,
	xfs_fileoff_t		*first_key,
	xfs_fileoff_t		*last_key,
	int			whichfork,
	bool			zap_metadata)
{
	return process_bmbt_reclist_int(mp, rp, numrecs, type, ino, tot,
				blkmapp, first_key, last_key, 0, whichfork,
				zap_metadata);
}

/*
 * return 1 if inode should be cleared, 0 otherwise, does not set
 * block bitmap
 */
int
scan_bmbt_reclist(
	xfs_mount_t		*mp,
	xfs_bmbt_rec_t		*rp,
	xfs_extnum_t		*numrecs,
	int			type,
	xfs_ino_t		ino,
	xfs_rfsblock_t		*tot,
	int			whichfork,
	bool			zap_metadata)
{
	xfs_fileoff_t		first_key = 0;
	xfs_fileoff_t		last_key = 0;

	return process_bmbt_reclist_int(mp, rp, numrecs, type, ino, tot,
				NULL, &first_key, &last_key, 1, whichfork,
				zap_metadata);
}

/*
 * Grab the buffer backing an inode.  This is meant for routines that
 * work with inodes one at a time in any order (like walking the
 * unlinked lists to look for inodes).  The caller is responsible for
 * writing/releasing the buffer.
 */
struct xfs_buf *
get_agino_buf(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		agino,
	struct xfs_dinode	**dipp)
{
	struct xfs_buf		*bp;
	xfs_agino_t		cluster_agino;
	xfs_daddr_t		cluster_daddr;
	xfs_daddr_t		cluster_blks;
	struct xfs_ino_geometry	*igeo = M_IGEO(mp);
	int			error;

	/*
	 * Inode buffers have been read into memory in inode_cluster_size
	 * chunks (or one FSB).  To find the correct buffer for an inode,
	 * we must find the buffer for its cluster, add the appropriate
	 * offset, and return that.
	 */
	cluster_agino = agino & ~(igeo->inodes_per_cluster - 1);
	cluster_blks = XFS_FSB_TO_DADDR(mp, igeo->blocks_per_cluster);
	cluster_daddr = XFS_AGB_TO_DADDR(mp, agno,
			XFS_AGINO_TO_AGBNO(mp, cluster_agino));

#ifdef XR_INODE_TRACE
	printf("cluster_size %d ipc %d clusagino %d daddr %lld sectors %lld\n",
		M_IGEO(mp)->inode_cluster_size, M_IGEO(mp)->inodes_per_cluster,
		cluster_agino, cluster_daddr, cluster_blks);
#endif

	error = -libxfs_buf_read(mp->m_dev, cluster_daddr, cluster_blks, 0,
			&bp, &xfs_inode_buf_ops);
	if (error) {
		do_warn(_("cannot read inode (%u/%u), disk block %" PRIu64 "\n"),
			agno, cluster_agino, cluster_daddr);
		return NULL;
	}

	*dipp = xfs_make_iptr(mp, bp, agino - cluster_agino);
	ASSERT(!xfs_has_crc(mp) ||
			XFS_AGINO_TO_INO(mp, agno, agino) ==
			be64_to_cpu((*dipp)->di_ino));
	return bp;
}

static inline xfs_rgnumber_t
metafile_rgnumber(
	const struct xfs_dinode	*dip)
{
	return (xfs_rgnumber_t)be16_to_cpu(dip->di_projid_hi) << 16 |
			       be16_to_cpu(dip->di_projid_lo);
}

/*
 * higher level inode processing stuff starts here:
 * first, one utility routine for each type of inode
 */

/*
 * return 1 if inode should be cleared, 0 otherwise
 */
static int
process_rtrmap(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	struct xfs_dinode	*dip,
	int			type,
	int			*dirty,
	xfs_rfsblock_t		*tot,
	uint64_t		*nex,
	blkmap_t		**blkmapp,
	int			check_dups)
{
	struct xfs_rmap_irec	oldkey;
	struct xfs_rmap_irec	key;
	struct rmap_priv	priv;
	struct xfs_rtrmap_root	*dib;
	xfs_rtrmap_ptr_t	*pp;
	struct xfs_rmap_key	*kp;
	struct xfs_rmap_rec	*rp;
	char			*forkname = get_forkname(XFS_DATA_FORK);
	xfs_ino_t		lino;
	xfs_fsblock_t		bno;
	size_t			droot_sz;
	int			i;
	int			level;
	int			numrecs;
	int			dmxr;
	int			suspect = 0;
	int			error;

	/* We rebuild the rtrmapbt, so no need to process blocks again. */
	if (check_dups) {
		*tot = be64_to_cpu(dip->di_nblocks);
		return 0;
	}

	lino = XFS_AGINO_TO_INO(mp, agno, ino);

	/*
	 * This rmap btree inode must be a metadata inode reachable via
	 * /rtgroups/$rgno.rmap in the metadata directory tree.
	 */
	if (!(dip->di_flags2 & be64_to_cpu(XFS_DIFLAG2_METADATA))) {
		do_warn(
_("rtrmap inode %" PRIu64 " not flagged as metadata\n"),
			lino);
		return 1;
	}

	/*
	 * If this rtrmap file claims to be from an rtgroup that actually
	 * exists, check that inode discovery actually found it.  Note that
	 * we can have stray rtrmap files from failed growfsrt operations.
	 */
	if (metafile_rgnumber(dip) < mp->m_sb.sb_rgcount) {
		if (type != XR_INO_RTRMAP) {
			do_warn(
_("rtrmap inode %" PRIu64 " was not found in the metadata directory tree\n"),
				lino);
			return 1;
		}

		if (!is_rtrmap_inode(lino)) {
			do_warn(
_("could not associate rtrmap inode %" PRIu64 " with any rtgroup\n"),
				lino);
			return 1;
		}
	}

	memset(&priv.high_key, 0xFF, sizeof(priv.high_key));
	priv.high_key.rm_blockcount = 0;
	priv.agcnts = NULL;
	priv.last_rec.rm_owner = XFS_RMAP_OWN_UNKNOWN;

	dib = (struct xfs_rtrmap_root *)XFS_DFORK_PTR(dip, XFS_DATA_FORK);
	*tot = 0;
	*nex = 0;

	level = be16_to_cpu(dib->bb_level);
	numrecs = be16_to_cpu(dib->bb_numrecs);

	if (level > mp->m_rtrmap_maxlevels) {
		do_warn(
_("bad level %d in inode %" PRIu64 " rtrmap btree root block\n"),
			level, lino);
		return 1;
	}

	/*
	 * use rtroot/dfork_dsize since the root block is in the data fork
	 */
	droot_sz = xfs_rtrmap_droot_space_calc(level, numrecs);
	if (droot_sz > XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK)) {
		do_warn(
_("computed size of rtrmapbt root (%zu bytes) is greater than space in "
	  "inode %" PRIu64 " %s fork\n"),
				droot_sz, lino, forkname);
		return 1;
	}

	if (level == 0) {
		rp = xfs_rtrmap_droot_rec_addr(dib, 1);
		error = process_rtrmap_reclist(mp, rp, numrecs,
				&priv.last_rec, NULL, "rtrmapbt root");
		if (error) {
			rmap_avoid_check(mp);
			return 1;
		}
		return 0;
	}

	dmxr = libxfs_rtrmapbt_droot_maxrecs(
			XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK), false);
	pp = xfs_rtrmap_droot_ptr_addr(dib, 1, dmxr);

	/* check for in-order keys */
	for (i = 0; i < numrecs; i++)  {
		kp = xfs_rtrmap_droot_key_addr(dib, i + 1);

		key.rm_flags = 0;
		key.rm_startblock = be32_to_cpu(kp->rm_startblock);
		key.rm_owner = be64_to_cpu(kp->rm_owner);
		if (libxfs_rmap_irec_offset_unpack(be64_to_cpu(kp->rm_offset),
				&key)) {
			/* Look for impossible flags. */
			do_warn(
_("invalid flags in key %u of rtrmap root ino %" PRIu64 "\n"),
				i, lino);
			suspect++;
			continue;
		}
		if (i == 0) {
			oldkey = key;
			continue;
		}
		if (rmap_diffkeys(&oldkey, &key) > 0) {
			do_warn(
_("out of order key %u in rtrmap root ino %" PRIu64 "\n"),
				i, lino);
			suspect++;
			continue;
		}
		oldkey = key;
	}

	/* probe keys */
	for (i = 0; i < numrecs; i++)  {
		bno = get_unaligned_be64(&pp[i]);

		if (!libxfs_verify_fsbno(mp, bno))  {
			do_warn(
_("bad rtrmap btree ptr 0x%" PRIx64 " in ino %" PRIu64 "\n"),
				bno, lino);
			return 1;
		}

		if (scan_lbtree(bno, level, scan_rtrmapbt,
				type, XFS_DATA_FORK, lino, tot, nex, blkmapp,
				NULL, 0, 1, check_dups, XFS_RTRMAP_CRC_MAGIC,
				&priv, &xfs_rtrmapbt_buf_ops))
			return 1;
	}

	return suspect ? 1 : 0;
}

/*
 * return 1 if inode should be cleared, 0 otherwise
 */
static int
process_rtrefc(
	struct xfs_mount		*mp,
	xfs_agnumber_t			agno,
	xfs_agino_t			ino,
	struct xfs_dinode		*dip,
	int				type,
	int				*dirty,
	xfs_rfsblock_t			*tot,
	uint64_t			*nex,
	blkmap_t			**blkmapp,
	int				check_dups)
{
	struct refc_priv		priv = { .nr_blocks = 0 };
	struct xfs_rtrefcount_root	*dib;
	xfs_rtrefcount_ptr_t		*pp;
	struct xfs_refcount_key		*kp;
	struct xfs_refcount_rec		*rp;
	char				*forkname = get_forkname(XFS_DATA_FORK);
	xfs_rgblock_t			oldkey, key;
	xfs_ino_t			lino;
	xfs_fsblock_t			bno;
	size_t				droot_sz;
	int				i;
	int				level;
	int				numrecs;
	int				dmxr;
	int				suspect = 0;
	int				error;

	/* We rebuild the rtrefcountbt, so no need to process blocks again. */
	if (check_dups) {
		*tot = be64_to_cpu(dip->di_nblocks);
		return 0;
	}

	lino = XFS_AGINO_TO_INO(mp, agno, ino);

	/*
	 * This refcount btree inode must be a metadata inode reachable via
	 * /rtgroups/$rgno.refcount in the metadata directory tree.
	 */
	if (!(dip->di_flags2 & be64_to_cpu(XFS_DIFLAG2_METADATA))) {
		do_warn(
_("rtrefcount inode %" PRIu64 " not flagged as metadata\n"),
			lino);
		return 1;
	}

	/*
	 * If this rtrefcount file claims to be from an rtgroup that actually
	 * exists, check that inode discovery actually found it.  Note that
	 * we can have stray rtrefcount files from failed growfsrt operations.
	 */
	priv.rgno = metafile_rgnumber(dip);
	if (priv.rgno < mp->m_sb.sb_rgcount) {
		if (type != XR_INO_RTREFC) {
			do_warn(
_("rtrefcount inode %" PRIu64 " was not found in the metadata directory tree\n"),
				lino);
			return 1;
		}

		if (!is_rtrefcount_inode(lino)) {
			do_warn(
_("could not associate refcount inode %" PRIu64 " with any rtgroup\n"),
				lino);
			return 1;
		}
	}

	dib = (struct xfs_rtrefcount_root *)XFS_DFORK_PTR(dip, XFS_DATA_FORK);
	*tot = 0;
	*nex = 0;

	level = be16_to_cpu(dib->bb_level);
	numrecs = be16_to_cpu(dib->bb_numrecs);

	if (level > mp->m_rtrefc_maxlevels) {
		do_warn(
_("bad level %d in inode %" PRIu64 " rtrefcount btree root block\n"),
			level, lino);
		return 1;
	}

	/*
	 * use rtroot/dfork_dsize since the root block is in the data fork
	 */
	droot_sz = xfs_rtrefcount_droot_space_calc(level, numrecs);
	if (droot_sz > XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK)) {
		do_warn(
_("computed size of rtrefcountbt root (%zu bytes) is greater than space in "
	  "inode %" PRIu64 " %s fork\n"),
				droot_sz, lino, forkname);
		return 1;
	}

	if (level == 0) {
		rp = xfs_rtrefcount_droot_rec_addr(dib, 1);
		error = process_rtrefc_reclist(mp, rp, numrecs,
				&priv, "rtrefcountbt root");
		if (error) {
			refcount_avoid_check(mp);
			return 1;
		}
		return 0;
	}

	dmxr = libxfs_rtrefcountbt_droot_maxrecs(
			XFS_DFORK_SIZE(dip, mp, XFS_DATA_FORK), false);
	pp = xfs_rtrefcount_droot_ptr_addr(dib, 1, dmxr);

	/* check for in-order keys */
	for (i = 0; i < numrecs; i++)  {
		kp = xfs_rtrefcount_droot_key_addr(dib, i + 1);

		key = be32_to_cpu(kp->rc_startblock);
		if (i == 0) {
			oldkey = key;
			continue;
		}
		if (key < oldkey) {
			do_warn(
_("out of order key %u in rtrefcount root ino %" PRIu64 "\n"),
				i, lino);
			suspect++;
			continue;
		}
		oldkey = key;
	}

	/* probe keys */
	for (i = 0; i < numrecs; i++)  {
		bno = get_unaligned_be64(&pp[i]);

		if (!libxfs_verify_fsbno(mp, bno))  {
			do_warn(
_("bad rtrefcount btree ptr 0x%" PRIx64 " in ino %" PRIu64 "\n"),
				bno, lino);
			return 1;
		}

		if (scan_lbtree(bno, level, scan_rtrefcbt,
				type, XFS_DATA_FORK, lino, tot, nex, blkmapp,
				NULL, 0, 1, check_dups, XFS_RTREFC_CRC_MAGIC,
				&priv, &xfs_rtrefcountbt_buf_ops))
			return 1;
	}

	*tot = priv.nr_blocks;
	return suspect ? 1 : 0;
}

/*
 * return 1 if inode should be cleared, 0 otherwise
 */
static int
process_btinode(
	xfs_mount_t		*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	struct xfs_dinode	*dip,
	int			type,
	int			*dirty,
	xfs_rfsblock_t		*tot,
	xfs_extnum_t		*nex,
	blkmap_t		**blkmapp,
	int			whichfork,
	int			check_dups,
	bool			zap_metadata)
{
	xfs_bmdr_block_t	*dib;
	xfs_fileoff_t		last_key;
	xfs_fileoff_t		first_key = 0;
	xfs_ino_t		lino;
	xfs_bmbt_ptr_t		*pp;
	xfs_bmbt_key_t		*pkey;
	char			*forkname = get_forkname(whichfork);
	int			i;
	int			level;
	int			numrecs;
	bmap_cursor_t		cursor;
	uint64_t		magic;

	dib = (xfs_bmdr_block_t *)XFS_DFORK_PTR(dip, whichfork);
	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	*tot = 0;
	*nex = 0;

	magic = xfs_has_crc(mp) ? XFS_BMAP_CRC_MAGIC
						 : XFS_BMAP_MAGIC;

	level = be16_to_cpu(dib->bb_level);
	numrecs = be16_to_cpu(dib->bb_numrecs);

	if ((level == 0) || (level > XFS_BM_MAXLEVELS(mp, whichfork))) {
		/*
		 * XXX - if we were going to fix up the inode,
		 * we'd try to treat the fork as an interior
		 * node and see if we could get an accurate
		 * level value from one of the blocks pointed
		 * to by the pointers in the fork.  For now
		 * though, we just bail (and blow out the inode).
		 */
		do_warn(
_("bad level %d in inode %" PRIu64 " bmap btree root block\n"),
			level, XFS_AGINO_TO_INO(mp, agno, ino));
		return(1);
	}
	if (numrecs == 0) {
		do_warn(
_("bad numrecs 0 in inode %" PRIu64 " bmap btree root block\n"),
			XFS_AGINO_TO_INO(mp, agno, ino));
		return(1);
	}
	/*
	 * use bmdr/dfork_dsize since the root block is in the data fork
	 */
	if (xfs_bmdr_space_calc(numrecs) > XFS_DFORK_SIZE(dip, mp, whichfork)) {
		do_warn(
	_("indicated size of %s btree root (%zu bytes) greater than space in "
	  "inode %" PRIu64 " %s fork\n"),
			forkname, xfs_bmdr_space_calc(numrecs), lino, forkname);
		return(1);
	}

	init_bm_cursor(&cursor, level + 1);

	pp = xfs_bmdr_ptr_addr(dib, 1,
		libxfs_bmdr_maxrecs(XFS_DFORK_SIZE(dip, mp, whichfork), 0));
	pkey = xfs_bmdr_key_addr(dib, 1);
	last_key = NULLFILEOFF;

	for (i = 0; i < numrecs; i++)  {
		/*
		 * XXX - if we were going to do more to fix up the inode
		 * btree, we'd do it right here.  For now, if there's a
		 * problem, we'll bail out and presumably clear the inode.
		 */
		if (!libxfs_verify_fsbno(mp, get_unaligned_be64(&pp[i])))  {
			do_warn(
_("bad bmap btree ptr 0x%" PRIx64 " in ino %" PRIu64 "\n"),
				get_unaligned_be64(&pp[i]), lino);
			return(1);
		}

		if (scan_lbtree(get_unaligned_be64(&pp[i]), level, scan_bmapbt,
				type, whichfork, lino, tot, nex, blkmapp,
				&cursor, 0, 1, check_dups, magic,
				(void *)zap_metadata, &xfs_bmbt_buf_ops))
			return(1);
		/*
		 * fix key (offset) mismatches between the keys in root
		 * block records and the first key of each child block.
		 * fixes cases where entries have been shifted between
		 * blocks but the parent hasn't been updated
		 */
		if (!check_dups && cursor.level[level-1].first_key !=
				   get_unaligned_be64(&pkey[i].br_startoff)) {
			if (!no_modify)  {
				do_warn(
_("correcting key in bmbt root (was %" PRIu64 ", now %" PRIu64") in inode "
  "%" PRIu64" %s fork\n"),
				       get_unaligned_be64(&pkey[i].br_startoff),
				       cursor.level[level-1].first_key,
				       XFS_AGINO_TO_INO(mp, agno, ino),
				       forkname);
				*dirty = 1;
				put_unaligned_be64(
					cursor.level[level-1].first_key,
					&pkey[i].br_startoff);
			} else  {
				do_warn(
_("bad key in bmbt root (is %" PRIu64 ", would reset to %" PRIu64 ") in inode "
  "%" PRIu64 " %s fork\n"),
				       get_unaligned_be64(&pkey[i].br_startoff),
				       cursor.level[level-1].first_key,
				       XFS_AGINO_TO_INO(mp, agno, ino),
				       forkname);
			}
		}
		/*
		 * make sure that keys are in ascending order.  blow out
		 * inode if the ordering doesn't hold
		 */
		if (check_dups == 0)  {
			if (last_key != NULLFILEOFF && last_key >=
			    cursor.level[level-1].first_key)  {
				do_warn(
	_("out of order bmbt root key %" PRIu64 " in inode %" PRIu64 " %s fork\n"),
					first_key,
					XFS_AGINO_TO_INO(mp, agno, ino),
					forkname);
				return(1);
			}
			last_key = cursor.level[level-1].first_key;
		}
	}
	/*
	 * Ideally if all the extents are ok (perhaps after further
	 * checks below?) we'd just move this back into extents format.
	 * But for now clear it, as the kernel will choke on this
	 */
	if (*nex <= XFS_DFORK_SIZE(dip, mp, whichfork) /
			sizeof(xfs_bmbt_rec_t)) {
		do_warn(
	_("extent count for ino %" PRIu64 " %s fork too low (%" PRIu64 ") for file format\n"),
				lino, forkname, (uint64_t)*nex);
		return(1);
	}
	/*
	 * Check that the last child block's forward sibling pointer
	 * is NULL.
	 */
	if (check_dups == 0 &&
		cursor.level[0].right_fsbno != NULLFSBLOCK)  {
		do_warn(
	_("bad fwd (right) sibling pointer (saw %" PRIu64 " should be NULLFSBLOCK)\n"),
			cursor.level[0].right_fsbno);
		do_warn(
	_("\tin inode %" PRIu64 " (%s fork) bmap btree block %" PRIu64 "\n"),
			XFS_AGINO_TO_INO(mp, agno, ino), forkname,
			cursor.level[0].fsbno);
		return(1);
	}

	return(0);
}

/*
 * return 1 if inode should be cleared, 0 otherwise
 */
static int
process_exinode(
	xfs_mount_t		*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	struct xfs_dinode	*dip,
	int			type,
	int			*dirty,
	xfs_rfsblock_t		*tot,
	xfs_extnum_t		*nex,
	blkmap_t		**blkmapp,
	int			whichfork,
	int			check_dups,
	bool			zap_metadata)
{
	xfs_ino_t		lino;
	xfs_bmbt_rec_t		*rp;
	xfs_fileoff_t		first_key;
	xfs_fileoff_t		last_key;
	xfs_extnum_t		numrecs, max_numrecs;
	int			ret;

	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	rp = (xfs_bmbt_rec_t *)XFS_DFORK_PTR(dip, whichfork);
	*tot = 0;
	numrecs = xfs_dfork_nextents(dip, whichfork);

	/*
	 * We've already decided on the maximum number of extents on the inode,
	 * and numrecs may be corrupt. Hence make sure we only allow numrecs to
	 * be in the range of valid on-disk numbers, which is:
	 *	0 < numrecs < 2^31 - 1
	 */
	max_numrecs = xfs_iext_max_nextents(
			xfs_dinode_has_large_extent_counts(dip),
			whichfork);
	if (numrecs > max_numrecs)
		numrecs = *nex;

	/*
	 * XXX - if we were going to fix up the btree record,
	 * we'd do it right here.  For now, if there's a problem,
	 * we'll bail out and presumably clear the inode.
	 */
	if (check_dups == 0)
		ret = process_bmbt_reclist(mp, rp, &numrecs, type, lino,
					tot, blkmapp, &first_key, &last_key,
					whichfork, zap_metadata);
	else
		ret = scan_bmbt_reclist(mp, rp, &numrecs, type, lino, tot,
					whichfork, zap_metadata);

	*nex = numrecs;
	return ret;
}

/*
 * return 1 if inode should be cleared, 0 otherwise
 */
static int
process_lclinode(
	xfs_mount_t			*mp,
	xfs_agnumber_t			agno,
	xfs_agino_t			ino,
	struct xfs_dinode		*dip,
	int				whichfork)
{
	struct xfs_attr_sf_hdr		*hdr;
	xfs_ino_t			lino;

	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	if (whichfork == XFS_DATA_FORK && be64_to_cpu(dip->di_size) >
						XFS_DFORK_DSIZE(dip, mp)) {
		do_warn(
	_("local inode %" PRIu64 " data fork is too large (size = %lld, max = %zu)\n"),
		       lino, (unsigned long long) be64_to_cpu(dip->di_size),
			XFS_DFORK_DSIZE(dip, mp));
		return(1);
	} else if (whichfork == XFS_ATTR_FORK) {
		hdr = XFS_DFORK_APTR(dip);

		if (be16_to_cpu(hdr->totsize) > XFS_DFORK_ASIZE(dip, mp)) {
			do_warn(
	_("local inode %" PRIu64 " attr fork too large (size %d, max = %zu)\n"),
				lino, be16_to_cpu(hdr->totsize),
				XFS_DFORK_ASIZE(dip, mp));
			return(1);
		}
		if (be16_to_cpu(hdr->totsize) < sizeof(struct xfs_attr_sf_hdr)) {
			do_warn(
	_("local inode %" PRIu64 " attr too small (size = %d, min size = %zd)\n"),
				lino, be16_to_cpu(hdr->totsize),
				sizeof(struct xfs_attr_sf_hdr));
			return(1);
		}
	}

	return(0);
}

static int
process_symlink_extlist(
	xfs_mount_t		*mp,
	xfs_ino_t		lino,
	struct xfs_dinode	*dino)
{
	xfs_fileoff_t		expected_offset;
	xfs_bmbt_rec_t		*rp;
	xfs_bmbt_irec_t		irec;
	xfs_extnum_t		numrecs;
	xfs_extnum_t		i;
	int			max_blocks;

	if (be64_to_cpu(dino->di_size) <= XFS_DFORK_DSIZE(dino, mp)) {
		if (dino->di_format == XFS_DINODE_FMT_LOCAL ||
		    dino->di_format == XFS_DINODE_FMT_EXTENTS)
			return 0;
		do_warn(
_("mismatch between format (%d) and size (%" PRId64 ") in symlink ino %" PRIu64 "\n"),
			dino->di_format,
			(int64_t)be64_to_cpu(dino->di_size), lino);
		return 1;
	}
	if (dino->di_format == XFS_DINODE_FMT_LOCAL) {
		do_warn(
_("mismatch between format (%d) and size (%" PRId64 ") in symlink inode %" PRIu64 "\n"),
			dino->di_format,
			(int64_t)be64_to_cpu(dino->di_size), lino);
		return 1;
	}

	rp = (xfs_bmbt_rec_t *)XFS_DFORK_DPTR(dino);
	numrecs = xfs_dfork_data_extents(dino);

	/*
	 * the max # of extents in a symlink inode is equal to the
	 * number of max # of blocks required to store the symlink
	 */
	if (numrecs > max_symlink_blocks)  {
		do_warn(
_("bad number of extents (%" PRIu64 ") in symlink %" PRIu64 " data fork\n"),
			numrecs, lino);
		return(1);
	}

	max_blocks = max_symlink_blocks;
	expected_offset = 0;

	for (i = 0; i < numrecs; i++)  {
		libxfs_bmbt_disk_get_all((rp +i), &irec);
		if (irec.br_startoff != expected_offset)  {
			do_warn(
_("bad extent #%" PRIu64 " offset (%" PRIu64 ") in symlink %" PRIu64 " data fork\n"),
				i, irec.br_startoff, lino);
			return(1);
		}
		if (irec.br_blockcount == 0 || irec.br_blockcount > max_blocks) {
			do_warn(
_("bad extent #%" PRIu64 " count (%" PRIu64 ") in symlink %" PRIu64 " data fork\n"),
				i, irec.br_blockcount, lino);
			return(1);
		}

		max_blocks -= irec.br_blockcount;
		expected_offset += irec.br_blockcount;
	}

	return(0);
}

/*
 * takes a name and length and returns 1 if the name contains
 * a \0, returns 0 otherwise
 */
static int
null_check(char *name, int length)
{
	int i;

	ASSERT(length < XFS_SYMLINK_MAXLEN);

	for (i = 0; i < length; i++, name++)  {
		if (*name == '\0')
			return(1);
	}

	return(0);
}

/*
 * This does /not/ do quotacheck, it validates the basic quota
 * inode metadata, checksums, etc.
 */
#define uuid_equal(s,d) (platform_uuid_compare((s),(d)) == 0)
static int
process_quota_inode(
	struct xfs_mount	*mp,
	xfs_ino_t		lino,
	struct xfs_dinode	*dino,
	uint			ino_type,
	struct blkmap		*blkmap)
{
	xfs_fsblock_t		fsbno;
	struct xfs_buf		*bp;
	xfs_filblks_t		dqchunklen;
	uint			dqperchunk;
	int			quota_type = 0;
	char			*quota_string = NULL;
	xfs_dqid_t		dqid;
	xfs_fileoff_t		qbno;
	int			i;
	xfs_extnum_t		t = 0;
	int			error;

	switch (ino_type) {
		case XR_INO_UQUOTA:
			quota_type = XFS_DQTYPE_USER;
			quota_string = _("User quota");
			break;
		case XR_INO_GQUOTA:
			quota_type = XFS_DQTYPE_GROUP;
			quota_string = _("Group quota");
			break;
		case XR_INO_PQUOTA:
			quota_type = XFS_DQTYPE_PROJ;
			quota_string = _("Project quota");
			break;
		default:
			ASSERT(0);
	}

	dqchunklen = XFS_FSB_TO_BB(mp, XFS_DQUOT_CLUSTER_SIZE_FSB);
	dqperchunk = libxfs_calc_dquots_per_chunk(dqchunklen);
	dqid = 0;
	qbno = NULLFILEOFF;

	while ((qbno = blkmap_next_off(blkmap, qbno, &t)) != NULLFILEOFF) {
		struct xfs_dqblk	*dqb;
		int			writebuf = 0;

		fsbno = blkmap_get(blkmap, qbno);
		dqid = (xfs_dqid_t)qbno * dqperchunk;

		error = -libxfs_buf_read(mp->m_dev,
				XFS_FSB_TO_DADDR(mp, fsbno), dqchunklen,
				LIBXFS_READBUF_SALVAGE, &bp,
				&xfs_dquot_buf_ops);
		if (error) {
			do_warn(
_("cannot read inode %" PRIu64 ", file block %" PRIu64 ", disk block %" PRIu64 "\n"),
				lino, qbno, fsbno);
			return 1;
		}

		dqb = bp->b_addr;
		for (i = 0; i < dqperchunk; i++, dqid++, dqb++) {
			int		bad_dqb = 0;

			/* We only print the first problem we find */
			if (xfs_has_crc(mp)) {
				if (!libxfs_verify_cksum((char *)dqb,
							sizeof(*dqb),
							XFS_DQUOT_CRC_OFF)) {
					do_warn(_("%s: bad CRC for id %u. "),
							quota_string, dqid);
					bad_dqb = 1;
					goto bad;
				}

				if (!uuid_equal(&dqb->dd_uuid,
						&mp->m_sb.sb_meta_uuid)) {
					do_warn(_("%s: bad UUID for id %u. "),
							quota_string, dqid);
					bad_dqb = 1;
					goto bad;
				}
			}
			if (libxfs_dquot_verify(mp, &dqb->dd_diskdq, dqid)
						!= NULL ||
			    (dqb->dd_diskdq.d_type & XFS_DQTYPE_REC_MASK)
						!= quota_type) {
				do_warn(_("%s: Corrupt quota for id %u. "),
						quota_string, dqid);
				bad_dqb = 1;
			}

bad:
			if (bad_dqb) {
				if (no_modify)
					do_warn(_("Would correct.\n"));
				else {
					do_warn(_("Corrected.\n"));
					libxfs_dqblk_repair(mp, dqb,
							    dqid, quota_type);
					writebuf = 1;
				}
			}
		}

		if (writebuf && !no_modify) {
			libxfs_buf_mark_dirty(bp);
			libxfs_buf_relse(bp);
		}
		else
			libxfs_buf_relse(bp);
	}
	return 0;
}

static int
process_symlink_remote(
	struct xfs_mount	*mp,
	xfs_ino_t		lino,
	struct xfs_dinode	*dino,
	struct blkmap		*blkmap,
	char			*dst)
{
	xfs_fsblock_t		fsbno;
	struct xfs_buf		*bp;
	char			*src;
	int			pathlen;
	int			offset;
	int			i;
	int			error;

	offset = 0;
	pathlen = be64_to_cpu(dino->di_size);
	i = 0;

	while (pathlen > 0) {
		int	blk_cnt = 1;
		int	byte_cnt;
		int	badcrc = 0;

		fsbno = blkmap_get(blkmap, i);
		if (fsbno == NULLFSBLOCK) {
			do_warn(
_("cannot read inode %" PRIu64 ", file block %d, NULL disk block\n"),
				lino, i);
			return 1;
		}

		/*
		 * There's a symlink header for each contiguous extent. If
		 * there are contiguous blocks, read them in one go.
		 */
		while (blk_cnt <= max_symlink_blocks) {
			if (blkmap_get(blkmap, i + 1) != fsbno + 1)
				break;
			blk_cnt++;
			i++;
		}

		byte_cnt = XFS_FSB_TO_B(mp, blk_cnt);

		error = -libxfs_buf_read(mp->m_dev,
				XFS_FSB_TO_DADDR(mp, fsbno), BTOBB(byte_cnt),
				LIBXFS_READBUF_SALVAGE, &bp,
				&xfs_symlink_buf_ops);
		if (error) {
			do_warn(
_("cannot read inode %" PRIu64 ", file block %d, disk block %" PRIu64 "\n"),
				lino, i, fsbno);
			return 1;
		}
		if (bp->b_error == -EFSCORRUPTED) {
			do_warn(
_("Corrupt symlink remote block %" PRIu64 ", inode %" PRIu64 ".\n"),
				fsbno, lino);
			libxfs_buf_relse(bp);
			return 1;
		}
		if (bp->b_error == -EFSBADCRC) {
			do_warn(
_("Bad symlink buffer CRC, block %" PRIu64 ", inode %" PRIu64 ".\n"
  "Correcting CRC, but symlink may be bad.\n"), fsbno, lino);
			badcrc = 1;
		}

		byte_cnt = XFS_SYMLINK_BUF_SPACE(mp, byte_cnt);
		byte_cnt = min(pathlen, byte_cnt);

		src = bp->b_addr;
		if (xfs_has_crc(mp)) {
			if (!libxfs_symlink_hdr_ok(lino, offset,
						   byte_cnt, bp)) {
				do_warn(
_("bad symlink header ino %" PRIu64 ", file block %d, disk block %" PRIu64 "\n"),
					lino, i, fsbno);
				libxfs_buf_relse(bp);
				return 1;
			}
			src += sizeof(struct xfs_dsymlink_hdr);
		}

		memmove(dst + offset, src, byte_cnt);

		pathlen -= byte_cnt;
		offset += byte_cnt;
		i++;

		if (badcrc && !no_modify) {
			libxfs_buf_mark_dirty(bp);
			libxfs_buf_relse(bp);
		}
		else
			libxfs_buf_relse(bp);
	}
	return 0;
}

/*
 * like usual, returns 0 if everything's ok and 1 if something's
 * bogus
 */
static int
process_symlink(
	xfs_mount_t		*mp,
	xfs_ino_t		lino,
	struct xfs_dinode	*dino,
	blkmap_t 		*blkmap)
{
	char			*symlink;
	char			data[XFS_SYMLINK_MAXLEN];

	/*
	 * check size against kernel symlink limits.  we know
	 * size is consistent with inode storage format -- e.g.
	 * the inode is structurally ok so we don't have to check
	 * for that
	 */
	if (be64_to_cpu(dino->di_size) >= XFS_SYMLINK_MAXLEN)  {
	       do_warn(_("symlink in inode %" PRIu64 " too long (%llu chars)\n"),
		       lino, (unsigned long long) be64_to_cpu(dino->di_size));
		return(1);
	}

	if (be64_to_cpu(dino->di_size) == 0) {
		do_warn(_("zero size symlink in inode %" PRIu64 "\n"), lino);
		return 1;
	}

	/*
	 * have to check symlink component by component.
	 * get symlink contents into data area
	 */
	symlink = &data[0];
	if (dino->di_format == XFS_DINODE_FMT_LOCAL) {
		/*
		 * local symlink, just copy the symlink out of the
		 * inode into the data area
		 */
		memmove(symlink, XFS_DFORK_DPTR(dino),
						be64_to_cpu(dino->di_size));
	} else {
		int error;

		error = process_symlink_remote(mp, lino, dino, blkmap, symlink);
		if (error)
			return error;
	}

	data[be64_to_cpu(dino->di_size)] = '\0';

	/*
	 * check for nulls
	 */
	if (null_check(symlink, be64_to_cpu(dino->di_size)))  {
		do_warn(
_("found illegal null character in symlink inode %" PRIu64 "\n"),
			lino);
		return(1);
	}

	return(0);
}

/*
 * called to process the set of misc inode special inode types
 * that have no associated data storage (fifos, pipes, devices, etc.).
 */
static int
process_misc_ino_types(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		lino,
	int			type)
{
	/*
	 * must also have a zero size
	 */
	if (be64_to_cpu(dino->di_size) != 0)  {
		switch (type)  {
		case XR_INO_CHRDEV:
			do_warn(
_("size of character device inode %" PRIu64 " != 0 (%" PRId64 " bytes)\n"), lino,
				(int64_t)be64_to_cpu(dino->di_size));
			break;
		case XR_INO_BLKDEV:
			do_warn(
_("size of block device inode %" PRIu64 " != 0 (%" PRId64 " bytes)\n"), lino,
				(int64_t)be64_to_cpu(dino->di_size));
			break;
		case XR_INO_SOCK:
			do_warn(
_("size of socket inode %" PRIu64 " != 0 (%" PRId64 " bytes)\n"), lino,
				(int64_t)be64_to_cpu(dino->di_size));
			break;
		case XR_INO_FIFO:
			do_warn(
_("size of fifo inode %" PRIu64 " != 0 (%" PRId64 " bytes)\n"), lino,
				(int64_t)be64_to_cpu(dino->di_size));
			break;
		case XR_INO_UQUOTA:
		case XR_INO_GQUOTA:
		case XR_INO_PQUOTA:
			do_warn(
_("size of quota inode %" PRIu64 " != 0 (%" PRId64 " bytes)\n"), lino,
				(int64_t)be64_to_cpu(dino->di_size));
			break;
		default:
			do_warn(_("Internal error - process_misc_ino_types, "
				  "illegal type %d\n"), type);
			abort();
		}

		return(1);
	}

	return(0);
}

static int
process_misc_ino_types_blocks(xfs_rfsblock_t totblocks, xfs_ino_t lino, int type)
{
	/*
	 * you can not enforce all misc types have zero data fork blocks
	 * by checking dino->di_nblocks because atotblocks (attribute
	 * blocks) are part of nblocks. We must check this later when atotblocks
	 * has been calculated or by doing a simple check that anExtents == 0.
	 * We must also guarantee that totblocks is 0. Thus nblocks checking
	 * will be done later in process_dinode_int for misc types.
	 */

	if (totblocks != 0)  {
		switch (type)  {
		case XR_INO_CHRDEV:
			do_warn(
_("size of character device inode %" PRIu64 " != 0 (%" PRIu64 " blocks)\n"),
				lino, totblocks);
			break;
		case XR_INO_BLKDEV:
			do_warn(
_("size of block device inode %" PRIu64 " != 0 (%" PRIu64 " blocks)\n"),
				lino, totblocks);
			break;
		case XR_INO_SOCK:
			do_warn(
_("size of socket inode %" PRIu64 " != 0 (%" PRIu64 " blocks)\n"),
				lino, totblocks);
			break;
		case XR_INO_FIFO:
			do_warn(
_("size of fifo inode %" PRIu64 " != 0 (%" PRIu64 " blocks)\n"),
				lino, totblocks);
			break;
		default:
			return(0);
		}
		return(1);
	}
	return (0);
}

static inline int
dinode_fmt(
	struct xfs_dinode *dino)
{
	return be16_to_cpu(dino->di_mode) & S_IFMT;
}

static inline void
change_dinode_fmt(
	struct xfs_dinode	*dino,
	int			new_fmt)
{
	int			mode = be16_to_cpu(dino->di_mode);

	ASSERT((new_fmt & ~S_IFMT) == 0);

	mode &= ~S_IFMT;
	mode |= new_fmt;
	dino->di_mode = cpu_to_be16(mode);
}

static int
check_dinode_mode_format(
	struct xfs_dinode	*dinoc)
{
	if (dinoc->di_format == XFS_DINODE_FMT_UUID)
		return -1;	/* FMT_UUID is not used */

	switch (dinode_fmt(dinoc)) {
	case S_IFIFO:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
		return (dinoc->di_format != XFS_DINODE_FMT_DEV) ? -1 : 0;

	case S_IFDIR:
		return (dinoc->di_format < XFS_DINODE_FMT_LOCAL ||
			dinoc->di_format > XFS_DINODE_FMT_BTREE) ? -1 : 0;

	case S_IFREG:
		switch (dinoc->di_format) {
		case XFS_DINODE_FMT_EXTENTS:
		case XFS_DINODE_FMT_BTREE:
			return 0;
		case XFS_DINODE_FMT_META_BTREE:
			switch (be16_to_cpu(dinoc->di_metatype)) {
			case XFS_METAFILE_RTRMAP:
			case XFS_METAFILE_RTREFCOUNT:
				return 0;
			default:
				return -1;
			}
		}
		return -1;

	case S_IFLNK:
		return (dinoc->di_format < XFS_DINODE_FMT_LOCAL ||
			dinoc->di_format > XFS_DINODE_FMT_EXTENTS) ? -1 : 0;

	default: ;
	}
	return 0;	/* invalid modes are checked elsewhere */
}

static int
process_check_rt_inode(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dinoc,
	xfs_ino_t		lino,
	int			*type,
	int			*dirty,
	int			expected_type,
	const char		*tag)
{
	xfs_extnum_t		dnextents = xfs_dfork_data_extents(dinoc);

	if (*type != expected_type) {
		do_warn(
_("%s inode %" PRIu64 " has bad type 0x%x, "),
			tag, lino, dinode_fmt(dinoc));
		if (!no_modify)  {
			do_warn(_("resetting to regular file\n"));
			change_dinode_fmt(dinoc, S_IFREG);
			*dirty = 1;
		} else  {
			do_warn(_("would reset to regular file\n"));
		}
	}
	if (mp->m_sb.sb_rblocks == 0 && dnextents != 0)  {
		do_warn(
_("bad # of extents (%" PRIu64 ") for %s inode %" PRIu64 "\n"),
			dnextents, tag, lino);
		return 1;
	}
	return 0;
}

/*
 * If inode is a superblock inode, does type check to make sure is it valid.
 * Returns 0 if it's valid, non-zero if it needs to be cleared.
 */

static int
process_check_metadata_inodes(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dinoc,
	xfs_ino_t		lino,
	int			*type,
	int			*dirty)
{
	if (lino == mp->m_sb.sb_rootino) {
		if (*type != XR_INO_DIR)  {
			do_warn(_("root inode %" PRIu64 " has bad type 0x%x\n"),
				lino, dinode_fmt(dinoc));
			*type = XR_INO_DIR;
			if (!no_modify)  {
				do_warn(_("resetting to directory\n"));
				change_dinode_fmt(dinoc, S_IFDIR);
				*dirty = 1;
			} else
				do_warn(_("would reset to directory\n"));
		}
		return 0;
	}
	if (is_quota_inode(XFS_DQTYPE_USER, lino))  {
		if (*type != XR_INO_UQUOTA)  {
			do_warn(_("user quota inode %" PRIu64 " has bad type 0x%x\n"),
				lino, dinode_fmt(dinoc));
			clear_quota_inode(XFS_DQTYPE_USER);
			return 1;
		}
		return 0;
	}
	if (is_quota_inode(XFS_DQTYPE_GROUP, lino))  {
		if (*type != XR_INO_GQUOTA)  {
			do_warn(_("group quota inode %" PRIu64 " has bad type 0x%x\n"),
				lino, dinode_fmt(dinoc));
			clear_quota_inode(XFS_DQTYPE_GROUP);
			return 1;
		}
		return 0;
	}
	if (is_quota_inode(XFS_DQTYPE_PROJ, lino))  {
		if (*type != XR_INO_PQUOTA)  {
			do_warn(_("project quota inode %" PRIu64 " has bad type 0x%x\n"),
				lino, dinode_fmt(dinoc));
			clear_quota_inode(XFS_DQTYPE_PROJ);
			return 1;
		}
		return 0;
	}
	if (lino == mp->m_sb.sb_rsumino || is_rtsummary_inode(lino))
		return process_check_rt_inode(mp, dinoc, lino, type, dirty,
				XR_INO_RTSUM, _("realtime summary"));
	if (lino == mp->m_sb.sb_rbmino || is_rtbitmap_inode(lino))
		return process_check_rt_inode(mp, dinoc, lino, type, dirty,
				XR_INO_RTBITMAP, _("realtime bitmap"));
	if (is_rtrmap_inode(lino))
		return process_check_rt_inode(mp, dinoc, lino, type, dirty,
				XR_INO_RTRMAP, _("realtime rmap btree"));
	if (is_rtrefcount_inode(lino))
		return process_check_rt_inode(mp, dinoc, lino, type, dirty,
				XR_INO_RTREFC, _("realtime refcount btree"));
	return 0;
}

/*
 * general size/consistency checks:
 *
 * if the size <= size of the data fork, directories  must be
 * local inodes unlike regular files which would be extent inodes.
 * all the other mentioned types have to have a zero size value.
 *
 * if the size and format don't match, get out now rather than
 * risk trying to process a non-existent extents or btree
 * type data fork.
 */
static int
process_check_inode_sizes(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		lino,
	int			type)
{
	xfs_fsize_t		size = be64_to_cpu(dino->di_size);

	switch (type)  {

	case XR_INO_DIR:
		if (size <= XFS_DFORK_DSIZE(dino, mp) &&
				dino->di_format != XFS_DINODE_FMT_LOCAL) {
			do_warn(
_("mismatch between format (%d) and size (%" PRId64 ") in directory ino %" PRIu64 "\n"),
				dino->di_format, size, lino);
			return 1;
		}
		if (size > XFS_DIR2_LEAF_OFFSET) {
			do_warn(
_("directory inode %" PRIu64 " has bad size %" PRId64 "\n"),
				lino, size);
			return 1;
		}
		break;

	case XR_INO_SYMLINK:
		if (process_symlink_extlist(mp, lino, dino))  {
			do_warn(_("bad data fork in symlink %" PRIu64 "\n"), lino);
			return 1;
		}
		break;

	case XR_INO_CHRDEV:
	case XR_INO_BLKDEV:
	case XR_INO_SOCK:
	case XR_INO_FIFO:
		if (process_misc_ino_types(mp, dino, lino, type))
			return 1;
		break;

	case XR_INO_UQUOTA:
	case XR_INO_GQUOTA:
	case XR_INO_PQUOTA:
		/* Quota inodes have same restrictions as above types */
		if (process_misc_ino_types(mp, dino, lino, type))
			return 1;
		break;

	case XR_INO_RTDATA:
		/*
		 * if we have no realtime blocks, any inode claiming
		 * to be a real-time file is bogus
		 */
		if (mp->m_sb.sb_rblocks == 0)  {
			do_warn(
_("found inode %" PRIu64 " claiming to be a real-time file\n"), lino);
			return 1;
		}
		break;

	case XR_INO_RTBITMAP:
		if (size != (int64_t)mp->m_sb.sb_rbmblocks *
					mp->m_sb.sb_blocksize) {
			do_warn(
_("realtime bitmap inode %" PRIu64 " has bad size %" PRId64 " (should be %" PRIu64 ")\n"),
				lino, size,
				(int64_t) mp->m_sb.sb_rbmblocks *
					mp->m_sb.sb_blocksize);
			return 1;
		}
		break;

	case XR_INO_RTSUM:
		if (size != XFS_FSB_TO_B(mp, mp->m_rsumblocks))  {
			do_warn(
_("realtime summary inode %" PRIu64 " has bad size %" PRIu64 " (should be %" PRIu64 ")\n"),
				lino, size,
				XFS_FSB_TO_B(mp, mp->m_rsumblocks));
			return 1;
		}
		break;

	case XR_INO_RTRMAP:
		/*
		 * if we have no rmapbt, any inode claiming
		 * to be a real-time file is bogus
		 */
		if (!xfs_has_rmapbt(mp)) {
			do_warn(
_("found inode %" PRIu64 " claiming to be a rtrmapbt file, but rmapbt is disabled\n"), lino);
			return 1;
		}
		break;

	case XR_INO_RTREFC:
		/*
		 * if we have no refcountbt, any inode claiming
		 * to be a real-time file is bogus
		 */
		if (!xfs_has_reflink(mp)) {
			do_warn(
_("found inode %" PRIu64 " claiming to be a rtrefcountbt file, but reflink is disabled\n"), lino);
			return 1;
		}
		break;

	default:
		break;
	}
	return 0;
}

/*
 * check for illegal values of forkoff
 */
static int
process_check_inode_forkoff(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		lino)
{
	if (dino->di_forkoff == 0)
		return 0;

	switch (dino->di_format)  {
	case XFS_DINODE_FMT_DEV:
		if (dino->di_forkoff != (roundup(sizeof(xfs_dev_t), 8) >> 3)) {
			do_warn(
_("bad attr fork offset %d in dev inode %" PRIu64 ", should be %d\n"),
				dino->di_forkoff, lino,
				(int)(roundup(sizeof(xfs_dev_t), 8) >> 3));
			return 1;
		}
		break;
	case XFS_DINODE_FMT_META_BTREE:
		if (!xfs_has_metadir(mp) || !xfs_has_parent(mp)) {
			do_warn(
_("metadata inode %" PRIu64 " type %d cannot have attr fork\n"),
				lino, dino->di_format);
			return 1;
		}
		fallthrough;
	case XFS_DINODE_FMT_LOCAL:
	case XFS_DINODE_FMT_EXTENTS:
	case XFS_DINODE_FMT_BTREE:
		if (dino->di_forkoff >= (XFS_LITINO(mp) >> 3)) {
			do_warn(
_("bad attr fork offset %d in inode %" PRIu64 ", max=%zu\n"),
				dino->di_forkoff, lino, XFS_LITINO(mp) >> 3);
			return 1;
		}
		break;
	default:
		do_error(_("unexpected inode format %d\n"), dino->di_format);
		break;
	}
	return 0;
}

/*
 * Updates the inodes block and extent counts if they are wrong
 */
static int
process_inode_blocks_and_extents(
	struct xfs_dinode	*dino,
	xfs_rfsblock_t		nblocks,
	uint64_t		nextents,
	uint64_t		anextents,
	xfs_ino_t		lino,
	int			*dirty)
{
	xfs_extnum_t		dnextents;
	xfs_extnum_t		danextents;

	if (nblocks != be64_to_cpu(dino->di_nblocks))  {
		if (!no_modify)  {
			do_warn(
_("correcting nblocks for inode %" PRIu64 ", was %llu - counted %" PRIu64 "\n"), lino,
			       (unsigned long long) be64_to_cpu(dino->di_nblocks),
			       nblocks);
			dino->di_nblocks = cpu_to_be64(nblocks);
			*dirty = 1;
		} else  {
			do_warn(
_("bad nblocks %llu for inode %" PRIu64 ", would reset to %" PRIu64 "\n"),
			       (unsigned long long) be64_to_cpu(dino->di_nblocks),
			       lino, nblocks);
		}
	}

	if (nextents > xfs_iext_max_nextents(
				xfs_dinode_has_large_extent_counts(dino),
				XFS_DATA_FORK)) {
		do_warn(
_("too many data fork extents (%" PRIu64 ") in inode %" PRIu64 "\n"),
			nextents, lino);
		return 1;
	}
	dnextents = xfs_dfork_data_extents(dino);
	if (nextents != dnextents)  {
		if (!no_modify)  {
			do_warn(
_("correcting nextents for inode %" PRIu64 ", was %" PRIu64 " - counted %" PRIu64 "\n"),
				lino, dnextents, nextents);
			if (xfs_dinode_has_large_extent_counts(dino))
				dino->di_big_nextents = cpu_to_be64(nextents);
			else
				dino->di_nextents = cpu_to_be32(nextents);
			*dirty = 1;
		} else  {
			do_warn(
_("bad nextents %" PRIu64 " for inode %" PRIu64 ", would reset to %" PRIu64 "\n"),
				dnextents, lino, nextents);
		}
	}

	if (anextents > xfs_iext_max_nextents(
				xfs_dinode_has_large_extent_counts(dino),
				XFS_ATTR_FORK)) {
		do_warn(
_("too many attr fork extents (%" PRIu64 ") in inode %" PRIu64 "\n"),
			anextents, lino);
		return 1;
	}
	danextents = xfs_dfork_attr_extents(dino);
	if (anextents != danextents)  {
		if (!no_modify)  {
			do_warn(
_("correcting anextents for inode %" PRIu64 ", was %" PRIu64 " - counted %" PRIu64 "\n"),
				lino, danextents, anextents);
			if (xfs_dinode_has_large_extent_counts(dino))
				dino->di_big_anextents = cpu_to_be32(anextents);
			else
				dino->di_anextents = cpu_to_be16(anextents);
			*dirty = 1;
		} else  {
			do_warn(
_("bad anextents %" PRIu64 " for inode %" PRIu64 ", would reset to %" PRIu64 "\n"),
				danextents, lino, anextents);
		}
	}

	/*
	 * We are comparing different units here, but that's fine given that
	 * an extent has to have at least a block in it.
	 */
	if (nblocks < nextents + anextents) {
		do_warn(
_("nblocks (%" PRIu64 ") smaller than nextents for inode %" PRIu64 "\n"), nblocks, lino);
		return 1;
	}

	return 0;
}

/*
 * check data fork -- if it's bad, clear the inode
 */
static int
process_inode_data_fork(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	struct xfs_dinode	**dinop,
	int			type,
	int			*dirty,
	xfs_rfsblock_t		*totblocks,
	xfs_extnum_t		*nextents,
	blkmap_t		**dblkmap,
	int			check_dups,
	struct xfs_buf		**ino_bpp,
	bool			zap_metadata)
{
	struct xfs_dinode	*dino = *dinop;
	xfs_ino_t		lino = XFS_AGINO_TO_INO(mp, agno, ino);
	int			err = 0;
	xfs_extnum_t		nex, max_nex;
	int			try_rebuild = -1; /* don't know yet */

retry:
	/*
	 * extent count on disk is only valid for positive values. The kernel
	 * uses negative values in memory. hence if we see negative numbers
	 * here, trash it!
	 */
	nex = xfs_dfork_data_extents(dino);
	max_nex = xfs_iext_max_nextents(
			xfs_dinode_has_large_extent_counts(dino),
			XFS_DATA_FORK);
	if (nex > max_nex)
		*nextents = 1;
	else
		*nextents = nex;

	if (*nextents > be64_to_cpu(dino->di_nblocks))
		*nextents = 1;

	/*
	 * Repair doesn't care about the block maps for regular file data
	 * because it never tries to read data blocks.  Only spend time on
	 * constructing a block map for directories, quota files, symlinks,
	 * and realtime space metadata.
	 */
	if (dino->di_format != XFS_DINODE_FMT_LOCAL &&
	    (type != XR_INO_RTDATA && type != XR_INO_DATA))
		*dblkmap = blkmap_alloc(*nextents, XFS_DATA_FORK);
	*nextents = 0;

	switch (dino->di_format) {
	case XFS_DINODE_FMT_LOCAL:
		err = process_lclinode(mp, agno, ino, dino, XFS_DATA_FORK);
		*totblocks = 0;
		break;
	case XFS_DINODE_FMT_EXTENTS:
		if (!rmapbt_suspect && try_rebuild == -1)
			try_rebuild = 1;
		err = process_exinode(mp, agno, ino, dino, type, dirty,
			totblocks, nextents, dblkmap, XFS_DATA_FORK,
			check_dups, zap_metadata);
		break;
	case XFS_DINODE_FMT_BTREE:
		if (!rmapbt_suspect && try_rebuild == -1)
			try_rebuild = 1;
		err = process_btinode(mp, agno, ino, dino, type, dirty,
			totblocks, nextents, dblkmap, XFS_DATA_FORK,
			check_dups, zap_metadata);
		break;
	case XFS_DINODE_FMT_META_BTREE:
		switch (be16_to_cpu(dino->di_metatype)) {
		case XFS_METAFILE_RTRMAP:
			err = process_rtrmap(mp, agno, ino, dino, type, dirty,
					totblocks, nextents, dblkmap,
					check_dups);
			break;
		case XFS_METAFILE_RTREFCOUNT:
			err = process_rtrefc(mp, agno, ino, dino, type, dirty,
					totblocks, nextents, dblkmap,
					check_dups);
			break;
		default:
			do_error(
 _("unknown meta btree type %d, ino %" PRIu64 " (mode = %d)\n"),
					be16_to_cpu(dino->di_metatype), lino,
					be16_to_cpu(dino->di_mode));
			err = 1;
			break;
		}
		break;
	case XFS_DINODE_FMT_DEV:
		err = 0;
		break;
	default:
		do_error(_("unknown format %d, ino %" PRIu64 " (mode = %d)\n"),
			dino->di_format, lino, be16_to_cpu(dino->di_mode));
	}

	if (err)  {
		do_warn(_("bad data fork in inode %" PRIu64 "\n"), lino);
		if (!no_modify)  {
			if (try_rebuild == 1) {
				do_warn(
_("rebuilding inode %"PRIu64" data fork\n"),
					lino);
				try_rebuild = 0;
				err = rebuild_bmap(mp, lino, XFS_DATA_FORK,
						be32_to_cpu(dino->di_nextents),
						ino_bpp, dinop, dirty);
				dino = *dinop;
				if (!err)
					goto retry;
				do_warn(
_("inode %"PRIu64" data fork rebuild failed, error %d, clearing\n"),
					lino, err);
			}
			clear_dinode(mp, dino, lino);
			*dirty += 1;
			ASSERT(*dirty > 0);
		} else if (try_rebuild == 1) {
			do_warn(
_("would have tried to rebuild inode %"PRIu64" data fork\n"),
					lino);
		}
		return 1;
	}

	if (check_dups)  {
		/*
		 * if check_dups was non-zero, we have to
		 * re-process data fork to set bitmap since the
		 * bitmap wasn't set the first time through
		 */
		switch (dino->di_format) {
		case XFS_DINODE_FMT_LOCAL:
			err = process_lclinode(mp, agno, ino, dino,
						XFS_DATA_FORK);
			break;
		case XFS_DINODE_FMT_EXTENTS:
			err = process_exinode(mp, agno, ino, dino, type,
				dirty, totblocks, nextents, dblkmap,
				XFS_DATA_FORK, 0, zap_metadata);
			break;
		case XFS_DINODE_FMT_BTREE:
			err = process_btinode(mp, agno, ino, dino, type,
				dirty, totblocks, nextents, dblkmap,
				XFS_DATA_FORK, 0, zap_metadata);
			break;
		case XFS_DINODE_FMT_DEV:
			err = 0;
			break;
		case XFS_DINODE_FMT_META_BTREE:
			switch (be16_to_cpu(dino->di_metatype)) {
			case XFS_METAFILE_RTRMAP:
			case XFS_METAFILE_RTREFCOUNT:
				err = 0;
				break;
			default:
				do_error(
 _("unknown meta btree type %d, ino %" PRIu64 " (mode = %d)\n"),
						be16_to_cpu(dino->di_metatype),
						lino,
						be16_to_cpu(dino->di_mode));
				err = 1;
				break;
			}
			break;
		default:
			do_error(_("unknown format %d, ino %" PRIu64 " (mode = %d)\n"),
				dino->di_format, lino,
				be16_to_cpu(dino->di_mode));
		}

		if (no_modify && err != 0)
			return 1;

		ASSERT(err == 0);
	}
	return 0;
}

/*
 * Process extended attribute fork in inode
 */
static int
process_inode_attr_fork(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	struct xfs_dinode	**dinop,
	int			type,
	int			*dirty,
	xfs_rfsblock_t		*atotblocks,
	xfs_extnum_t		*anextents,
	int			check_dups,
	int			extra_attr_check,
	int			*retval,
	struct xfs_buf		**ino_bpp,
	bool			zap_metadata)
{
	xfs_ino_t		lino = XFS_AGINO_TO_INO(mp, agno, ino);
	struct xfs_dinode	*dino = *dinop;
	struct blkmap		*ablkmap = NULL;
	xfs_extnum_t		max_nex;
	int			repair = 0;
	int			err;
	int			try_rebuild = -1; /* don't know yet */

retry:
	if (!dino->di_forkoff) {
		*anextents = 0;
		if (dino->di_aformat != XFS_DINODE_FMT_EXTENTS) {
			do_warn(_("bad attribute format %d in inode %" PRIu64 ", "),
				dino->di_aformat, lino);
			if (!no_modify) {
				do_warn(_("resetting value\n"));
				dino->di_aformat = XFS_DINODE_FMT_EXTENTS;
				*dirty = 1;
			} else
				do_warn(_("would reset value\n"));
		}
		return 0;
	}

	*anextents = xfs_dfork_attr_extents(dino);
	max_nex = xfs_iext_max_nextents(
			xfs_dinode_has_large_extent_counts(dino),
			XFS_ATTR_FORK);
	if (*anextents > max_nex)
		*anextents = 1;
	if (*anextents > be64_to_cpu(dino->di_nblocks))
		*anextents = 1;

	switch (dino->di_aformat) {
	case XFS_DINODE_FMT_LOCAL:
		*anextents = 0;
		*atotblocks = 0;
		err = process_lclinode(mp, agno, ino, dino, XFS_ATTR_FORK);
		break;
	case XFS_DINODE_FMT_EXTENTS:
		if (!rmapbt_suspect && try_rebuild == -1)
			try_rebuild = 1;
		ablkmap = blkmap_alloc(*anextents, XFS_ATTR_FORK);
		*anextents = 0;
		err = process_exinode(mp, agno, ino, dino, type, dirty,
				atotblocks, anextents, &ablkmap,
				XFS_ATTR_FORK, check_dups, zap_metadata);
		break;
	case XFS_DINODE_FMT_BTREE:
		if (!rmapbt_suspect && try_rebuild == -1)
			try_rebuild = 1;
		ablkmap = blkmap_alloc(*anextents, XFS_ATTR_FORK);
		*anextents = 0;
		err = process_btinode(mp, agno, ino, dino, type, dirty,
				atotblocks, anextents, &ablkmap,
				XFS_ATTR_FORK, check_dups, zap_metadata);
		break;
	default:
		do_warn(_("illegal attribute format %d, ino %" PRIu64 "\n"),
				dino->di_aformat, lino);
		err = 1;
		break;
	}

	if (err) {
		/*
		 * clear the attribute fork if necessary.  we can't
		 * clear the inode because we've already put the
		 * inode space info into the blockmap.
		 *
		 * XXX - put the inode onto the "move it" list and
		 *	log the the attribute scrubbing
		 */
		do_warn(_("bad attribute fork in inode %" PRIu64 "\n"), lino);

		if (!no_modify)  {
			if (try_rebuild == 1) {
				do_warn(
_("rebuilding inode %"PRIu64" attr fork\n"),
					lino);
				try_rebuild = 0;
				err = rebuild_bmap(mp, lino, XFS_ATTR_FORK,
						be16_to_cpu(dino->di_anextents),
						ino_bpp, dinop, dirty);
				dino = *dinop;
				if (!err)
					goto retry;
				do_warn(
_("inode %"PRIu64" attr fork rebuild failed, error %d"),
					lino, err);
			}
			do_warn(_(", clearing attr fork\n"));
			*dirty += clear_dinode_attr(mp, dino, lino);
			ASSERT(*dirty > 0);
		} else if (try_rebuild) {
			do_warn(
_("would have tried to rebuild inode %"PRIu64" attr fork or cleared it\n"),
					lino);
		} else {
			do_warn(_(", would clear attr fork\n"));
		}

		*atotblocks = 0;
		*anextents = 0;
		blkmap_free(ablkmap);
		*retval = 1;

		return 0;
	}

	if (check_dups)  {
		switch (dino->di_aformat) {
		case XFS_DINODE_FMT_LOCAL:
			err = process_lclinode(mp, agno, ino, dino,
						XFS_ATTR_FORK);
			break;
		case XFS_DINODE_FMT_EXTENTS:
			err = process_exinode(mp, agno, ino, dino,
				type, dirty, atotblocks, anextents,
				&ablkmap, XFS_ATTR_FORK, 0, zap_metadata);
			break;
		case XFS_DINODE_FMT_BTREE:
			err = process_btinode(mp, agno, ino, dino,
				type, dirty, atotblocks, anextents,
				&ablkmap, XFS_ATTR_FORK, 0, zap_metadata);
			break;
		default:
			do_error(_("illegal attribute fmt %d, ino %" PRIu64 "\n"),
				dino->di_aformat, lino);
		}

		if (no_modify && err != 0) {
			blkmap_free(ablkmap);
			return 1;
		}

		ASSERT(err == 0);
	}

	/*
	 * do attribute semantic-based consistency checks now
	 */

	/* get this only in phase 3, not in both phase 3 and 4 */
	if (extra_attr_check &&
			process_attributes(mp, lino, dino, ablkmap, &repair)) {
		do_warn(
	_("problem with attribute contents in inode %" PRIu64 "\n"),
			lino);
		if (!repair) {
			/* clear attributes if not done already */
			if (!no_modify)  {
				*dirty += clear_dinode_attr(mp, dino, lino);
			} else  {
				do_warn(_("would clear attr fork\n"));
			}
			*atotblocks = 0;
			*anextents = 0;
		}
		else {
			*dirty = 1; /* it's been repaired */
		}
	}
	blkmap_free(ablkmap);
	return 0;
}

/*
 * check nlinks feature, if it's a version 1 inode,
 * just leave nlinks alone.  even if it's set wrong,
 * it'll be reset when read in.
 */

static int
process_check_inode_nlink_version(
	struct xfs_dinode	*dino,
	xfs_ino_t		lino)
{
	int			dirty = 0;

	/*
	 * if it's a version 2 non-metadir inode, it should have a zero
	 * onlink field, so clear it.
	 */
	if (dino->di_version > 1 &&
	    !(dino->di_flags2 & cpu_to_be64(XFS_DIFLAG2_METADATA)) &&
	    dino->di_metatype != 0) {
		if (!no_modify) {
			do_warn(
_("clearing obsolete nlink field in version 2 inode %" PRIu64 ", was %d, now 0\n"),
				lino, be16_to_cpu(dino->di_metatype));
			dino->di_metatype = 0;
			dirty = 1;
		} else  {
			do_warn(
_("would clear obsolete nlink field in version 2 inode %" PRIu64 ", currently %d\n"),
				lino, be16_to_cpu(dino->di_metatype));
		}
	}
	return dirty;
}

/* Check nanoseconds of a timestamp don't exceed 1 second. */
static void
check_nsec(
	const char		*name,
	xfs_ino_t		lino,
	struct xfs_dinode	*dip,
	xfs_timestamp_t		*ts,
	int			*dirty)
{
	struct xfs_legacy_timestamp *t;

	if (xfs_dinode_has_bigtime(dip))
		return;

	t = (struct xfs_legacy_timestamp *)ts;
	if (be32_to_cpu(t->t_nsec) < NSEC_PER_SEC)
		return;

	do_warn(
_("Bad %s nsec %u on inode %" PRIu64 ", "), name, be32_to_cpu(t->t_nsec), lino);
	if (no_modify) {
		do_warn(_("would reset to zero\n"));
	} else {
		do_warn(_("resetting to zero\n"));
		t->t_nsec = 0;
		*dirty = 1;
	}
}

static void
validate_extsize(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		lino,
	int			*dirty)
{
	uint16_t		flags = be16_to_cpu(dino->di_flags);
	unsigned int		value = be32_to_cpu(dino->di_extsize);
	bool			misaligned = false;
	bool			bad;

	/*
	 * XFS allows a sysadmin to change the rt extent size when adding a rt
	 * section to a filesystem after formatting.  If there are any
	 * directories with extszinherit and rtinherit set, the hint could
	 * become misaligned with the new rextsize.  The verifier doesn't check
	 * this, because we allow rtinherit directories even without an rt
	 * device.
	 */
	if ((flags & XFS_DIFLAG_EXTSZINHERIT) &&
	    (flags & XFS_DIFLAG_RTINHERIT) &&
	    xfs_extlen_to_rtxmod(mp, value) > 0)
		misaligned = true;

	/*
	 * Complain if the verifier fails.
	 *
	 * Old kernels didn't check the alignment of extsize hints when copying
	 * them to new regular realtime files.  The inode verifier now checks
	 * the alignment (because misaligned hints cause misbehavior in the rt
	 * allocator), so we have to complain and fix them.
	 */
	bad = libxfs_inode_validate_extsize(mp, value,
			be16_to_cpu(dino->di_mode), flags) != NULL;
	if (bad || misaligned) {
		do_warn(
_("Bad extent size hint %u on inode %" PRIu64 ", "),
				value, lino);
		if (!no_modify)  {
			do_warn(_("resetting to zero\n"));
			dino->di_extsize = 0;
			dino->di_flags &= ~cpu_to_be16(XFS_DIFLAG_EXTSIZE |
						       XFS_DIFLAG_EXTSZINHERIT);
			*dirty = 1;
		} else
			do_warn(_("would reset to zero\n"));
	}
}

static inline bool
should_have_metadir_iflag(
	struct xfs_mount	*mp,
	xfs_ino_t		ino)
{
	if (ino == mp->m_sb.sb_metadirino)
		return true;
	if (ino == mp->m_sb.sb_rbmino)
		return true;
	if (ino == mp->m_sb.sb_rsumino)
		return true;
	if (ino == mp->m_sb.sb_uquotino)
		return true;
	if (ino == mp->m_sb.sb_gquotino)
		return true;
	if (ino == mp->m_sb.sb_pquotino)
		return true;
	return false;
}

static void
validate_cowextsize(
	struct xfs_mount	*mp,
	struct xfs_dinode	*dino,
	xfs_ino_t		lino,
	int			*dirty)
{
	uint16_t		flags = be16_to_cpu(dino->di_flags);
	uint64_t		flags2 = be64_to_cpu(dino->di_flags2);
	unsigned int		value = be32_to_cpu(dino->di_cowextsize);
	bool			misaligned = false;
	bool			bad;

	/*
	 * XFS allows a sysadmin to change the rt extent size when adding a
	 * rt section to a filesystem after formatting.  If there are any
	 * directories with both a cowextsize hint and rtinherit set, the
	 * hint could become misaligned with the new rextsize.
	 */
	if ((flags2 & XFS_DIFLAG2_COWEXTSIZE) &&
	    (flags & XFS_DIFLAG_RTINHERIT) &&
	    value % mp->m_sb.sb_rextsize > 0)
		misaligned = true;

	/* Complain if the verifier fails. */
	bad = libxfs_inode_validate_cowextsize(mp, value,
			be16_to_cpu(dino->di_mode), flags, flags2) != NULL;
	if (bad || misaligned) {
		do_warn(
_("Bad CoW extent size hint %u on inode %" PRIu64 ", "),
				be32_to_cpu(dino->di_cowextsize), lino);
		if (!no_modify) {
			do_warn(_("resetting to zero\n"));
			dino->di_flags2 &= ~cpu_to_be64(XFS_DIFLAG2_COWEXTSIZE);
			dino->di_cowextsize = 0;
			*dirty = 1;
		} else
			do_warn(_("would reset to zero\n"));
	}
}

/*
 * returns 0 if the inode is ok, 1 if the inode is corrupt
 * check_dups can be set to 1 *only* when called by the
 * first pass of the duplicate block checking of phase 4.
 * *dirty is set > 0 if the dinode has been altered and
 * needs to be written out.
 *
 * for detailed, info, look at process_dinode() comments.
 */
static int
process_dinode_int(
	struct xfs_mount	*mp,
	struct xfs_dinode	**dinop,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	int			was_free,	/* 1 if inode is currently free */
	int			*dirty,		/* out == > 0 if inode is now dirty */
	int			*used,		/* out == 1 if inode is in use */
	int			verify_mode,	/* 1 == verify but don't modify inode */
	int			uncertain,	/* 1 == inode is uncertain */
	int			ino_discovery,	/* 1 == check dirs for unknown inodes */
	int			check_dups,	/* 1 == check if inode claims duplicate blocks */
	int			extra_attr_check, /* 1 == do attribute format and value checks */
	int			*isa_dir,	/* out == 1 if inode is a directory */
	xfs_ino_t		*parent,	/* out -- parent if ino is a dir */
	struct xfs_buf		**ino_bpp)
{
	xfs_rfsblock_t		totblocks = 0;
	xfs_rfsblock_t		atotblocks = 0;
	int			di_mode;
	int			type;
	int			retval = 0;
	xfs_extnum_t		nextents;
	xfs_extnum_t		anextents;
	xfs_ino_t		lino;
	const int		is_free = 0;
	const int		is_used = 1;
	blkmap_t		*dblkmap = NULL;
	struct xfs_dinode	*dino = *dinop;
	xfs_agino_t		unlinked_ino;
	struct xfs_perag	*pag;
	bool			is_meta = false;
	bool			zap_metadata = false;

	*dirty = *isa_dir = 0;
	*used = is_used;
	type = XR_INO_UNKNOWN;

	lino = XFS_AGINO_TO_INO(mp, agno, ino);
	di_mode = be16_to_cpu(dino->di_mode);

	/*
	 * if in verify mode, don't modify the inode.
	 *
	 * if correcting, reset stuff that has known values
	 *
	 * if in uncertain mode, be silent on errors since we're
	 * trying to find out if these are inodes as opposed
	 * to assuming that they are.  Just return the appropriate
	 * return code in that case.
	 *
	 * If uncertain is set, verify_mode MUST be set.
	 */
	ASSERT(uncertain == 0 || verify_mode != 0);
	ASSERT(ino_bpp != NULL || verify_mode != 0);

	/*
	 * This is the only valid point to check the CRC; after this we may have
	 * made changes which invalidate it, and the CRC is only updated again
	 * when it gets written out.
	 *
	 * Of course if we make any modifications after this, the inode gets
	 * rewritten, and the CRC is updated automagically.
	 */
	if (xfs_has_crc(mp) &&
	    !libxfs_verify_cksum((char *)dino, mp->m_sb.sb_inodesize,
				XFS_DINODE_CRC_OFF)) {
		retval = 1;
		if (!uncertain)
			do_warn(_("bad CRC for inode %" PRIu64 "%c"),
				lino, verify_mode ? '\n' : ',');
		if (!verify_mode) {
			if (!no_modify) {
				do_warn(_(" will rewrite\n"));
				*dirty = 1;
			} else
				do_warn(_(" would rewrite\n"));
		}
	}

	if (be16_to_cpu(dino->di_magic) != XFS_DINODE_MAGIC)  {
		retval = 1;
		if (!uncertain)
			do_warn(_("bad magic number 0x%x on inode %" PRIu64 "%c"),
				be16_to_cpu(dino->di_magic), lino,
				verify_mode ? '\n' : ',');
		if (!verify_mode) {
			if (!no_modify)  {
				do_warn(_(" resetting magic number\n"));
				dino->di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
				*dirty = 1;
			} else
				do_warn(_(" would reset magic number\n"));
		}
	}

	if (!libxfs_dinode_good_version(mp, dino->di_version)) {
		retval = 1;
		if (!uncertain)
			do_warn(_("bad version number 0x%x on inode %" PRIu64 "%c"),
				(__s8)dino->di_version, lino,
				verify_mode ? '\n' : ',');
		if (!verify_mode) {
			if (!no_modify) {
				do_warn(_(" resetting version number\n"));
				dino->di_version =
					xfs_has_crc(mp) ? 3 : 2;
				*dirty = 1;
			} else
				do_warn(_(" would reset version number\n"));
		}
	}

	unlinked_ino = be32_to_cpu(dino->di_next_unlinked);
	pag = libxfs_perag_get(mp, agno);
	if (!xfs_verify_agino_or_null(pag, unlinked_ino)) {
		retval = 1;
		if (!uncertain)
			do_warn(_("bad next_unlinked 0x%x on inode %" PRIu64 "%c"),
				be32_to_cpu(dino->di_next_unlinked), lino,
				verify_mode ? '\n' : ',');
		if (!verify_mode) {
			if (!no_modify) {
				do_warn(_(" resetting next_unlinked\n"));
				clear_dinode_unlinked(mp, dino);
				*dirty = 1;
			} else
				do_warn(_(" would reset next_unlinked\n"));
		}
	}
	libxfs_perag_put(pag);

	/*
	 * We don't bother checking the CRC here - we cannot guarantee that when
	 * we are called here that the inode has not already been modified in
	 * memory and hence invalidated the CRC.
	 */
	if (xfs_has_crc(mp)) {
		if (be64_to_cpu(dino->di_ino) != lino) {
			if (!uncertain)
				do_warn(
_("inode identifier %llu mismatch on inode %" PRIu64 "\n"),
					(unsigned long long)be64_to_cpu(dino->di_ino),
					lino);
			if (verify_mode)
				return 1;
			goto clear_bad_out;
		}
		if (platform_uuid_compare(&dino->di_uuid,
					  &mp->m_sb.sb_meta_uuid)) {
			if (!uncertain)
				do_warn(
			_("UUID mismatch on inode %" PRIu64 "\n"), lino);
			if (verify_mode)
				return 1;
			goto clear_bad_out;
		}
	}

	/*
	 * blow out of here if the inode size is < 0
	 */
	if ((xfs_fsize_t)be64_to_cpu(dino->di_size) < 0)  {
		if (!uncertain)
			do_warn(
_("bad (negative) size %" PRId64 " on inode %" PRIu64 "\n"),
				(int64_t)be64_to_cpu(dino->di_size),
				lino);
		if (verify_mode)
			return 1;
		goto clear_bad_out;
	}

	/*
	 * if not in verify mode, check to see if the inode and imap
	 * agree that the inode is free
	 */
	if (!verify_mode && di_mode == 0) {
		/*
		 * was_free value is not meaningful if we're in verify mode
		 */
		if (was_free) {
			/*
			 * easy case, inode free -- inode and map agree, check
			 * it just in case to ensure that format, etc. are
			 * set correctly
			 */
			if (libxfs_dinode_verify(mp, lino, dino) != NULL) {
				do_warn(
 _("free inode %" PRIu64 " contains errors, "), lino);
				if (!no_modify) {
					clear_dinode(mp, dino, lino);
					do_warn(_("corrected\n"));
					*dirty += 1;
				} else {
					do_warn(_("would correct\n"));
				}
			}
			*used = is_free;
			return 0;
		}
		/*
		 * the inode looks free but the map says it's in use.
		 * clear the inode just to be safe and mark the inode
		 * free.
		 */
		do_warn(
	_("imap claims a free inode %" PRIu64 " is in use, "), lino);
		if (!no_modify)  {
			do_warn(_("correcting imap and clearing inode\n"));
			clear_dinode(mp, dino, lino);
			*dirty += 1;
			retval = 1;
		} else
			do_warn(_("would correct imap and clear inode\n"));
		*used = is_free;
		return retval;
	}

	/*
	 * because of the lack of any write ordering guarantee, it's
	 * possible that the core got updated but the forks didn't.
	 * so rather than be ambitious (and probably incorrect),
	 * if there's an inconsistency, we get conservative and
	 * just pitch the file.  blow off checking formats of
	 * free inodes since technically any format is legal
	 * as we reset the inode when we re-use it.
	 */
	if (di_mode != 0 && check_dinode_mode_format(dino) != 0) {
		if (!uncertain)
			do_warn(
	_("bad inode format in inode %" PRIu64 "\n"), lino);
		if (verify_mode)
			return 1;
		goto clear_bad_out;
	}

	/*
	 * check that we only have valid flags set, and those that are set make
	 * sense.
	 */
	if (dino->di_flags) {
		uint16_t flags = be16_to_cpu(dino->di_flags);

		if (flags & ~XFS_DIFLAG_ANY) {
			if (!uncertain) {
				do_warn(
	_("Bad flags set in inode %" PRIu64 "\n"),
					lino);
			}
			flags &= XFS_DIFLAG_ANY;
		}

		/* need an rt-dev for the realtime flag */
		if ((flags & XFS_DIFLAG_REALTIME) && !mp->m_sb.sb_rextents) {
			if (!uncertain) {
				do_warn(
	_("inode %" PRIu64 " has RT flag set but there is no RT device\n"),
					lino);
			}
			flags &= ~XFS_DIFLAG_REALTIME;
		}
		if (flags & XFS_DIFLAG_NEWRTBM) {
			/* must be a rt bitmap inode */
			if (lino != mp->m_sb.sb_rbmino) {
				if (!uncertain) {
					do_warn(
	_("inode %" PRIu64 " not rt bitmap\n"),
						lino);
				}
				flags &= ~XFS_DIFLAG_NEWRTBM;
			}
		}
		if (flags & (XFS_DIFLAG_RTINHERIT |
			     XFS_DIFLAG_EXTSZINHERIT |
			     XFS_DIFLAG_PROJINHERIT |
			     XFS_DIFLAG_NOSYMLINKS)) {
			/* must be a directory */
			if (di_mode && !S_ISDIR(di_mode)) {
				if (!uncertain) {
					do_warn(
	_("directory flags set on non-directory inode %" PRIu64 "\n" ),
						lino);
				}
				flags &= ~(XFS_DIFLAG_RTINHERIT |
						XFS_DIFLAG_EXTSZINHERIT |
						XFS_DIFLAG_PROJINHERIT |
						XFS_DIFLAG_NOSYMLINKS);
			}
		}
		if (flags & (XFS_DIFLAG_REALTIME | FS_XFLAG_EXTSIZE)) {
			/* must be a file */
			if (di_mode && !S_ISREG(di_mode)) {
				if (!uncertain) {
					do_warn(
	_("file flags set on non-file inode %" PRIu64 "\n"),
						lino);
				}
				flags &= ~(XFS_DIFLAG_REALTIME |
						FS_XFLAG_EXTSIZE);
			}
		}
		if (!verify_mode && flags != be16_to_cpu(dino->di_flags)) {
			if (!no_modify) {
				do_warn(_("fixing bad flags.\n"));
				dino->di_flags = cpu_to_be16(flags);
				*dirty = 1;
			} else
				do_warn(_("would fix bad flags.\n"));
		}
	}

	/*
	 * check that we only have valid flags2 set, and those that are set make
	 * sense.
	 */
	if (dino->di_version >= 3) {
		uint16_t flags = be16_to_cpu(dino->di_flags);
		uint64_t flags2 = be64_to_cpu(dino->di_flags2);

		if (flags2 & ~XFS_DIFLAG2_ANY) {
			if (!uncertain) {
				do_warn(
	_("Bad flags2 set in inode %" PRIu64 "\n"),
					lino);
			}
			flags2 &= XFS_DIFLAG2_ANY;
		}

		if (flags2 & XFS_DIFLAG2_DAX) {
			/* must be a file or dir */
			if (di_mode && !(S_ISREG(di_mode) || S_ISDIR(di_mode))) {
				if (!uncertain) {
					do_warn(
	_("DAX flag set on special inode %" PRIu64 "\n"),
						lino);
				}
				flags2 &= ~XFS_DIFLAG2_DAX;
			}
		}

		if (flags2 & XFS_DIFLAG2_METADATA) {
			xfs_failaddr_t	fa;

			fa = libxfs_dinode_verify_metadir(mp, dino, di_mode,
					be16_to_cpu(dino->di_flags), flags2);
			if (fa) {
				if (!uncertain)
					do_warn(
	_("inode %" PRIu64 " is incorrectly marked as metadata\n"),
						lino);
				goto clear_bad_out;
			}
		} else if (xfs_has_metadir(mp) &&
			   should_have_metadir_iflag(mp, lino)) {
			if (!uncertain)
				do_warn(
	_("inode %" PRIu64 " should be marked as metadata\n"),
					lino);
			goto clear_bad_out;
		}

		if ((flags2 & XFS_DIFLAG2_REFLINK) &&
		    !xfs_has_reflink(mp)) {
			if (!uncertain) {
				do_warn(
	_("inode %" PRIu64 " is marked reflinked but file system does not support reflink\n"),
					lino);
			}
			goto clear_bad_out;
		}

		if (flags2 & XFS_DIFLAG2_REFLINK) {
			/* must be a file */
			if (di_mode && !S_ISREG(di_mode)) {
				if (!uncertain) {
					do_warn(
	_("reflink flag set on non-file inode %" PRIu64 "\n"),
						lino);
				}
				goto clear_bad_out;
			}
		}

		if ((flags2 & XFS_DIFLAG2_REFLINK) &&
		    !xfs_has_rtreflink(mp) &&
		    (flags & XFS_DIFLAG_REALTIME)) {
			if (!uncertain) {
				do_warn(
	_("Cannot have a reflinked realtime inode %" PRIu64 "\n"),
					lino);
			}
			goto clear_bad_out;
		}

		if ((flags2 & XFS_DIFLAG2_COWEXTSIZE) &&
		    !xfs_has_reflink(mp)) {
			if (!uncertain) {
				do_warn(
	_("inode %" PRIu64 " has CoW extent size hint but file system does not support reflink\n"),
					lino);
			}
			flags2 &= ~XFS_DIFLAG2_COWEXTSIZE;
		}

		if (flags2 & XFS_DIFLAG2_COWEXTSIZE) {
			/* must be a directory or file */
			if (di_mode && !S_ISDIR(di_mode) && !S_ISREG(di_mode)) {
				if (!uncertain) {
					do_warn(
	_("CoW extent size flag set on non-file, non-directory inode %" PRIu64 "\n" ),
						lino);
				}
				flags2 &= ~XFS_DIFLAG2_COWEXTSIZE;
			}
		}

		if ((flags2 & XFS_DIFLAG2_COWEXTSIZE) &&
		    !xfs_has_rtreflink(mp) &&
		    (flags & XFS_DIFLAG_REALTIME)) {
			if (!uncertain) {
				do_warn(
	_("Cannot have CoW extent size hint on a realtime inode %" PRIu64 "\n"),
					lino);
			}
			flags2 &= ~XFS_DIFLAG2_COWEXTSIZE;
		}

		if (xfs_dinode_has_bigtime(dino) &&
		    !xfs_has_bigtime(mp)) {
			if (!uncertain) {
				do_warn(
	_("inode %" PRIu64 " is marked bigtime but file system does not support large timestamps\n"),
					lino);
			}
			flags2 &= ~XFS_DIFLAG2_BIGTIME;

			if (no_modify) {
				do_warn(_("would zero timestamps.\n"));
			} else {
				do_warn(_("zeroing timestamps.\n"));
				dino->di_atime = 0;
				dino->di_mtime = 0;
				dino->di_ctime = 0;
				dino->di_crtime = 0;
				*dirty = 1;
			}
		}

		if (xfs_dinode_has_large_extent_counts(dino) &&
		    !xfs_has_large_extent_counts(mp)) {
			if (!uncertain) {
				do_warn(
	_("inode %" PRIu64 " is marked large extent counts but file system does not support large extent counts\n"),
					lino);
			}
			flags2 &= ~XFS_DIFLAG2_NREXT64;

			if (!no_modify)
				*dirty = 1;
		}

		if (xfs_dinode_has_large_extent_counts(dino)) {
			if (dino->di_nrext64_pad) {
				if (!no_modify) {
					do_warn(_("fixing bad nrext64_pad.\n"));
					dino->di_nrext64_pad = 0;
					*dirty = 1;
				} else
					do_warn(_("would fix bad nrext64_pad.\n"));
			}
		} else if (dino->di_version >= 3) {
			if (dino->di_v3_pad) {
				if (!no_modify) {
					do_warn(_("fixing bad v3_pad.\n"));
					dino->di_v3_pad = 0;
					*dirty = 1;
				} else
					do_warn(_("would fix bad v3_pad.\n"));
			}
		}

		if (!verify_mode && flags2 != be64_to_cpu(dino->di_flags2)) {
			if (!no_modify) {
				do_warn(_("fixing bad flags2.\n"));
				dino->di_flags2 = cpu_to_be64(flags2);
				*dirty = 1;
			} else
				do_warn(_("would fix bad flags2.\n"));
		}
	}

	if (verify_mode)
		return retval;

	/*
	 * clear the next unlinked field if necessary on a good
	 * inode only during phase 4 -- when checking for inodes
	 * referencing duplicate blocks.  then it's safe because
	 * we've done the inode discovery and have found all the inodes
	 * we're going to find.  check_dups is set to 1 only during
	 * phase 4.  Ugly.
	 */
	if (check_dups && be32_to_cpu(dino->di_next_unlinked) != NULLAGINO) {
		if (no_modify) {
			do_warn(
	_("Would clear next_unlinked in inode %" PRIu64 "\n"), lino);
		} else  {
			clear_dinode_unlinked(mp, dino);
			do_warn(
	_("Cleared next_unlinked in inode %" PRIu64 "\n"), lino);
			*dirty += 1;
		}
	}

	/* set type and map type info */

	switch (di_mode & S_IFMT) {
	case S_IFDIR:
		type = XR_INO_DIR;
		*isa_dir = 1;
		break;
	case S_IFREG:
		if (be16_to_cpu(dino->di_flags) & XFS_DIFLAG_REALTIME)
			type = XR_INO_RTDATA;
		else if (lino == mp->m_sb.sb_rbmino ||
			 is_rtbitmap_inode(lino))
			type = XR_INO_RTBITMAP;
		else if (lino == mp->m_sb.sb_rsumino ||
			 is_rtsummary_inode(lino))
			type = XR_INO_RTSUM;
		else if (is_quota_inode(XFS_DQTYPE_USER, lino))
			type = XR_INO_UQUOTA;
		else if (is_quota_inode(XFS_DQTYPE_GROUP, lino))
			type = XR_INO_GQUOTA;
		else if (is_quota_inode(XFS_DQTYPE_PROJ, lino))
			type = XR_INO_PQUOTA;
		else if (is_rtrmap_inode(lino))
			type = XR_INO_RTRMAP;
		else if (is_rtrefcount_inode(lino))
			type = XR_INO_RTREFC;
		else
			type = XR_INO_DATA;
		break;
	case S_IFLNK:
		type = XR_INO_SYMLINK;
		break;
	case S_IFCHR:
		type = XR_INO_CHRDEV;
		break;
	case S_IFBLK:
		type = XR_INO_BLKDEV;
		break;
	case S_IFSOCK:
		type = XR_INO_SOCK;
		break;
	case S_IFIFO:
		type = XR_INO_FIFO;
		break;
	default:
		do_warn(_("bad inode type %#o inode %" PRIu64 "\n"),
				di_mode & S_IFMT, lino);
		goto clear_bad_out;
	}

	/*
	 * type checks for metadata inodes
	 */
	if (process_check_metadata_inodes(mp, dino, lino, &type, dirty) != 0)
		goto clear_bad_out;

	validate_extsize(mp, dino, lino, dirty);

	if (dino->di_version >= 3 &&
	    (!xfs_has_zoned(mp) ||
	     dino->di_metatype != cpu_to_be16(XFS_METAFILE_RTRMAP)))
		validate_cowextsize(mp, dino, lino, dirty);

	/* nsec fields cannot be larger than 1 billion */
	check_nsec("atime", lino, dino, &dino->di_atime, dirty);
	check_nsec("mtime", lino, dino, &dino->di_mtime, dirty);
	check_nsec("ctime", lino, dino, &dino->di_ctime, dirty);
	if (dino->di_version >= 3)
		check_nsec("crtime", lino, dino, &dino->di_crtime, dirty);

	/*
	 * general size/consistency checks:
	 */
	if (process_check_inode_sizes(mp, dino, lino, type) != 0)
		goto clear_bad_out;

	/*
	 * check for illegal values of forkoff
	 */
	if (process_check_inode_forkoff(mp, dino, lino) != 0)
		goto clear_bad_out;

	/*
	 * record the state of the reflink flag
	 */
	if (collect_rmaps)
		record_inode_reflink_flag(mp, dino, agno, ino, lino);

	/* Does this inode think it was metadata? */
	if (dino->di_version >= 3 &&
	    (dino->di_flags2 & cpu_to_be64(XFS_DIFLAG2_METADATA))) {
		struct ino_tree_node	*irec;
		int			off;

		irec = find_inode_rec(mp, agno, ino);
		off = get_inode_offset(mp, lino, irec);
		set_inode_is_meta(irec, off);
		is_meta = true;

		/*
		 * We always rebuild the metadata directory tree during phase
		 * 6, so we use this flag to get all the directory blocks
		 * marked as free, and any other metadata files whose contents
		 * we don't want to save.
		 *
		 * Currently, there are no metadata files that use xattrs, so
		 * we always drop the xattr blocks of metadata files.  Parent
		 * pointers will be rebuilt during phase 6.
		 */
		switch (type) {
		case XR_INO_RTBITMAP:
		case XR_INO_RTSUM:
			/*
			 * rt bitmap and summary files are always recreated
			 * when rtgroups are enabled.  For older filesystems,
			 * they exist at fixed locations and cannot be zapped.
			 */
			if (xfs_has_rtgroups(mp))
				zap_metadata = true;
			break;
		case XR_INO_UQUOTA:
		case XR_INO_GQUOTA:
		case XR_INO_PQUOTA:
			/*
			 * Quota checking and repair doesn't happen until
			 * phase7, so preserve quota inodes and their contents
			 * for later.
			 */
			break;
		default:
			zap_metadata = true;
			break;
		}
	}

	/*
	 * check data fork -- if it's bad, clear the inode
	 */
	if (process_inode_data_fork(mp, agno, ino, dinop, type, dirty,
			&totblocks, &nextents, &dblkmap, check_dups,
			ino_bpp, zap_metadata) != 0)
		goto bad_out;
	dino = *dinop;

	/*
	 * check attribute fork if necessary.  attributes are
	 * always stored in the regular filesystem.
	 */
	if (process_inode_attr_fork(mp, agno, ino, dinop, type, dirty,
			&atotblocks, &anextents, check_dups, extra_attr_check,
			&retval, ino_bpp, is_meta))
		goto bad_out;
	dino = *dinop;

	/*
	 * enforce totblocks is 0 for misc types
	 */
	if (process_misc_ino_types_blocks(totblocks, lino, type))
		goto clear_bad_out;

	/*
	 * correct space counters if required
	 */
	if (process_inode_blocks_and_extents(dino, totblocks + atotblocks,
			nextents, anextents, lino, dirty) != 0)
		goto clear_bad_out;

	/*
	 * do any semantic type-based checking here
	 */
	switch (type)  {
	case XR_INO_DIR:
		if (process_dir2(mp, lino, dino, ino_discovery,
						dirty, "", parent, dblkmap)) {
			do_warn(
	_("problem with directory contents in inode %" PRIu64 "\n"),
				lino);
			goto clear_bad_out;
		}
		break;
	case XR_INO_SYMLINK:
		if (process_symlink(mp, lino, dino, dblkmap) != 0) {
			do_warn(
	_("problem with symbolic link in inode %" PRIu64 "\n"),
				lino);
			goto clear_bad_out;
		}
		break;
	case XR_INO_UQUOTA:
	case XR_INO_GQUOTA:
	case XR_INO_PQUOTA:
		if (process_quota_inode(mp, lino, dino, type, dblkmap) != 0) {
			do_warn(
	_("problem with quota inode %" PRIu64 "\n"), lino);
			goto clear_bad_out;
		}
		break;
	default:
		break;
	}

	blkmap_free(dblkmap);

	/*
	 * check nlinks feature, if it's a version 1 inode,
	 * just leave nlinks alone.  even if it's set wrong,
	 * it'll be reset when read in.
	 */
	*dirty += process_check_inode_nlink_version(dino, lino);

	/*
	 * The entire metadata directory tree will be rebuilt during phase 6.
	 * Therefore, if we're at the end of phase 4 and this is a metadata
	 * file, zero the ondisk inode and the incore state.
	 */
	if (check_dups && zap_metadata && !no_modify) {
		zero_dinode(mp, dino, lino);
		*dirty += 1;
		*used = is_free;
		*isa_dir = 0;
	}

	return retval;

clear_bad_out:
	if (!no_modify)  {
		clear_dinode(mp, dino, lino);
		*dirty += 1;
	}
bad_out:
	*used = is_free;
	*isa_dir = 0;
	blkmap_free(dblkmap);
	if (is_meta) {
		struct ino_tree_node	*irec;
		int			off;

		irec = find_inode_rec(mp, agno, ino);
		off = get_inode_offset(mp, lino, irec);
		clear_inode_is_meta(irec, off);
	}
	return 1;
}

/*
 * returns 1 if inode is used, 0 if free.
 * performs any necessary salvaging actions.
 * note that we leave the generation count alone
 * because nothing we could set it to would be
 * guaranteed to be correct so the best guess for
 * the correct value is just to leave it alone.
 *
 * The trick is detecting empty files.  For those,
 * the core and the forks should all be in the "empty"
 * or zero-length state -- a zero or possibly minimum length
 * (in the case of dirs) extent list -- although inline directories
 * and symlinks might be handled differently.  So it should be
 * possible to sanity check them against each other.
 *
 * If the forks are an empty extent list though, then forget it.
 * The file is toast anyway since we can't recover its storage.
 *
 * Parameters:
 *	Ins:
 *		mp -- mount structure
 *		dino -- pointer to on-disk inode structure
 *		agno/ino -- inode numbers
 *		free -- whether the map thinks the inode is free (1 == free)
 *		ino_discovery -- whether we should examine directory
 *				contents to discover new inodes
 *		check_dups -- whether we should check to see if the
 *				inode references duplicate blocks
 *				if so, we compare the inode's claimed
 *				blocks against the contents of the
 *				duplicate extent list but we don't
 *				set the bitmap.  If not, we set the
 *				bitmap and try and detect multiply
 *				claimed blocks using the bitmap.
 *	Outs:
 *		dirty -- whether we changed the inode (1 == yes)
 *		used -- 1 if the inode is used, 0 if free.  In no modify
 *			mode, whether the inode should be used or free
 *		isa_dir -- 1 if the inode is a directory, 0 if not.  In
 *			no modify mode, if the inode would be a dir or not.
 *
 *	Return value -- 0 if the inode is good, 1 if it is/was corrupt
 */

int
process_dinode(
	struct xfs_mount	*mp,
	struct xfs_dinode	**dinop,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino,
	int			was_free,
	int			*dirty,
	int			*used,
	int			ino_discovery,
	int			check_dups,
	int			extra_attr_check,
	int			*isa_dir,
	xfs_ino_t		*parent,
	struct xfs_buf		**ino_bpp)
{
	const int		verify_mode = 0;
	const int		uncertain = 0;

#ifdef XR_INODE_TRACE
	fprintf(stderr, _("processing inode %d/%d\n"), agno, ino);
#endif
	return process_dinode_int(mp, dinop, agno, ino, was_free, dirty, used,
			verify_mode, uncertain, ino_discovery,
			check_dups, extra_attr_check, isa_dir, parent,
			ino_bpp);
}

/*
 * a more cursory check, check inode core, *DON'T* check forks
 * this basically just verifies whether the inode is an inode
 * and whether or not it has been totally trashed.  returns 0
 * if the inode passes the cursory sanity check, 1 otherwise.
 */
int
verify_dinode(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino)
{
	xfs_ino_t		parent;
	int			used = 0;
	int			dirty = 0;
	int			isa_dir = 0;
	const int		verify_mode = 1;
	const int		check_dups = 0;
	const int		ino_discovery = 0;
	const int		uncertain = 0;

	return process_dinode_int(mp, &dino, agno, ino, 0, &dirty, &used,
			verify_mode, uncertain, ino_discovery,
			check_dups, 0, &isa_dir, &parent, NULL);
}

/*
 * like above only for inode on the uncertain list.  it sets
 * the uncertain flag which makes process_dinode_int quieter.
 * returns 0 if the inode passes the cursory sanity check, 1 otherwise.
 */
int
verify_uncertain_dinode(
	xfs_mount_t		*mp,
	struct xfs_dinode	*dino,
	xfs_agnumber_t		agno,
	xfs_agino_t		ino)
{
	xfs_ino_t		parent;
	int			used = 0;
	int			dirty = 0;
	int			isa_dir = 0;
	const int		verify_mode = 1;
	const int		check_dups = 0;
	const int		ino_discovery = 0;
	const int		uncertain = 1;

	return process_dinode_int(mp, &dino, agno, ino, 0, &dirty, &used,
				verify_mode, uncertain, ino_discovery,
				check_dups, 0, &isa_dir, &parent, NULL);
}
