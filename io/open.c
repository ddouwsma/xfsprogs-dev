// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include "libxfs.h"
#include "libfrog/logging.h"
#include "libfrog/fsgeom.h"
#include "libfrog/bulkstat.h"

#ifndef __O_TMPFILE
#if defined __alpha__
#define __O_TMPFILE	0100000000
#elif defined(__hppa__)
#define __O_TMPFILE	 040000000
#elif defined(__sparc__)
#define __O_TMPFILE	 0x2000000
#else
#define __O_TMPFILE	 020000000
#endif
#endif /* __O_TMPFILE */

#ifndef O_TMPFILE
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

#ifndef O_PATH
#if defined __alpha__
#define O_PATH          040000000
#elif defined(__hppa__)
#define O_PATH          020000000
#elif defined(__sparc__)
#define O_PATH          0x1000000
#else
#define O_PATH          010000000
#endif
#endif /* O_PATH */

static cmdinfo_t open_cmd;
static cmdinfo_t close_cmd;
static cmdinfo_t chproj_cmd;
static cmdinfo_t lsproj_cmd;
static cmdinfo_t extsize_cmd;
static cmdinfo_t inode_cmd;
static cmdinfo_t chmod_cmd;
static prid_t prid;
static long extsize;

int
openfile(
	char		*path,
	struct xfs_fsop_geom *geom,
	int		flags,
	mode_t		mode,
	struct fs_path	*fs_path)
{
	struct fs_path	*fsp;
	struct stat	st = { 0 };
	int		fd;
	int		oflags;

	oflags = flags & IO_READONLY ? O_RDONLY : O_RDWR;
	if (flags & IO_APPEND)
		oflags |= O_APPEND;
	if (flags & IO_CREAT)
		oflags |= O_CREAT;
	if (flags & IO_DIRECT)
		oflags |= O_DIRECT;
	if (flags & IO_OSYNC)
		oflags |= O_SYNC;
	if (flags & IO_TRUNC)
		oflags |= O_TRUNC;
	if (flags & IO_NONBLOCK)
		oflags |= O_NONBLOCK;
	if (flags & IO_TMPFILE)
		oflags |= O_TMPFILE;
	if (flags & IO_PATH)
		oflags |= O_PATH;
	if (flags & IO_NOFOLLOW)
		oflags |= O_NOFOLLOW;

	/*
	 * if we've been passed a pipe to open, don't block waiting for a
	 * reader or writer to appear. We want to either succeed or error out
	 * immediately.
	 */
	if (stat(path, &st) < 0 && errno != ENOENT) {
		perror("stat");
		return -1;
	}
	if (S_ISFIFO(st.st_mode))
		oflags |= O_NONBLOCK;

	fd = open(path, oflags, mode);
	if (fd < 0) {
		if (errno == EISDIR &&
		    ((oflags & (O_RDWR|O_TMPFILE)) == O_RDWR)) {
			/* make it as if we asked for O_RDONLY & try again */
			oflags &= ~O_RDWR;
			oflags |= O_RDONLY;
			flags |= IO_READONLY;
			fd = open(path, oflags, mode);
			if (fd < 0) {
				perror(path);
				return -1;
			}
		} else {
			perror(path);
			return -1;
		}
	}

	if (!geom || !platform_test_xfs_fd(fd))
		goto set_fspath;

	if (flags & IO_PATH) {
		/* Can't call ioctl() on O_PATH fds */
		memset(geom, 0, sizeof(*geom));
	} else {
		int	ret;

		ret = -xfrog_geometry(fd, geom);
		if (ret) {
			xfrog_perror(ret, "XFS_IOC_FSGEOMETRY");
			close(fd);
			return -1;
		}
	}

	if (!(flags & (IO_READONLY | IO_PATH)) && (flags & IO_REALTIME)) {
		struct fsxattr	attr;

		if (xfsctl(path, fd, FS_IOC_FSGETXATTR, &attr) < 0) {
			perror("FS_IOC_FSGETXATTR");
			close(fd);
			return -1;
		}
		if (!(attr.fsx_xflags & FS_XFLAG_REALTIME)) {
			attr.fsx_xflags |= FS_XFLAG_REALTIME;
			if (xfsctl(path, fd, FS_IOC_FSSETXATTR, &attr) < 0) {
				perror("FS_IOC_FSSETXATTR");
				close(fd);
				return -1;
			}
		}
	}

set_fspath:
	if (fs_path) {
		fsp = fs_table_lookup(path, FS_MOUNT_POINT);
		if (!fsp)
			memset(fs_path, 0, sizeof(*fs_path));
		else
			*fs_path = *fsp;
	}
	return fd;
}

int
addfile(
	char		*name,
	int		fd,
	struct xfs_fsop_geom *geometry,
	int		flags,
	struct fs_path	*fs_path)
{
	char		*filename;

	filename = strdup(name);
	if (!filename) {
		perror("strdup");
		close(fd);
		return -1;
	}

	/* Extend the table of currently open files */
	filetable = (fileio_t *)realloc(filetable,	/* growing */
					++filecount * sizeof(fileio_t));
	if (!filetable) {
		perror("realloc");
		filecount = 0;
		free(filename);
		close(fd);
		return -1;
	}

	/* Finally, make this the new active open file */
	file = &filetable[filecount - 1];
	file->fd = fd;
	file->flags = flags;
	file->name = filename;
	file->geom = *geometry;
	file->fs_path = *fs_path;
	return 0;
}

static void
open_help(void)
{
	printf(_(
"\n"
" opens a new file in the requested mode\n"
"\n"
" Example:\n"
" 'open -cd /tmp/data' - creates/opens data file read-write for direct IO\n"
"\n"
" Opens a file for subsequent use by all of the other xfs_io commands.\n"
" With no arguments, open uses the stat command to show the current file.\n"
" -a -- open with the O_APPEND flag (append-only mode)\n"
" -d -- open with O_DIRECT (non-buffered IO, note alignment constraints)\n"
" -f -- open with O_CREAT (create the file if it doesn't exist)\n"
" -m -- permissions to use in case a new file is created (default 0600)\n"
" -n -- open with O_NONBLOCK\n"
" -r -- open with O_RDONLY, the default is O_RDWR\n"
" -s -- open with O_SYNC\n"
" -t -- open with O_TRUNC (truncate the file to zero length if it exists)\n"
" -R -- mark the file as a realtime XFS file immediately after opening it\n"
" -T -- open with O_TMPFILE (create a file not visible in the namespace)\n"
" -P -- open with O_PATH (create an fd that is merely a location reference)\n"
" -L -- open with O_NOFOLLOW (don't follow symlink)\n"
" Note1: usually read/write direct IO requests must be blocksize aligned;\n"
"        some kernels, however, allow sectorsize alignment for direct IO.\n"
" Note2: the bmap for non-regular files can be obtained provided the file\n"
"        was opened correctly (in particular, must be opened read-only).\n"
"\n"));
}

static int
open_f(
	int		argc,
	char		**argv)
{
	int		c, fd, flags = 0;
	char		*sp;
	mode_t		mode = 0600;
	struct xfs_fsop_geom geometry = { 0 };
	struct fs_path	fsp;

	if (argc == 1) {
		if (file)
			return stat_f(argc, argv);
		fprintf(stderr, _("no files are open, try 'help open'\n"));
		exitcode = 1;
		return 0;
	}

	while ((c = getopt(argc, argv, "FLPRTacdfm:nrstx")) != EOF) {
		switch (c) {
		case 'F':
			/* Ignored / deprecated now, handled automatically */
			break;
		case 'a':
			flags |= IO_APPEND;
			break;
		case 'c':
		case 'f':
			flags |= IO_CREAT;
			break;
		case 'd':
			flags |= IO_DIRECT;
			break;
		case 'm':
			mode = strtoul(optarg, &sp, 0);
			if (!sp || sp == optarg) {
				printf(_("non-numeric mode -- %s\n"), optarg);
				exitcode = 1;
				return 0;
			}
			break;
		case 'n':
			flags |= IO_NONBLOCK;
			break;
		case 'r':
			flags |= IO_READONLY;
			break;
		case 's':
			flags |= IO_OSYNC;
			break;
		case 't':
			flags |= IO_TRUNC;
			break;
		case 'R':
		case 'x':	/* backwards compatibility */
			flags |= IO_REALTIME;
			break;
		case 'T':
			flags |= IO_TMPFILE;
			break;
		case 'P':
			flags |= IO_PATH;
			break;
		case 'L':
			flags |= IO_NOFOLLOW;
			break;
		default:
			exitcode = 1;
			return command_usage(&open_cmd);
		}
	}

	if (optind != argc - 1) {
		exitcode = 1;
		return command_usage(&open_cmd);
	}

	if ((flags & (IO_READONLY|IO_TMPFILE)) == (IO_READONLY|IO_TMPFILE)) {
		fprintf(stderr, _("-T and -r options are incompatible\n"));
		exitcode = 1;
		return -1;
	}

	if ((flags & (IO_PATH|IO_NOFOLLOW)) &&
	    (flags & ~(IO_PATH|IO_NOFOLLOW))) {
		fprintf(stderr, _("-P and -L are incompatible with the other options\n"));
		exitcode = 1;
		return -1;
	}

	fd = openfile(argv[optind], &geometry, flags, mode, &fsp);
	if (fd < 0) {
		exitcode = 1;
		return 0;
	}

	if (!platform_test_xfs_fd(fd))
		flags |= IO_FOREIGN;

	if (addfile(argv[optind], fd, &geometry, flags, &fsp) != 0) {
		exitcode = 1;
		return 0;
	}

	return 0;
}

int
closefile(void)
{
	size_t		length;
	unsigned int	offset;

	if (file->flags & IO_ATOMICUPDATE) {
		fprintf(stderr,
	_("%s: atomic update in progress, cannot close.\n"),
			file->name);
		exitcode = 1;
		return 0;
	}
	if (close(file->fd) < 0) {
		perror("close");
		exitcode = 1;
		return 0;
	}
	free(file->name);

	/* Shuffle the file table entries down over the removed entry */
	offset = file - &filetable[0];
	length = filecount * sizeof(fileio_t);
	length -= (offset + 1) * sizeof(fileio_t);
	if (length)
		memmove(file, file + 1, length);

	/* Resize the memory allocated for the table, possibly freeing */
	if (--filecount) {
		filetable = (fileio_t *)realloc(filetable,	/* shrinking */
					filecount * sizeof(fileio_t));
		if (offset == filecount)
			offset--;
		file = filetable + offset;
	} else {
		free(filetable);
		file = filetable = NULL;
	}
	return 0;
}

static int
close_f(
	int		argc,
	char		**argv)
{
	int		ret;

	ret = closefile();
	if (!ret)
		filelist_f();
	return 0;
}

static void
lsproj_help(void)
{
	printf(_(
"\n"
" displays the project identifier associated with the current path\n"
"\n"
" Options:\n"
" -R -- recursively descend (useful when current path is a directory)\n"
" -D -- recursively descend, but only list projects on directories\n"
"\n"));
}

static int
lsproj_callback(
	const char		*path,
	const struct stat	*stat,
	int			status,
	struct FTW		*data)
{
	prid_t			projid;
	int			fd;

	if (recurse_dir && !S_ISDIR(stat->st_mode))
		return 0;

	if ((fd = open(path, O_RDONLY)) == -1) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, path, strerror(errno));
		exitcode = 1;
	} else {
		if (getprojid(path, fd, &projid) == 0)
			printf("[%u] %s\n", (unsigned int)projid, path);
		else
			exitcode = 1;
		close(fd);
	}
	return 0;
}

static int
lsproj_f(
	int		argc,
	char		**argv)
{
	prid_t		projid;
	int		c;

	recurse_all = recurse_dir = 0;
	while ((c = getopt(argc, argv, "DR")) != EOF) {
		switch (c) {
		case 'D':
			recurse_dir = 1;
			break;
		case 'R':
			recurse_all = 1;
			break;
		default:
			exitcode = 1;
			return command_usage(&lsproj_cmd);
		}
	}

	if (argc != optind) {
		exitcode = 1;
		return command_usage(&lsproj_cmd);
	}

	if (recurse_all || recurse_dir)
		nftw(file->name, lsproj_callback,
			100, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
	else if (getprojid(file->name, file->fd, &projid) < 0) {
		perror("getprojid");
		exitcode = 1;
	} else
		printf(_("projid = %u\n"), (unsigned int)projid);
	return 0;
}

static void
chproj_help(void)
{
	printf(_(
"\n"
" modifies the project identifier associated with the current path\n"
"\n"
" -R -- recursively descend (useful when current path is a directory)\n"
" -D -- recursively descend, only modifying projects on directories\n"
"\n"));
}

static int
chproj_callback(
	const char		*path,
	const struct stat	*stat,
	int			status,
	struct FTW		*data)
{
	int			fd;

	if (recurse_dir && !S_ISDIR(stat->st_mode))
		return 0;

	if ((fd = open(path, O_RDONLY)) == -1) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, path, strerror(errno));
		exitcode = 1;
	} else {
		if (setprojid(path, fd, prid) < 0) {
			perror("setprojid");
			exitcode = 1;
		}
		close(fd);
	}
	return 0;
}

static int
chproj_f(
	int		argc,
	char		**argv)
{
	int		c;

	recurse_all = recurse_dir = 0;
	while ((c = getopt(argc, argv, "DR")) != EOF) {
		switch (c) {
		case 'D':
			recurse_dir = 1;
			break;
		case 'R':
			recurse_all = 1;
			break;
		default:
			exitcode = 1;
			return command_usage(&chproj_cmd);
		}
	}

	if (argc != optind + 1) {
		exitcode = 1;
		return command_usage(&chproj_cmd);
	}

	prid = prid_from_string(argv[optind]);
	if (prid == -1) {
		printf(_("invalid project ID -- %s\n"), argv[optind]);
		exitcode = 1;
		return 0;
	}

	if (recurse_all && recurse_dir) {
		fprintf(stderr, _("%s: -R and -D options are mutually exclusive\n"),
			progname);
		exitcode = 1;
		return 0;
	}

	if (recurse_all || recurse_dir)
		nftw(file->name, chproj_callback,
			100, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
	else if (setprojid(file->name, file->fd, prid) < 0) {
		perror("setprojid");
		exitcode = 1;
	}
	return 0;
}

static void
extsize_help(void)
{
	printf(_(
"\n"
" report or modify preferred extent size (in bytes) for the current path\n"
"\n"
" -R -- recursively descend (useful when current path is a directory)\n"
" -D -- recursively descend, only modifying extsize on directories\n"
"\n"));
}

static int
get_extsize(const char *path, int fd)
{
	struct fsxattr	fsx;

	if ((xfsctl(path, fd, FS_IOC_FSGETXATTR, &fsx)) < 0) {
		printf("%s: FS_IOC_FSGETXATTR %s: %s\n",
			progname, path, strerror(errno));
		return -1;
	}
	printf("[%u] %s\n", fsx.fsx_extsize, path);
	return 0;
}

static int
set_extsize(const char *path, int fd, long extsz)
{
	struct fsxattr	fsx;
	struct stat	stat;

	if (fstat(fd, &stat) < 0) {
		perror("fstat");
		return -1;
	}
	if ((xfsctl(path, fd, FS_IOC_FSGETXATTR, &fsx)) < 0) {
		printf("%s: FS_IOC_FSGETXATTR %s: %s\n",
			progname, path, strerror(errno));
		return -1;
	}

	if (S_ISREG(stat.st_mode)) {
		fsx.fsx_xflags |= FS_XFLAG_EXTSIZE;
	} else if (S_ISDIR(stat.st_mode)) {
		fsx.fsx_xflags |= FS_XFLAG_EXTSZINHERIT;
	} else {
		printf(_("invalid target file type - file %s\n"), path);
		return -1;
	}
	fsx.fsx_extsize = extsz;

	if ((xfsctl(path, fd, FS_IOC_FSSETXATTR, &fsx)) < 0) {
		printf("%s: FS_IOC_FSSETXATTR %s: %s\n",
			progname, path, strerror(errno));
		return -1;
	}

	return 0;
}

static int
get_extsize_callback(
	const char		*path,
	const struct stat	*stat,
	int			status,
	struct FTW		*data)
{
	int			fd;

	if (recurse_dir && !S_ISDIR(stat->st_mode))
		return 0;

	if ((fd = open(path, O_RDONLY)) == -1) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, path, strerror(errno));
		exitcode = 1;
	} else {
		if (get_extsize(path, fd) < 0)
			exitcode = 1;
		close(fd);
	}
	return 0;
}

static int
set_extsize_callback(
	const char		*path,
	const struct stat	*stat,
	int			status,
	struct FTW		*data)
{
	int			fd;

	if (recurse_dir && !S_ISDIR(stat->st_mode))
		return 0;

	if ((fd = open(path, O_RDONLY)) == -1) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, path, strerror(errno));
		exitcode = 1;
	} else {
		if (set_extsize(path, fd, extsize) < 0)
			exitcode = 1;
		close(fd);
	}
	return 0;
}

static int
extsize_f(
	int		argc,
	char		**argv)
{
	size_t			blocksize, sectsize;
	int			c;

	recurse_all = recurse_dir = 0;
	init_cvtnum(&blocksize, &sectsize);
	while ((c = getopt(argc, argv, "DR")) != EOF) {
		switch (c) {
		case 'D':
			recurse_dir = 1;
			break;
		case 'R':
			recurse_all = 1;
			break;
		default:
			exitcode = 1;
			return command_usage(&extsize_cmd);
		}
	}

	if (optind < argc) {
		extsize = (long)cvtnum(blocksize, sectsize, argv[optind]);
		if (extsize < 0) {
			printf(_("non-numeric extsize argument -- %s\n"),
				argv[optind]);
			exitcode = 1;
			return 0;
		}
	} else {
		extsize = -1;
	}

	if (recurse_all && recurse_dir) {
		fprintf(stderr, _("%s: -R and -D options are mutually exclusive\n"),
			progname);
		exitcode = 1;
		return 0;
	}

	if (recurse_all || recurse_dir) {
		nftw(file->name, (extsize >= 0) ?
			set_extsize_callback : get_extsize_callback,
			100, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
	} else if (extsize >= 0) {
		if (set_extsize(file->name, file->fd, extsize) < 0)
			exitcode = 1;
	} else if (get_extsize(file->name, file->fd) < 0) {
		exitcode = 1;
	}
	return 0;
}

static void
inode_help(void)
{
	printf(_(
"\n"
"Query physical information about an inode"
"\n"
" Default:	-- Return 1 if any inode number greater than 32 bits exists in\n"
"		   the filesystem, or 0 if none exist\n"
" num		-- Return inode number [num] if in use, or 0 if not in use\n"
" -n num	-- Return the next used inode after [num]\n"
" -v		-- Verbose mode - display returned inode number's size in bits\n"
"\n"));
}

#define IGROUP_NR	(1024)
static __u64
get_last_inode(void)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct xfs_inumbers_req	*ireq;
	uint32_t		lastgrp = 0;
	__u64			last_ino = 0;
	int			ret;

	ret = -xfrog_inumbers_alloc_req(IGROUP_NR, 0, &ireq);
	if (ret) {
		xfrog_perror(ret, "alloc req");
		exitcode = 1;
		return 0;
	}

	for (;;) {
		ret = -xfrog_inumbers(&xfd, ireq);
		if (ret) {
			xfrog_perror(ret, "XFS_IOC_FSINUMBERS");
			exitcode = 1;
			goto out;
		}

		/* Did we reach the last inode? */
		if (ireq->hdr.ocount == 0)
			break;

		/* last inode in igroup table */
		lastgrp = ireq->hdr.ocount;
	}

	if (lastgrp == 0)
		goto out;

	lastgrp--;

	/* The last inode number in use */
	last_ino = ireq->inumbers[lastgrp].xi_startino +
		  libxfs_highbit64(ireq->inumbers[lastgrp].xi_allocmask);
out:
	free(ireq);

	return last_ino;
}

static int
inode_f(
	int			argc,
	char			**argv)
{
	struct xfs_bulkstat	bulkstat;
	uint64_t		result_ino = 0;
	uint64_t		userino = NULLFSINO;
	char			*p;
	int			c;
	int			verbose = 0;
	int			ret_next = 0;
	int			ret;

	while ((c = getopt(argc, argv, "nv")) != EOF) {
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		case 'n':
			ret_next = 1;
			break;
		default:
			exitcode = 1;
			return command_usage(&inode_cmd);
		}
	}

	/* Last arg (if present) should be an inode number */
	if (optind < argc) {
		userino = strtoull(argv[optind], &p, 10);
		if ((*p != '\0')) {
			printf(_("%s is not a numeric inode value\n"),
				argv[optind]);
			exitcode = 1;
			return 0;
		}
		optind++;
	}

	/* Extra junk? */
	if (optind < argc) {
		exitcode = 1;
		return command_usage(&inode_cmd);
	}

	/* -n option requires an inode number */
	if (ret_next && userino == NULLFSINO) {
		exitcode = 1;
		return command_usage(&inode_cmd);
	}

	if (userino == NULLFSINO) {
		/* We are finding last inode in use */
		result_ino = get_last_inode();
		if (!result_ino) {
			exitcode = 1;
			return 0;
		}
	} else if (ret_next) {
		struct xfs_fd	xfd = XFS_FD_INIT(file->fd);
		struct xfs_bulkstat_req	*breq;

		/*
		 * The -n option means that the caller wants to know the number
		 * of the next allocated inode, so we need to increment here.
		 */
		ret = -xfrog_bulkstat_alloc_req(1, userino + 1, &breq);
		if (ret) {
			xfrog_perror(ret, "alloc bulkstat");
			exitcode = 1;
			return 0;
		}

		/* get next inode */
		ret = -xfrog_bulkstat(&xfd, breq);
		if (ret) {
			xfrog_perror(ret, "bulkstat");
			free(breq);
			exitcode = 1;
			return 0;
		}

		/* The next inode in use, or 0 if none */
		if (breq->hdr.ocount)
			result_ino = breq->bulkstat[0].bs_ino;
		else
			result_ino = 0;
		free(breq);
	} else {
		struct xfs_fd	xfd = XFS_FD_INIT(file->fd);

		/* get this inode */
		ret = -xfrog_bulkstat_single(&xfd, userino, 0, &bulkstat);
		if (ret == EINVAL) {
			/* Not in use */
			result_ino = 0;
		} else if (ret) {
			xfrog_perror(ret, "bulkstat_single");
			exitcode = 1;
			return 0;
		} else {
			result_ino = bulkstat.bs_ino;
		}
	}

	if (verbose && result_ino) {
		/* Requested verbose and we have an answer */
		printf("%llu:%d\n", (unsigned long long)result_ino,
			result_ino > XFS_MAXINUMBER_32 ? 64 : 32);
	} else if (userino == NULLFSINO) {
		/* Just checking 32 or 64 bit presence, non-verbose */
		printf("%d\n", result_ino > XFS_MAXINUMBER_32 ? 1 : 0);
	} else {
		/* We asked about a specific inode, non-verbose */
		printf("%llu\n", (unsigned long long)result_ino);
	}

	return 0;
}

static void
chmod_help(void)
{
	printf(_(
"\n"
" Change the read/write permissions on the current file\n"
"\n"
" Options:\n"
" -r -- make the file read only (0444 permissions)\n"
" -w -- make the file read/write (0664 permissions)\n"
"\n"));
}

static int
chmod_f(
	int		argc,
	char		**argv)
{
	mode_t		mode = S_IRUSR | S_IRGRP | S_IROTH;
	int		c;

	while ((c = getopt(argc, argv, "rw")) != EOF) {
		switch (c) {
		case 'r':
			break;
		case 'w':
			mode |= S_IWUSR | S_IWGRP;
			break;
		default:
			return command_usage(&chmod_cmd);
		}
	}

	if (argc != optind)
		return command_usage(&chmod_cmd);

	if (fchmod(file->fd, mode) < 0) {
		exitcode = 1;
		perror("fchmod");
	}
	return 0;
}
void
open_init(void)
{
	open_cmd.name = "open";
	open_cmd.altname = "o";
	open_cmd.cfunc = open_f;
	open_cmd.argmin = 0;
	open_cmd.argmax = -1;
	open_cmd.flags = CMD_NOMAP_OK | CMD_NOFILE_OK |
			 CMD_FOREIGN_OK | CMD_FLAG_ONESHOT;
	open_cmd.args = _("[-acdrstxRTPL] [-m mode] [path]");
	open_cmd.oneline = _("open the file specified by path");
	open_cmd.help = open_help;

	close_cmd.name = "close";
	close_cmd.altname = "c";
	close_cmd.cfunc = close_f;
	close_cmd.argmin = 0;
	close_cmd.argmax = 0;
	close_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK | CMD_FLAG_ONESHOT;
	close_cmd.oneline = _("close the current open file");

	chproj_cmd.name = "chproj";
	chproj_cmd.cfunc = chproj_f;
	chproj_cmd.args = _("[-D | -R] projid");
	chproj_cmd.argmin = 1;
	chproj_cmd.argmax = -1;
	chproj_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	chproj_cmd.oneline =
		_("change project identifier on the currently open file");
	chproj_cmd.help = chproj_help;

	lsproj_cmd.name = "lsproj";
	lsproj_cmd.cfunc = lsproj_f;
	lsproj_cmd.args = _("[-D | -R]");
	lsproj_cmd.argmin = 0;
	lsproj_cmd.argmax = 1;
	lsproj_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	lsproj_cmd.oneline =
		_("list project identifier set on the currently open file");
	lsproj_cmd.help = lsproj_help;

	extsize_cmd.name = "extsize";
	extsize_cmd.cfunc = extsize_f;
	extsize_cmd.args = _("[-D | -R] [extsize]");
	extsize_cmd.argmin = 0;
	extsize_cmd.argmax = -1;
	extsize_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK;
	extsize_cmd.oneline =
		_("get/set preferred extent size (in bytes) for the open file");
	extsize_cmd.help = extsize_help;

	inode_cmd.name = "inode";
	inode_cmd.cfunc = inode_f;
	inode_cmd.args = _("[-nv] [num]");
	inode_cmd.argmin = 0;
	inode_cmd.argmax = 3;
	inode_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_ONESHOT;
	inode_cmd.oneline =
		_("Query inode number usage in the filesystem");
	inode_cmd.help = inode_help;

	chmod_cmd.name = "chmod";
	chmod_cmd.cfunc = chmod_f;
	chmod_cmd.args = _("-r | -w");
	chmod_cmd.argmin = 1;
	chmod_cmd.argmax = 1;
	chmod_cmd.flags = CMD_NOMAP_OK | CMD_FOREIGN_OK | CMD_FLAG_ONESHOT;
	chmod_cmd.oneline =
		_("change the read/write permissions on the currently open file");
	chmod_cmd.help = chmod_help;

	add_command(&open_cmd);
	add_command(&close_cmd);
	add_command(&chproj_cmd);
	add_command(&lsproj_cmd);
	add_command(&extsize_cmd);
	add_command(&inode_cmd);
	add_command(&chmod_cmd);
}
