// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "libxlog.h"
#include "command.h"
#include "type.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "io.h"
#include "sb.h"
#include "bit.h"
#include "output.h"
#include "init.h"
#include "rtgroup.h"

#define uuid_equal(s,d)		(platform_uuid_compare((s),(d)) == 0)

static int	rtsb_f(int argc, char **argv);
static void     rtsb_help(void);

static const cmdinfo_t	rtsb_cmd =
	{ "rtsb", NULL, rtsb_f, 0, 0, 1, "",
	  N_("set current address to realtime sb header"), rtsb_help };

void
rtsb_init(void)
{
	if (xfs_has_rtgroups(mp))
		add_command(&rtsb_cmd);
}

#define	OFF(f)	bitize(offsetof(struct xfs_rtsb, rsb_ ## f))
#define	SZC(f)	szcount(struct xfs_rtsb, rsb_ ## f)
const field_t	rtsb_flds[] = {
	{ "magicnum", FLDT_UINT32X, OI(OFF(magicnum)), C1, 0, TYP_NONE },
	{ "crc", FLDT_CRC, OI(OFF(crc)), C1, 0, TYP_NONE },
	{ "pad", FLDT_UINT32X, OI(OFF(pad)), C1, 0, TYP_NONE },
	{ "fname", FLDT_CHARNS, OI(OFF(fname)), CI(SZC(fname)), 0, TYP_NONE },
	{ "uuid", FLDT_UUID, OI(OFF(uuid)), C1, 0, TYP_NONE },
	{ "meta_uuid", FLDT_UUID, OI(OFF(meta_uuid)), C1, 0, TYP_NONE },
	{ NULL }
};
#undef OFF

const field_t	rtsb_hfld[] = {
	{ "", FLDT_RTSB, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};

static void
rtsb_help(void)
{
	dbprintf(_(
"\n"
" seek to realtime superblock\n"
"\n"
" Example:\n"
"\n"
" 'rtsb - set location to realtime superblock, set type to 'rtsb'\n"
"\n"
" Located in the first block of the realtime volume, the rt superblock\n"
" contains the base information for the realtime section of a filesystem.\n"
"\n"
));
}

static int
rtsb_f(
	int		argc,
	char		**argv)
{
	int		c;

	while ((c = getopt(argc, argv, "")) != -1) {
		switch (c) {
		default:
			rtsb_help();
			return 0;
		}
	}

	cur_agno = NULLAGNUMBER;

	ASSERT(typtab[TYP_RTSB].typnm == TYP_RTSB);
	set_rt_cur(&typtab[TYP_RTSB], XFS_RTSB_DADDR, XFS_FSB_TO_BB(mp, 1),
			DB_RING_ADD, NULL);
	return 0;
}

int
rtsb_size(
	void	*obj,
	int	startoff,
	int	idx)
{
	return bitize(mp->m_sb.sb_blocksize);
}

static int
rtwords_count(
	void			*obj,
	int			startoff)
{
	unsigned int		blksz = mp->m_sb.sb_blocksize;

	if (xfs_has_rtgroups(mp))
		blksz -= sizeof(struct xfs_rtbuf_blkinfo);

	return blksz >> XFS_WORDLOG;
}

#define	OFF(f)	bitize(offsetof(struct xfs_rtbuf_blkinfo, rt_ ## f))
const field_t	rgbitmap_flds[] = {
	{ "magicnum", FLDT_UINT32X, OI(OFF(magic)), C1, 0, TYP_NONE },
	{ "crc", FLDT_CRC, OI(OFF(crc)), C1, 0, TYP_NONE },
	{ "owner", FLDT_INO, OI(OFF(owner)), C1, 0, TYP_NONE },
	{ "bno", FLDT_DFSBNO, OI(OFF(blkno)), C1, 0, TYP_BMAPBTD },
	{ "lsn", FLDT_UINT64X, OI(OFF(lsn)), C1, 0, TYP_NONE },
	{ "uuid", FLDT_UUID, OI(OFF(uuid)), C1, 0, TYP_NONE },
	/* the rtword array is after the actual structure */
	{ "rtwords", FLDT_RTWORD, OI(bitize(sizeof(struct xfs_rtbuf_blkinfo))),
	  rtwords_count, FLD_ARRAY | FLD_COUNT, TYP_DATA },
	{ NULL }
};
#undef OFF

const field_t	rgbitmap_hfld[] = {
	{ "", FLDT_RGBITMAP, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};

#define	OFF(f)	bitize(offsetof(struct xfs_rtbuf_blkinfo, rt_ ## f))
const field_t	rgsummary_flds[] = {
	{ "magicnum", FLDT_UINT32X, OI(OFF(magic)), C1, 0, TYP_NONE },
	{ "crc", FLDT_CRC, OI(OFF(crc)), C1, 0, TYP_NONE },
	{ "owner", FLDT_INO, OI(OFF(owner)), C1, 0, TYP_NONE },
	{ "bno", FLDT_DFSBNO, OI(OFF(blkno)), C1, 0, TYP_BMAPBTD },
	{ "lsn", FLDT_UINT64X, OI(OFF(lsn)), C1, 0, TYP_NONE },
	{ "uuid", FLDT_UUID, OI(OFF(uuid)), C1, 0, TYP_NONE },
	/* the suminfo array is after the actual structure */
	{ "suminfo", FLDT_SUMINFO, OI(bitize(sizeof(struct xfs_rtbuf_blkinfo))),
	  rtwords_count, FLD_ARRAY | FLD_COUNT, TYP_DATA },
	{ NULL }
};
#undef OFF

const field_t	rgsummary_hfld[] = {
	{ "", FLDT_RGSUMMARY, OI(0), C1, 0, TYP_NONE },
	{ NULL }
};
