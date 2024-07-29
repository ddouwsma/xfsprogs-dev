// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/file_exchange.h"
#include "libfrog/bulkstat.h"

static void
exchangerange_help(void)
{
	printf(_(
"\n"
" Exchange file data between the open file descriptor and the supplied filename.\n"
" -C   -- Print timing information in a condensed format\n"
" -d N -- Start exchanging contents at this position in the open file\n"
" -f   -- Flush changed file data and metadata to disk\n"
" -l N -- Exchange this many bytes between the two files instead of to EOF\n"
" -n   -- Dry run; do all the parameter validation but do not change anything.\n"
" -s N -- Start exchanging contents at this position in the supplied file\n"
" -t   -- Print timing information\n"
" -w   -- Only exchange written ranges in the supplied file\n"
));
}

static int
exchangerange_f(
	int			argc,
	char			**argv)
{
	struct xfs_exchange_range	fxr;
	struct stat		stat;
	struct timeval		t1, t2;
	uint64_t		flags = XFS_EXCHANGE_RANGE_TO_EOF;
	int64_t			src_offset = 0;
	int64_t			dest_offset = 0;
	int64_t			length = -1;
	size_t			fsblocksize, fssectsize;
	int			condensed = 0, quiet_flag = 1;
	int			c;
	int			fd;
	int			ret;

	init_cvtnum(&fsblocksize, &fssectsize);
	while ((c = getopt(argc, argv, "Ccd:fl:ns:tw")) != -1) {
		switch (c) {
		case 'C':
			condensed = 1;
			break;
		case 'd':
			dest_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (dest_offset < 0) {
				printf(
			_("non-numeric open file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			break;
		case 'f':
			flags |= XFS_EXCHANGE_RANGE_DSYNC;
			break;
		case 'l':
			length = cvtnum(fsblocksize, fssectsize, optarg);
			if (length < 0) {
				printf(
			_("non-numeric length argument -- %s\n"),
						optarg);
				return 0;
			}
			flags &= ~XFS_EXCHANGE_RANGE_TO_EOF;
			break;
		case 'n':
			flags |= XFS_EXCHANGE_RANGE_DRY_RUN;
			break;
		case 's':
			src_offset = cvtnum(fsblocksize, fssectsize, optarg);
			if (src_offset < 0) {
				printf(
			_("non-numeric supplied file offset argument -- %s\n"),
						optarg);
				return 0;
			}
			break;
		case 't':
			quiet_flag = 0;
			break;
		case 'w':
			flags |= XFS_EXCHANGE_RANGE_FILE1_WRITTEN;
			break;
		default:
			exchangerange_help();
			return 0;
		}
	}
	if (optind != argc - 1) {
		exchangerange_help();
		return 0;
	}

	/* open the donor file */
	fd = openfile(argv[optind], NULL, 0, 0, NULL);
	if (fd < 0)
		return 0;

	ret = fstat(file->fd, &stat);
	if (ret) {
		perror("fstat");
		exitcode = 1;
		goto out;
	}
	if (length < 0)
		length = stat.st_size;

	xfrog_exchangerange_prep(&fxr, dest_offset, fd, src_offset, length);
	ret = xfrog_exchangerange(file->fd, &fxr, flags);
	if (ret) {
		xfrog_perror(ret, "exchangerange");
		exitcode = 1;
		goto out;
	}
	if (quiet_flag)
		goto out;

	gettimeofday(&t2, NULL);
	t2 = tsub(t2, t1);

	report_io_times("exchangerange", &t2, dest_offset, length, length, 1,
			condensed);
out:
	close(fd);
	return 0;
}

static struct cmdinfo exchangerange_cmd = {
	.name		= "exchangerange",
	.cfunc		= exchangerange_f,
	.argmin		= 1,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= exchangerange_help,
};

void
exchangerange_init(void)
{
	exchangerange_cmd.args = _("[-Cfntw] [-d dest_offset] [-s src_offset] [-l length] <donorfile>");
	exchangerange_cmd.oneline = _("Exchange contents between files.");

	add_command(&exchangerange_cmd);
}
