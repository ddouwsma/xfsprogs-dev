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
#include "dinode.h"
#include "protos.h"
#include "err_protos.h"
#include "libfrog/bitmap.h"
#include "rt.h"

/* Bitmap of rt group inodes */
static struct bitmap	*rtg_inodes[XFS_RTGI_MAX];
static bool		rtginodes_bad[XFS_RTGI_MAX];

/* Computed rt bitmap/summary data */
struct rtg_computed {
	union xfs_rtword_raw	*bmp;
	union xfs_suminfo_raw	*sum;
};
struct rtg_computed *rt_computed;

static inline void
set_rtword(
	struct xfs_mount	*mp,
	union xfs_rtword_raw	*word,
	xfs_rtword_t		value)
{
	if (xfs_has_rtgroups(mp))
		word->rtg = cpu_to_be32(value);
	else
		word->old = value;
}

static inline void
inc_sumcount(
	struct xfs_mount	*mp,
	union xfs_suminfo_raw	*info,
	xfs_rtsumoff_t		index)
{
	union xfs_suminfo_raw	*p = info + index;

	if (xfs_has_rtgroups(mp))
		be32_add_cpu(&p->rtg, 1);
	else
		p->old++;
}

static void
generate_rtgroup_rtinfo(
	struct xfs_rtgroup	*rtg)
{
	struct rtg_computed	*comp = &rt_computed[rtg_rgno(rtg)];
	struct xfs_mount	*mp = rtg_mount(rtg);
	unsigned int		bitsperblock =
		mp->m_blockwsize << XFS_NBWORDLOG;
	xfs_rtxnum_t		extno = 0;
	xfs_rtxnum_t		start_ext = 0;
	int			bmbno = 0;
	int			start_bmbno = 0;
	bool			in_extent = false;
	unsigned long long	wordcnt;
	union xfs_rtword_raw	*words;

	wordcnt = XFS_FSB_TO_B(mp, mp->m_sb.sb_rbmblocks) >> XFS_WORDLOG;
	comp->bmp = calloc(wordcnt, sizeof(union xfs_rtword_raw));
	if (!comp->bmp)
		do_error(
_("couldn't allocate memory for incore realtime bitmap.\n"));
	words = comp->bmp;

	wordcnt = XFS_FSB_TO_B(mp, mp->m_rsumblocks) >> XFS_WORDLOG;
	comp->sum = calloc(wordcnt, sizeof(union xfs_suminfo_raw));
	if (!comp->sum)
		do_error(
_("couldn't allocate memory for incore realtime summary info.\n"));

	/*
	 * Slower but simple, don't play around with trying to set things one
	 * word at a time, just set bit as required.  Have to track start and
	 * end (size) of each range of free extents to set the summary info
	 * properly.
	 */
	while (extno < rtg->rtg_extents) {
		xfs_rtword_t		freebit = 1;
		xfs_rtword_t		bits = 0;
		int			state, i;

		set_rtword(mp, words, 0);
		for (i = 0; i < sizeof(xfs_rtword_t) * NBBY; i++) {
			if (extno == rtg->rtg_extents)
				break;

			/*
			 * Note: for the RTG case it might make sense to use
			 * get_rgbmap_ext here and generate multiple bitmap
			 * entries per lookup.
			 */
			if (xfs_has_rtgroups(mp))
				state = get_rgbmap(rtg_rgno(rtg),
					extno * mp->m_sb.sb_rextsize);
			else
				state = get_rtbmap(extno);
			if (state == XR_E_FREE)  {
				sb_frextents++;
				bits |= freebit;

				if (!in_extent) {
					start_ext = extno;
					start_bmbno = bmbno;
					in_extent = true;
				}
			} else if (in_extent) {
				uint64_t	len = extno - start_ext;
				xfs_rtsumoff_t	offs;

				offs = xfs_rtsumoffs(mp, libxfs_highbit64(len),
						start_bmbno);
				inc_sumcount(mp, comp->sum, offs);
				in_extent = false;
			}

			freebit <<= 1;
			extno++;
		}
		set_rtword(mp, words, bits);
		words++;

		if (extno % bitsperblock == 0)
			bmbno++;
	}

	if (in_extent) {
		uint64_t	len = extno - start_ext;
		xfs_rtsumoff_t	offs;

		offs = xfs_rtsumoffs(mp, libxfs_highbit64(len), start_bmbno);
		inc_sumcount(mp, comp->sum, offs);
	}
}

/*
 * generate the real-time bitmap and summary info based on the
 * incore realtime extent map.
 */
void
generate_rtinfo(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;

	rt_computed = calloc(mp->m_sb.sb_rgcount, sizeof(struct rtg_computed));
	if (!rt_computed)
		do_error(
	_("couldn't allocate memory for incore realtime info.\n"));

	while ((rtg = xfs_rtgroup_next(mp, rtg)))
		generate_rtgroup_rtinfo(rtg);

	if (mp->m_sb.sb_frextents != sb_frextents) {
		do_warn(_("sb_frextents %" PRIu64 ", counted %" PRIu64 "\n"),
				mp->m_sb.sb_frextents, sb_frextents);
	}
}

static void
check_rtwords(
	struct xfs_rtgroup	*rtg,
	const char		*filename,
	unsigned long long	bno,
	void			*ondisk,
	void			*incore)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	unsigned int		wordcnt = mp->m_blockwsize;
	union xfs_rtword_raw	*o = ondisk, *i = incore;
	int			badstart = -1;
	unsigned int		j;

	if (memcmp(ondisk, incore, wordcnt << XFS_WORDLOG) == 0)
		return;

	for (j = 0; j < wordcnt; j++, o++, i++) {
		if (o->old == i->old) {
			/* Report a range of inconsistency that just ended. */
			if (badstart >= 0)
				do_warn(
 _("discrepancy in %s (%u) at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
					filename, rtg_rgno(rtg), bno,
					badstart, j - 1, wordcnt);
			badstart = -1;
			continue;
		}

		if (badstart == -1)
			badstart = j;
	}

	if (badstart >= 0)
		do_warn(
 _("discrepancy in %s (%u) at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
			filename, rtg_rgno(rtg), bno,
			badstart, wordcnt, wordcnt);
}

static void
check_rtfile_contents(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type,
	void			*buf,
	xfs_fileoff_t		filelen)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	const char		*filename = libxfs_rtginode_name(type);
	struct xfs_inode	*ip = rtg->rtg_inodes[type];
	xfs_fileoff_t		bno = 0;
	int			error;

	if (!ip) {
		do_warn(_("unable to open %s file\n"), filename);
		return;
	}

	if (ip->i_disk_size != XFS_FSB_TO_B(mp, filelen)) {
		do_warn(_("expected %s file size %llu, found %llu\n"),
				filename,
				(unsigned long long)XFS_FSB_TO_B(mp, filelen),
				(unsigned long long)ip->i_disk_size);
	}

	while (bno < filelen)  {
		struct xfs_bmbt_irec	map;
		struct xfs_buf		*bp;
		int			nmap = 1;

		error = -libxfs_bmapi_read(ip, bno, 1, &map, &nmap, 0);
		if (error) {
			do_warn(_("unable to read %s mapping, err %d\n"),
					filename, error);
			break;
		}

		if (map.br_startblock == HOLESTARTBLOCK) {
			do_warn(_("hole in %s file at dblock 0x%llx\n"),
					filename, (unsigned long long)bno);
			break;
		}

		error = -libxfs_buf_read_uncached(mp->m_dev,
				XFS_FSB_TO_DADDR(mp, map.br_startblock),
				XFS_FSB_TO_BB(mp, 1), 0, &bp,
				xfs_rtblock_ops(mp, type));
		if (error) {
			do_warn(_("unable to read %s at dblock 0x%llx, err %d\n"),
					filename, (unsigned long long)bno, error);
			break;
		}

		check_rtwords(rtg, filename, bno, bp->b_addr, buf);

		buf += mp->m_blockwsize << XFS_WORDLOG;
		bno++;
		libxfs_buf_relse(bp);
	}
}

/*
 * Try to load a sb-rooted rt metadata file now, since earlier phases may have
 * fixed verifier problems in the root inode chunk.
 */
static void
try_load_sb_rtfile(
	struct xfs_mount	*mp,
	enum xfs_rtg_inodes	type)
{
	struct xfs_rtgroup	*rtg = libxfs_rtgroup_grab(mp, 0);
	struct xfs_trans	*tp;
	int			error;

	if (rtg->rtg_inodes[type])
		goto out_rtg;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		goto out_rtg;


	error = -libxfs_rtginode_load(rtg, type, tp);
	if (error)
		goto out_cancel;

	/* If we can't load the inode, signal to phase 6 to recreate it. */
	if (!rtg->rtg_inodes[type]) {
		switch (type) {
		case XFS_RTGI_BITMAP:
			need_rbmino = 1;
			break;
		case XFS_RTGI_SUMMARY:
			need_rsumino = 1;
			break;
		default:
			ASSERT(0);
			break;
		}
	}

out_cancel:
	libxfs_trans_cancel(tp);
out_rtg:
	libxfs_rtgroup_rele(rtg);
}

void
check_rtbitmap(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;

	if (need_rbmino)
		return;

	if (!xfs_has_rtgroups(mp))
		try_load_sb_rtfile(mp, XFS_RTGI_BITMAP);

	while ((rtg = xfs_rtgroup_next(mp, rtg))) {
		check_rtfile_contents(rtg, XFS_RTGI_BITMAP,
				rt_computed[rtg_rgno(rtg)].bmp,
				mp->m_sb.sb_rbmblocks);
	}
}

void
check_rtsummary(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;

	if (need_rsumino)
		return;

	if (!xfs_has_rtgroups(mp))
		try_load_sb_rtfile(mp, XFS_RTGI_SUMMARY);

	while ((rtg = xfs_rtgroup_next(mp, rtg))) {
		check_rtfile_contents(rtg, XFS_RTGI_SUMMARY,
				rt_computed[rtg_rgno(rtg)].sum,
				mp->m_rsumblocks);
	}
}

void
fill_rtbitmap(
	struct xfs_rtgroup	*rtg)
{
	int			error;

	/*
	 * For file systems without a RT subvolume we have the bitmap and
	 * summary files, but they are empty.  In that case rt_computed is
	 * NULL.
	 */
	if (!rt_computed)
		return;

	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_BITMAP,
			0, rtg_mount(rtg)->m_sb.sb_rbmblocks,
			rt_computed[rtg_rgno(rtg)].bmp);
	if (error)
		do_error(
_("couldn't re-initialize realtime bitmap inode, error %d\n"), error);
}

void
fill_rtsummary(
	struct xfs_rtgroup	*rtg)
{
	int			error;

	/*
	 * For file systems without a RT subvolume we have the bitmap and
	 * summary files, but they are empty.  In that case rt_computed is
	 * NULL.
	 */
	if (!rt_computed)
		return;

	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_SUMMARY,
			0, rtg_mount(rtg)->m_rsumblocks,
			rt_computed[rtg_rgno(rtg)].sum);
	if (error)
		do_error(
_("couldn't re-initialize realtime summary inode, error %d\n"), error);
}

bool
is_rtgroup_inode(
	xfs_ino_t		ino,
	enum xfs_rtg_inodes	type)
{
	if (!rtg_inodes[type])
		return false;
	return bitmap_test(rtg_inodes[type], ino, 1);
}

bool
rtgroup_inodes_were_bad(
	enum xfs_rtg_inodes	type)
{
	return rtginodes_bad[type];
}

void
mark_rtgroup_inodes_bad(
	struct xfs_mount	*mp,
	enum xfs_rtg_inodes	type)
{
	struct xfs_rtgroup	*rtg = NULL;

	while ((rtg = xfs_rtgroup_next(mp, rtg)))
		libxfs_rtginode_irele(&rtg->rtg_inodes[type]);

	rtginodes_bad[type] = true;
}

static inline int
mark_rtginode(
	struct xfs_trans	*tp,
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	struct xfs_inode	*ip;
	int			error;

	if (!xfs_rtginode_enabled(rtg, type))
		return 0;

	error = -libxfs_rtginode_load(rtg, type, tp);
	if (error)
		goto out_corrupt;

	ip = rtg->rtg_inodes[type];
	if (!ip)
		goto out_corrupt;

	if (xfs_has_rtgroups(rtg_mount(rtg))) {
		if (bitmap_test(rtg_inodes[type], ip->i_ino, 1)) {
			error = EFSCORRUPTED;
			goto out_corrupt;
		}

		error = bitmap_set(rtg_inodes[type], ip->i_ino, 1);
		if (error)
			goto out_corrupt;
	}

	/*
	 * Phase 3 will clear the ondisk inodes of all rt metadata files, but
	 * it doesn't reset any blocks.  Keep the incore inodes loaded so that
	 * phase 4 can check the rt metadata.  These inodes must be dropped
	 * before rebuilding can begin during phase 6.
	 */
	return 0;

out_corrupt:
	rtginodes_bad[type] = true;
	return error;
}

/* Mark the reachable rt metadata inodes prior to the inode scan. */
void
discover_rtgroup_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;
	struct xfs_trans	*tp;
	int			error, err2;
	int			i;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error)
		goto out;
	if (xfs_has_rtgroups(mp) && mp->m_sb.sb_rgcount > 0) {
		error = -libxfs_rtginode_load_parent(tp);
		if (error)
			goto out_cancel;
	}

	while ((rtg = xfs_rtgroup_next(mp, rtg))) {
		for (i = 0; i < XFS_RTGI_MAX; i++) {
			err2 = mark_rtginode(tp, rtg, i);
			if (err2 && !error)
				error = err2;
		}
	}

out_cancel:
	libxfs_trans_cancel(tp);
out:
	if (xfs_has_rtgroups(mp) && error) {
		/*
		 * Old xfs_repair didn't complain if rtbitmaps didn't load
		 * until phase 5, so only turn on extra warnings during phase 2
		 * for newer filesystems.
		 */
		switch (error) {
		case EFSCORRUPTED:
			do_warn(
 _("corruption in metadata directory tree while discovering rt group inodes\n"));
			break;
		default:
			do_warn(
 _("couldn't discover rt group inodes, err %d\n"),
						error);
			break;
		}
	}
}

/* Unload incore rtgroup inodes before rebuilding rt metadata. */
void
unload_rtgroup_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;
	unsigned int		i;

	while ((rtg = xfs_rtgroup_next(mp, rtg)))
		for (i = 0; i < XFS_RTGI_MAX; i++)
			libxfs_rtginode_irele(&rtg->rtg_inodes[i]);

	libxfs_rtginode_irele(&mp->m_rtdirip);
}

void
init_rtgroup_inodes(void)
{
	unsigned int		i;
	int			error;

	for (i = 0; i < XFS_RTGI_MAX; i++) {
		error = bitmap_alloc(&rtg_inodes[i]);
		if (error)
			break;
	}

	if (error)
		do_error(_("could not allocate rtginode bitmap, err=%d!\n"),
				error);
}

void
free_rtgroup_inodes(void)
{
	int			i;

	for (i = 0; i < XFS_RTGI_MAX; i++)
		bitmap_free(&rtg_inodes[i]);
}
