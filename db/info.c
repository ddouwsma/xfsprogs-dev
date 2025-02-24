// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */
#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "output.h"
#include "libfrog/fsgeom.h"
#include "libfrog/logging.h"

static void
info_help(void)
{
	dbprintf(_(
"\n"
" Pretty-prints the filesystem geometry as derived from the superblock.\n"
" The output has the same format as mkfs.xfs, xfs_info, and other utilities.\n"
"\n"
));

}

static int
info_f(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_geom	geo;

	libxfs_fs_geometry(mp, &geo, XFS_FS_GEOM_MAX_STRUCT_VER);
	xfs_report_geom(&geo, x.data.name, x.log.name, x.rt.name);
	return 0;
}

static const struct cmdinfo info_cmd = {
	.name =		"info",
	.altname =	"i",
	.cfunc =	info_f,
	.argmin =	0,
	.argmax =	0,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("pretty-print superblock info"),
	.help =		info_help,
};

static void
agresv_help(void)
{
	dbprintf(_(
"\n"
" Print the size and per-AG reservation information some allocation groups.\n"
"\n"
" Specific allocation group numbers can be provided as command line arguments.\n"
" If no arguments are provided, all allocation groups are iterated.\n"
"\n"
));

}

static void
print_agresv_info(
	struct xfs_perag *pag)
{
	struct xfs_buf	*bp;
	struct xfs_agf	*agf;
	xfs_agnumber_t	agno = pag_agno(pag);
	xfs_extlen_t	ask = 0;
	xfs_extlen_t	used = 0;
	xfs_extlen_t	free = 0;
	xfs_extlen_t	length = 0;
	int		error;

	error = -libxfs_refcountbt_calc_reserves(mp, NULL, pag, &ask, &used);
	if (error)
		xfrog_perror(error, "refcountbt");
	error = -libxfs_finobt_calc_reserves(pag, NULL, &ask, &used);
	if (error)
		xfrog_perror(error, "finobt");
	error = -libxfs_rmapbt_calc_reserves(mp, NULL, pag, &ask, &used);
	if (error)
		xfrog_perror(error, "rmapbt");

	error = -libxfs_read_agf(pag, NULL, 0, &bp);
	if (error)
		xfrog_perror(error, "AGF");
	agf = bp->b_addr;
	length = be32_to_cpu(agf->agf_length);
	free = be32_to_cpu(agf->agf_freeblks) +
	       be32_to_cpu(agf->agf_flcount);
	libxfs_buf_relse(bp);

	printf("AG %d: length: %u free: %u reserved: %u used: %u",
			agno, length, free, ask, used);
	if (ask - used > free)
		printf(" <not enough space>");
	printf("\n");
}

static int
agresv_f(
	int			argc,
	char			**argv)
{
	struct xfs_perag	*pag = NULL;
	int			i;

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			long	a;
			char	*p;

			errno = 0;
			a = strtol(argv[i], &p, 0);
			if (p == argv[i])
				errno = ERANGE;
			if (errno) {
				perror(argv[i]);
				continue;
			}

			if (a < 0 || a >= mp->m_sb.sb_agcount) {
				fprintf(stderr, "%ld: Not a AG.\n", a);
				continue;
			}

			pag = libxfs_perag_get(mp, a);
			print_agresv_info(pag);
			libxfs_perag_put(pag);
		}
		return 0;
	}

	while ((pag = xfs_perag_next(mp, pag)))
		print_agresv_info(pag);

	return 0;
}

static const struct cmdinfo agresv_cmd = {
	.name =		"agresv",
	.altname =	NULL,
	.cfunc =	agresv_f,
	.argmin =	0,
	.argmax =	-1,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("print AG reservation stats"),
	.help =		agresv_help,
};

static void
rgresv_help(void)
{
	dbprintf(_(
"\n"
" Print the size and per-rtgroup reservation information for some realtime allocation groups.\n"
"\n"
" Specific realtime allocation group numbers can be provided as command line\n"
" arguments.  If no arguments are provided, all allocation groups are iterated.\n"
"\n"
));

}

static void
print_rgresv_info(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_trans	*tp;
	xfs_filblks_t		ask = 0;
	xfs_filblks_t		used = 0;
	int			error;

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error) {
		dbprintf(
 _("Cannot alloc transaction to look up rtgroup %u rmap inode\n"),
				rtg_rgno(rtg));
		return;
	}

	error = -libxfs_rtginode_load_parent(tp);
	if (error) {
		dbprintf(_("Cannot load realtime metadir, error %d\n"),
			error);
		goto out_trans;
	}

	/* rtrmapbt */
	error = -libxfs_rtginode_load(rtg, XFS_RTGI_RMAP, tp);
	if (error) {
		dbprintf(_("Cannot load rtgroup %u rmap inode, error %d\n"),
			rtg_rgno(rtg), error);
		goto out_rele_dp;
	}
	if (rtg_rmap(rtg))
		used += rtg_rmap(rtg)->i_nblocks;
	libxfs_rtginode_irele(&rtg->rtg_inodes[XFS_RTGI_RMAP]);

	ask += libxfs_rtrmapbt_calc_reserves(mp);

	printf(_("rtg %d: dblocks: %llu fdblocks: %llu reserved: %llu used: %llu"),
			rtg_rgno(rtg),
			(unsigned long long)mp->m_sb.sb_dblocks,
			(unsigned long long)mp->m_sb.sb_fdblocks,
			(unsigned long long)ask,
			(unsigned long long)used);
	if (ask - used > mp->m_sb.sb_fdblocks)
		printf(_(" <not enough space>"));
	printf("\n");
out_rele_dp:
	libxfs_rtginode_irele(&mp->m_rtdirip);
out_trans:
	libxfs_trans_cancel(tp);
}

static int
rgresv_f(
	int			argc,
	char			**argv)
{
	struct xfs_rtgroup	*rtg = NULL;
	int			i;

	if (argc > 1) {
		for (i = 1; i < argc; i++) {
			long	a;
			char	*p;

			errno = 0;
			a = strtol(argv[i], &p, 0);
			if (p == argv[i])
				errno = ERANGE;
			if (errno) {
				perror(argv[i]);
				continue;
			}

			if (a < 0 || a >= mp->m_sb.sb_rgcount) {
				fprintf(stderr, "%ld: Not a rtgroup.\n", a);
				continue;
			}

			rtg = libxfs_rtgroup_get(mp, a);
			print_rgresv_info(rtg);
			libxfs_rtgroup_put(rtg);
		}
		return 0;
	}

	while ((rtg = xfs_rtgroup_next(mp, rtg)))
		print_rgresv_info(rtg);

	return 0;
}

static const struct cmdinfo rgresv_cmd = {
	.name =		"rgresv",
	.altname =	NULL,
	.cfunc =	rgresv_f,
	.argmin =	0,
	.argmax =	-1,
	.canpush =	0,
	.args =		NULL,
	.oneline =	N_("print rtgroup reservation stats"),
	.help =		rgresv_help,
};

void
info_init(void)
{
	add_command(&info_cmd);
	add_command(&agresv_cmd);
	add_command(&rgresv_cmd);
}
