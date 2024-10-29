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
" -c   -- Commit to the exchange only if file2 has not changed.\n"
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
	struct stat		stat;
	struct timeval		t1, t2;
	bool			use_commit = false;
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
		case 'c':
			use_commit = true;
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

	if (use_commit) {
		struct xfs_commit_range	xcr;

		ret = xfrog_commitrange_prep(&xcr, file->fd, dest_offset, fd,
				src_offset, length);
		if (!ret) {
			gettimeofday(&t1, NULL);
			ret = xfrog_commitrange(file->fd, &xcr, flags);
		}
	} else {
		struct xfs_exchange_range	fxr;

		xfrog_exchangerange_prep(&fxr, dest_offset, fd, src_offset,
				length);
		ret = xfrog_exchangerange(file->fd, &fxr, flags);
	}
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

/* Atomic file updates commands */

struct update_info {
	/* File that we're updating. */
	int			fd;

	/* ioctl data to commit the changes */
	struct xfs_commit_range	xcr;

	/* Name of the file we're updating. */
	char			*old_fname;

	/* fd we're using to stage the updates. */
	int			temp_fd;
};

enum finish_how	{
	FINISH_ABORT,
	FINISH_COMMIT,
	FINISH_CHECK
};

static struct update_info *updates;
static unsigned int nr_updates;

static void
startupdate_help(void)
{
	printf(_(
"\n"
" Prepare for an atomic file update, if supported by the filesystem.\n"
" A temporary file will be opened for writing and inserted into the file\n"
" table.  The current file will be changed to this temporary file.  Neither\n"
" file can be closed for the duration of the update.\n"
"\n"
" -e   -- Start with an empty file\n"
"\n"));
}

static int
startupdate_f(
	int			argc,
	char			*argv[])
{
	struct fsxattr		attr;
	struct xfs_fsop_geom	fsgeom;
	struct fs_path		fspath;
	struct stat		stat;
	struct update_info	*p;
	char			*fname;
	char			*path = NULL, *d;
	size_t			fname_len;
	int			flags = IO_TMPFILE | IO_ATOMICUPDATE;
	int			temp_fd = -1;
	bool			clone_file = true;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "e")) != -1) {
		switch (c) {
		case 'e':
			clone_file = false;
			break;
		default:
			startupdate_help();
			return 0;
		}
	}
	if (optind != argc) {
		startupdate_help();
		return 0;
	}

	/* Allocate a new slot. */
	p = realloc(updates, (++nr_updates) * sizeof(*p));
	if (!p) {
		perror("startupdate realloc");
		goto fail;
	}
	updates = p;

	/* Fill out the update information so that we can commit later. */
	p = &updates[nr_updates - 1];
	memset(p, 0, sizeof(*p));

	ret = fstat(file->fd, &stat);
	if (ret) {
		perror(file->name);
		goto fail;
	}

	/* Is the current file realtime?  If so, the temp file must match. */
	ret = ioctl(file->fd, FS_IOC_FSGETXATTR, &attr);
	if (ret == 0 && attr.fsx_xflags & FS_XFLAG_REALTIME)
		flags |= IO_REALTIME;

	/* Compute path to the directory that the current file is in. */
	path = strdup(file->name);
	d = strrchr(path, '/');
	if (!d) {
		fprintf(stderr, _("%s: cannot compute dirname?"), path);
		goto fail;
	}
	*d = 0;

	/* Open a temporary file to stage the new contents. */
	temp_fd = openfile(path, &fsgeom, flags, 0600, &fspath);
	if (temp_fd < 0) {
		perror(path);
		goto fail;
	}

	/*
	 * Snapshot the original file metadata in anticipation of the later
	 * file mapping exchange request.
	 */
	ret = xfrog_commitrange_prep(&p->xcr, file->fd, 0, temp_fd, 0,
			stat.st_size);
	if (ret) {
		perror("update prep");
		goto fail;
	}

	/* Clone all the data from the original file into the temporary file. */
	if (clone_file) {
		ret = ioctl(temp_fd, XFS_IOC_CLONE, file->fd);
		if (ret) {
			perror(path);
			goto fail;
		}
	}

	/* Prepare a new path string for the duration of the update. */
#define FILEUPDATE_STR	" (fileupdate)"
	fname_len = strlen(file->name) + strlen(FILEUPDATE_STR);
	fname = malloc(fname_len + 1);
	if (!fname) {
		perror("new path");
		goto fail;
	}
	snprintf(fname, fname_len + 1, "%s%s", file->name, FILEUPDATE_STR);

	/*
	 * Install the temporary file into the same slot of the file table as
	 * the original file.  Ensure that the original file cannot be closed.
	 */
	file->flags |= IO_ATOMICUPDATE;
	p->old_fname = file->name;
	file->name = fname;
	p->fd = file->fd;
	p->temp_fd = file->fd = temp_fd;

	free(path);
	return 0;
fail:
	if (temp_fd >= 0)
		close(temp_fd);
	free(path);
	nr_updates--;
	exitcode = 1;
	return 1;
}

static long long
finish_update(
	enum finish_how		how,
	uint64_t		flags,
	long long		*offset)
{
	struct update_info	*p;
	long long		committed_bytes = 0;
	size_t			length;
	unsigned int		i;
	unsigned int		upd_offset;
	int			temp_fd;
	int			ret;

	/* Find our update descriptor. */
	for (i = 0, p = updates; i < nr_updates; i++, p++) {
		if (p->temp_fd == file->fd)
			break;
	}

	if (i == nr_updates) {
		fprintf(stderr,
	_("Current file is not the staging file for an atomic update.\n"));
		exitcode = 1;
		return -1;
	}

	/*
	 * Commit our changes, if desired.  If the mapping exchange fails, we
	 * stop processing immediately so that we can run more xfs_io commands.
	 */
	switch (how) {
	case FINISH_CHECK:
		flags |= XFS_EXCHANGE_RANGE_DRY_RUN;
		fallthrough;
	case FINISH_COMMIT:
		ret = xfrog_commitrange(p->fd, &p->xcr, flags);
		if (ret) {
			xfrog_perror(ret, _("committing update"));
			exitcode = 1;
			return -1;
		}
		printf(_("Committed updates to '%s'.\n"), p->old_fname);
		*offset = p->xcr.file2_offset;
		committed_bytes = p->xcr.length;
		break;
	case FINISH_ABORT:
		printf(_("Cancelled updates to '%s'.\n"), p->old_fname);
		break;
	}

	/*
	 * Reset the filetable to point to the original file, and close the
	 * temporary file.
	 */
	free(file->name);
	file->name = p->old_fname;
	file->flags &= ~IO_ATOMICUPDATE;
	temp_fd = file->fd;
	file->fd = p->fd;
	ret = close(temp_fd);
	if (ret)
		perror(_("closing temporary file"));

	/* Remove the atomic update context, shifting things down. */
	upd_offset = p - updates;
	length = nr_updates * sizeof(struct update_info);
	length -= (upd_offset + 1) * sizeof(struct update_info);
	if (length)
		memmove(p, p + 1, length);

	nr_updates--;
	return committed_bytes;
}

static void
cancelupdate_help(void)
{
	printf(_(
"\n"
" Cancels an atomic file update.  The temporary file will be closed, and the\n"
" current file set back to the original file.\n"
"\n"));
}

static int
cancelupdate_f(
	int		argc,
	char		*argv[])
{
	return finish_update(FINISH_ABORT, 0, NULL);
}

static void
commitupdate_help(void)
{
	printf(_(
"\n"
" Commits an atomic file update.  File contents written to the temporary file\n"
" will be exchanged atomically with the corresponding range in the original\n"
" file.  The temporary file will be closed, and the current file set back to\n"
" the original file.\n"
"\n"
" -C   -- Print timing information in a condensed format.\n"
" -h   -- Only exchange written ranges in the temporary file.\n"
" -k   -- Exchange to end of file, ignore any length previously set.\n"
" -n   -- Check parameters but do not change anything.\n"
" -q   -- Do not print timing information at all.\n"));
}

static int
commitupdate_f(
	int		argc,
	char		*argv[])
{
	struct timeval	t1, t2;
	enum finish_how	how = FINISH_COMMIT;
	uint64_t	flags = XFS_EXCHANGE_RANGE_TO_EOF;
	long long	offset, len;
	int		condensed = 0, quiet_flag = 0;
	int		c;

	while ((c = getopt(argc, argv, "Chknq")) != -1) {
		switch (c) {
		case 'C':
			condensed = 1;
			break;
		case 'h':
			flags |= XFS_EXCHANGE_RANGE_FILE1_WRITTEN;
			break;
		case 'k':
			flags &= ~XFS_EXCHANGE_RANGE_TO_EOF;
			break;
		case 'n':
			how = FINISH_CHECK;
			break;
		case 'q':
			quiet_flag = 1;
			break;
		default:
			commitupdate_help();
			return 0;
		}
	}
	if (optind != argc) {
		commitupdate_help();
		return 0;
	}

	gettimeofday(&t1, NULL);
	len = finish_update(how, flags, &offset);
	if (len < 0)
		return 1;
	if (quiet_flag)
		return 0;

	gettimeofday(&t2, NULL);
	t2 = tsub(t2, t1);
	report_io_times("commitupdate", &t2, offset, len, len, 1, condensed);
	return 0;
}

static struct cmdinfo startupdate_cmd = {
	.name		= "startupdate",
	.cfunc		= startupdate_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= startupdate_help,
};

static struct cmdinfo cancelupdate_cmd = {
	.name		= "cancelupdate",
	.cfunc		= cancelupdate_f,
	.argmin		= 0,
	.argmax		= 0,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= cancelupdate_help,
};

static struct cmdinfo commitupdate_cmd = {
	.name		= "commitupdate",
	.cfunc		= commitupdate_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_FLAG_ONESHOT | CMD_NOMAP_OK,
	.help		= commitupdate_help,
};

void
exchangerange_init(void)
{
	exchangerange_cmd.args = _("[-Ccfntw] [-d dest_offset] [-s src_offset] [-l length] <donorfile>");
	exchangerange_cmd.oneline = _("Exchange contents between files.");

	add_command(&exchangerange_cmd);

	startupdate_cmd.oneline = _("start an atomic update of a file");
	startupdate_cmd.args = _("[-e]");

	cancelupdate_cmd.oneline = _("cancel an atomic update");

	commitupdate_cmd.oneline = _("commit a file update atomically");
	commitupdate_cmd.args = _("[-C] [-h] [-n] [-q]");

	add_command(&startupdate_cmd);
	add_command(&cancelupdate_cmd);
	add_command(&commitupdate_cmd);
}
