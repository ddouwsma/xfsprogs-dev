// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "type.h"
#include "fprint.h"
#include "faddr.h"
#include "field.h"
#include "bmap.h"
#include "io.h"
#include "inode.h"
#include "output.h"
#include "init.h"

static int		bmap_f(int argc, char **argv);
static int		bmap_one_extent(xfs_bmbt_rec_t *ep,
					xfs_fileoff_t *offp, xfs_fileoff_t eoff,
					xfs_extnum_t *idxp, bmap_ext_t *bep);
static xfs_fsblock_t	select_child(xfs_fileoff_t off, xfs_bmbt_key_t *kp,
				     xfs_bmbt_ptr_t *pp, int nrecs);

static const cmdinfo_t	bmap_cmd =
	{ "bmap", NULL, bmap_f, 0, 3, 0, N_("[-ad] [block [len]]"),
	  N_("show block map for current file"), NULL };

void
bmap(
	xfs_fileoff_t		offset,
	xfs_filblks_t		len,
	int			whichfork,
	xfs_extnum_t		*nexp,
	bmap_ext_t		*bep)
{
	struct xfs_btree_block	*block;
	xfs_fsblock_t		bno;
	xfs_fileoff_t		curoffset;
	struct xfs_dinode	*dip;
	xfs_fileoff_t		eoffset;
	xfs_bmbt_rec_t		*ep;
	enum xfs_dinode_fmt	fmt;
	int			fsize;
	xfs_bmbt_key_t		*kp;
	xfs_extnum_t		n;
	int			nex;
	xfs_fsblock_t		nextbno;
	xfs_extnum_t		nextents;
	xfs_bmbt_ptr_t		*pp;
	xfs_bmdr_block_t	*rblock;
	typnm_t			typ;
	xfs_bmbt_rec_t		*xp;

	push_cur();
	set_cur_inode(iocur_top->ino);
	nex = *nexp;
	*nexp = 0;
	ASSERT(nex > 0);
	dip = iocur_top->data;
	n = 0;
	eoffset = offset + len - 1;
	curoffset = offset;
	fmt = (enum xfs_dinode_fmt)XFS_DFORK_FORMAT(dip, whichfork);
	typ = whichfork == XFS_DATA_FORK ? TYP_BMAPBTD : TYP_BMAPBTA;
	ASSERT(typtab[typ].typnm == typ);
	switch (fmt) {
	case XFS_DINODE_FMT_LOCAL:
		break;
	case XFS_DINODE_FMT_EXTENTS:
		nextents = xfs_dfork_nextents(dip, whichfork);
		xp = (xfs_bmbt_rec_t *)XFS_DFORK_PTR(dip, whichfork);
		for (ep = xp; ep < &xp[nextents] && n < nex; ep++) {
			if (!bmap_one_extent(ep, &curoffset, eoffset, &n, bep))
				break;
		}
		break;
	case XFS_DINODE_FMT_BTREE:
		push_cur();
		rblock = (xfs_bmdr_block_t *)XFS_DFORK_PTR(dip, whichfork);
		fsize = XFS_DFORK_SIZE(dip, mp, whichfork);
		pp = xfs_bmdr_ptr_addr(rblock, 1, libxfs_bmdr_maxrecs(fsize, 0));
		kp = xfs_bmdr_key_addr(rblock, 1);
		bno = select_child(curoffset, kp, pp,
					be16_to_cpu(rblock->bb_numrecs));
		for (;;) {
			set_cur(&typtab[typ], XFS_FSB_TO_DADDR(mp, bno),
				blkbb, DB_RING_IGN, NULL);
			block = (struct xfs_btree_block *)iocur_top->data;
			if (be16_to_cpu(block->bb_level) == 0)
				break;
			pp = xfs_bmbt_ptr_addr(mp, block, 1,
				libxfs_bmbt_maxrecs(mp, mp->m_sb.sb_blocksize, 0));
			kp = xfs_bmbt_key_addr(mp, block, 1);
			bno = select_child(curoffset, kp, pp,
					be16_to_cpu(block->bb_numrecs));
		}
		for (;;) {
			nextbno = be64_to_cpu(block->bb_u.l.bb_rightsib);
			nextents = be16_to_cpu(block->bb_numrecs);
			xp = (xfs_bmbt_rec_t *)
				xfs_bmbt_rec_addr(mp, block, 1);
			for (ep = xp; ep < &xp[nextents] && n < nex; ep++) {
				if (!bmap_one_extent(ep, &curoffset, eoffset,
						&n, bep)) {
					nextbno = NULLFSBLOCK;
					break;
				}
			}
			bno = nextbno;
			if (bno == NULLFSBLOCK)
				break;
			set_cur(&typtab[typ], XFS_FSB_TO_DADDR(mp, bno),
				blkbb, DB_RING_IGN, NULL);
			block = (struct xfs_btree_block *)iocur_top->data;
		}
		pop_cur();
		break;
	default:
		dbprintf(
 _("%s fork format %u does not support indexable blocks\n"),
				whichfork == XFS_DATA_FORK ? "data" : "attr",
				fmt);
		break;
	}
	pop_cur();
	*nexp = n;
}

static void
print_group_bmbt(
	bool			isrt,
	int			whichfork,
	const struct bmap_ext	*be)
{
	unsigned int		gno;
	unsigned long long	gbno;

	if (whichfork == XFS_DATA_FORK && isrt) {
		gno = xfs_fsb_to_gno(mp, be->startblock, XG_TYPE_RTG);
		gbno = xfs_fsb_to_gbno(mp, be->startblock, XG_TYPE_RTG);
	} else {
		gno = xfs_fsb_to_gno(mp, be->startblock, XG_TYPE_AG);
		gbno = xfs_fsb_to_gbno(mp, be->startblock, XG_TYPE_AG);
	}

	dbprintf(
 _("%s offset %lld startblock %llu (%u/%llu) count %llu flag %u\n"),
			whichfork == XFS_DATA_FORK ? _("data") : _("attr"),
			be->startoff, be->startblock,
			gno, gbno,
			be->blockcount, be->flag);
}

static void
print_linear_bmbt(
	const struct bmap_ext	*be)
{
	dbprintf(_("%s offset %lld startblock %llu count %llu flag %u\n"),
			_("data"),
			be->startoff, be->startblock,
			be->blockcount, be->flag);
}

static int
bmap_f(
	int			argc,
	char			**argv)
{
	int			afork = 0;
	bmap_ext_t		be;
	int			c;
	xfs_fileoff_t		co, cosave;
	int			dfork = 0;
	struct xfs_dinode	*dip;
	xfs_fileoff_t		eo;
	xfs_filblks_t		len;
	xfs_extnum_t		nex;
	char			*p;
	int			whichfork;
	bool			isrt;

	if (iocur_top->ino == NULLFSINO) {
		dbprintf(_("no current inode\n"));
		return 0;
	}
	optind = 0;
	if (argc) while ((c = getopt(argc, argv, "ad")) != EOF) {
		switch (c) {
		case 'a':
			afork = 1;
			break;
		case 'd':
			dfork = 1;
			break;
		default:
			dbprintf(_("bad option for bmap command\n"));
			return 0;
		}
	}

	dip = iocur_top->data;
	isrt = (dip->di_flags & cpu_to_be16(XFS_DIFLAG_REALTIME));

	if (afork + dfork == 0) {
		push_cur();
		set_cur_inode(iocur_top->ino);
		dip = iocur_top->data;
		if (xfs_dfork_data_extents(dip))
			dfork = 1;
		if (xfs_dfork_attr_extents(dip))
			afork = 1;
		pop_cur();
	}
	if (optind < argc) {
		co = (xfs_fileoff_t)strtoull(argv[optind], &p, 0);
		if (*p != '\0') {
			dbprintf(_("bad block number for bmap %s\n"),
				argv[optind]);
			return 0;
		}
		optind++;
		if (optind < argc) {
			len = (xfs_filblks_t)strtoull(argv[optind], &p, 0);
			if (*p != '\0') {
				dbprintf(_("bad len for bmap %s\n"), argv[optind]);
				return 0;
			}
			eo = co + len - 1;
		} else
			eo = co;
	} else {
		co = 0;
		eo = -1;
	}
	cosave = co;
	for (whichfork = XFS_DATA_FORK;
	     whichfork <= XFS_ATTR_FORK;
	     whichfork++) {
		if (whichfork == XFS_DATA_FORK && !dfork)
			continue;
		if (whichfork == XFS_ATTR_FORK && !afork)
			continue;
		for (;;) {
			nex = 1;
			bmap(co, eo - co + 1, whichfork, &nex, &be);
			if (nex == 0)
				break;

			if (whichfork == XFS_DATA_FORK && isrt) {
				if (xfs_has_rtgroups(mp))
					print_group_bmbt(isrt, whichfork, &be);
				else
					print_linear_bmbt(&be);
			} else {
				print_group_bmbt(isrt, whichfork, &be);
			}
			co = be.startoff + be.blockcount;
		}
		co = cosave;
	}
	return 0;
}

void
bmap_init(void)
{
	add_command(&bmap_cmd);
}

static int
bmap_one_extent(
	xfs_bmbt_rec_t		*ep,
	xfs_fileoff_t		*offp,
	xfs_fileoff_t		eoff,
	xfs_extnum_t		*idxp,
	bmap_ext_t		*bep)
{
	xfs_filblks_t		c;
	xfs_fileoff_t		curoffset;
	int			f;
	xfs_extnum_t		idx;
	xfs_fileoff_t		o;
	xfs_fsblock_t		s;

	convert_extent(ep, &o, &s, &c, &f);
	curoffset = *offp;
	idx = *idxp;
	if (o + c <= curoffset)
		return 1;
	if (o > eoff)
		return 0;
	if (o < curoffset) {
		c -= curoffset - o;
		s += curoffset - o;
		o = curoffset;
	}
	if (o + c - 1 > eoff)
		c -= (o + c - 1) - eoff;
	bep[idx].startoff = o;
	bep[idx].startblock = s;
	bep[idx].blockcount = c;
	bep[idx].flag = f;
	*idxp = idx + 1;
	*offp = o + c;
	return 1;
}

void
convert_extent(
	xfs_bmbt_rec_t		*rp,
	xfs_fileoff_t		*op,
	xfs_fsblock_t		*sp,
	xfs_filblks_t		*cp,
	int			*fp)
{
	struct xfs_bmbt_irec	irec;

	libxfs_bmbt_disk_get_all(rp, &irec);
	*fp = irec.br_state == XFS_EXT_UNWRITTEN;
	*op = irec.br_startoff;
	*sp = irec.br_startblock;
	*cp = irec.br_blockcount;
}

void
make_bbmap(
	bbmap_t		*bbmap,
	xfs_extnum_t	nex,
	bmap_ext_t	*bmp)
{
	xfs_extnum_t	i;

	for (i = 0; i < nex; i++) {
		bbmap->b[i].bm_bn = XFS_FSB_TO_DADDR(mp, bmp[i].startblock);
		bbmap->b[i].bm_len = XFS_FSB_TO_BB(mp, bmp[i].blockcount);
	}
	bbmap->nmaps = nex;
}

static xfs_fsblock_t
select_child(
	xfs_fileoff_t	off,
	xfs_bmbt_key_t	*kp,
	xfs_bmbt_ptr_t	*pp,
	int		nrecs)
{
	int		i;

	for (i = 0; i < nrecs; i++) {
		if (be64_to_cpu(kp[i].br_startoff) == off)
			return be64_to_cpu(pp[i]);
		if (be64_to_cpu(kp[i].br_startoff) > off) {
			if (i == 0)
				return be64_to_cpu(pp[i]);
			else
				return be64_to_cpu(pp[i - 1]);
		}
	}
	return be64_to_cpu(pp[nrecs - 1]);
}
