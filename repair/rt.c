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
#include "rt.h"

/* Computed rt bitmap/summary data */
static union xfs_rtword_raw	*btmcompute;
static union xfs_suminfo_raw	*sumcompute;

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

/*
 * generate the real-time bitmap and summary info based on the
 * incore realtime extent map.
 */
void
generate_rtinfo(
	struct xfs_mount	*mp)
{
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
	btmcompute = calloc(wordcnt, sizeof(union xfs_rtword_raw));
	if (!btmcompute)
		do_error(
_("couldn't allocate memory for incore realtime bitmap.\n"));
	words = btmcompute;

	wordcnt = XFS_FSB_TO_B(mp, mp->m_rsumblocks) >> XFS_WORDLOG;
	sumcompute = calloc(wordcnt, sizeof(union xfs_suminfo_raw));
	if (!sumcompute)
		do_error(
_("couldn't allocate memory for incore realtime summary info.\n"));

	/*
	 * Slower but simple, don't play around with trying to set things one
	 * word at a time, just set bit as required.  Have to track start and
	 * end (size) of each range of free extents to set the summary info
	 * properly.
	 */
	while (extno < mp->m_sb.sb_rextents)  {
		xfs_rtword_t		freebit = 1;
		xfs_rtword_t		bits = 0;
		int			i;

		set_rtword(mp, words, 0);
		for (i = 0; i < sizeof(xfs_rtword_t) * NBBY &&
				extno < mp->m_sb.sb_rextents; i++, extno++)  {
			if (get_rtbmap(extno) == XR_E_FREE)  {
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
				inc_sumcount(mp, sumcompute, offs);
				in_extent = false;
			}

			freebit <<= 1;
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
		inc_sumcount(mp, sumcompute, offs);
	}

	if (mp->m_sb.sb_frextents != sb_frextents) {
		do_warn(_("sb_frextents %" PRIu64 ", counted %" PRIu64 "\n"),
				mp->m_sb.sb_frextents, sb_frextents);
	}
}

static void
check_rtwords(
	struct xfs_mount	*mp,
	const char		*filename,
	unsigned long long	bno,
	void			*ondisk,
	void			*incore)
{
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
 _("discrepancy in %s at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
					filename, bno, badstart, j - 1, wordcnt);
			badstart = -1;
			continue;
		}

		if (badstart == -1)
			badstart = j;
	}

	if (badstart >= 0)
		do_warn(
 _("discrepancy in %s at dblock 0x%llx words 0x%x-0x%x/0x%x\n"),
					filename, bno, badstart, wordcnt,
					wordcnt);
}

static void
check_rtfile_contents(
	struct xfs_mount	*mp,
	enum xfs_metafile_type	metafile_type,
	xfs_fileoff_t		filelen)
{
	struct xfs_bmbt_irec	map;
	struct xfs_buf		*bp;
	struct xfs_inode	*ip;
	const char		*filename;
	void			*buf;
	xfs_ino_t		ino;
	xfs_fileoff_t		bno = 0;
	int			error;

	switch (metafile_type) {
	case XFS_METAFILE_RTBITMAP:
		ino = mp->m_sb.sb_rbmino;
		filename = "rtbitmap";
		buf = btmcompute;
		break;
	case XFS_METAFILE_RTSUMMARY:
		ino = mp->m_sb.sb_rsumino;
		filename = "rtsummary";
		buf = sumcompute;
		break;
	default:
		return;
	}

	error = -libxfs_metafile_iget(mp, ino, metafile_type, &ip);
	if (error) {
		do_warn(_("unable to open %s file, err %d\n"), filename, error);
		return;
	}

	if (ip->i_disk_size != XFS_FSB_TO_B(mp, filelen)) {
		do_warn(_("expected %s file size %llu, found %llu\n"),
				filename,
				(unsigned long long)XFS_FSB_TO_B(mp, filelen),
				(unsigned long long)ip->i_disk_size);
	}

	while (bno < filelen)  {
		xfs_filblks_t	maplen;
		int		nmap = 1;

		/* Read up to 1MB at a time. */
		maplen = min(filelen - bno, XFS_B_TO_FSBT(mp, 1048576));
		error = -libxfs_bmapi_read(ip, bno, maplen, &map, &nmap, 0);
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
				XFS_FSB_TO_BB(mp, map.br_blockcount),
				0, &bp, NULL);
		if (error) {
			do_warn(_("unable to read %s at dblock 0x%llx, err %d\n"),
					filename, (unsigned long long)bno, error);
			break;
		}

		check_rtwords(mp, filename, bno, bp->b_addr, buf);

		buf += XFS_FSB_TO_B(mp, map.br_blockcount);
		bno += map.br_blockcount;
		libxfs_buf_relse(bp);
	}

	libxfs_irele(ip);
}

void
check_rtbitmap(
	struct xfs_mount	*mp)
{
	if (need_rbmino)
		return;

	check_rtfile_contents(mp, XFS_METAFILE_RTBITMAP,
			mp->m_sb.sb_rbmblocks);
}

void
check_rtsummary(
	struct xfs_mount	*mp)
{
	if (need_rsumino)
		return;

	check_rtfile_contents(mp, XFS_METAFILE_RTSUMMARY, mp->m_rsumblocks);
}

void
fill_rtbitmap(
	struct xfs_rtgroup	*rtg)
{
	int			error;

	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_BITMAP,
			0, rtg_mount(rtg)->m_sb.sb_rbmblocks, btmcompute);
	if (error)
		do_error(
_("couldn't re-initialize realtime bitmap inode, error %d\n"), error);
}

void
fill_rtsummary(
	struct xfs_rtgroup	*rtg)
{
	int			error;

	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_SUMMARY,
			0, rtg_mount(rtg)->m_rsumblocks, sumcompute);
	if (error)
		do_error(
_("couldn't re-initialize realtime summary inode, error %d\n"), error);
}
