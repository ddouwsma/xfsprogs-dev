// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include "libfrog/paths.h"
#include "libfrog/getparents.h"
#include "handle.h"
#include "init.h"
#include "io.h"
#include "libfrog/handle_priv.h"

static cmdinfo_t parent_cmd;
static char *mntpt;

struct pptr_args {
	char		*pathbuf;
	char		*filter_name;
	uint64_t	filter_ino;
	bool		shortformat;
};

static int
pptr_print(
	const struct parent_rec	*rec,
	void			*arg)
{
	const struct xfs_fid	*fid = &rec->p_handle.ha_fid;
	struct pptr_args	*args = arg;

	if (rec->p_flags & PARENTREC_FILE_IS_ROOT) {
		printf(_("Root directory.\n"));
		return 0;
	}

	if (args->filter_ino && fid->fid_ino != args->filter_ino)
		return 0;
	if (args->filter_name && strcmp(args->filter_name, rec->p_name))
		return 0;

	if (args->shortformat) {
		printf("%llu:%u:%zu:%s\n",
				(unsigned long long)fid->fid_ino,
				(unsigned int)fid->fid_gen,
				strlen(rec->p_name),
				rec->p_name);
		return 0;
	}

	printf(_("p_ino     = %llu\n"), (unsigned long long)fid->fid_ino);
	printf(_("p_gen     = %u\n"), (unsigned int)fid->fid_gen);
	printf(_("p_namelen = %zu\n"), strlen(rec->p_name));
	printf(_("p_name    = \"%s\"\n\n"), rec->p_name);

	return 0;
}

static int
filter_path_components(
	const char		*name,
	uint64_t		ino,
	void			*arg)
{
	struct pptr_args	*args = arg;

	if (args->filter_ino && ino == args->filter_ino)
		return ECANCELED;
	if (args->filter_name && !strcmp(args->filter_name, name))
		return ECANCELED;
	return 0;
}

static int
paths_print(
	const char		*mntpt,
	const struct path_list	*path,
	void			*arg)
{
	struct pptr_args	*args = arg;
	char			*buf = args->pathbuf;
	size_t			len = MAXPATHLEN;
	int			mntpt_len = strlen(mntpt);
	int			ret;

	if (args->filter_ino || args->filter_name) {
		ret = path_walk_components(path, filter_path_components, args);
		if (ret != ECANCELED)
			return 0;
	}

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(buf, len, "%.*s", mntpt_len, mntpt);
	if (ret != mntpt_len)
		return ENAMETOOLONG;

	ret = path_list_to_string(path, buf + ret, len - ret);
	if (ret < 0)
		return ENAMETOOLONG;

	printf("%s\n", buf);
	return 0;
}

static int
parent_f(
	int			argc,
	char			**argv)
{
	char			pathbuf[MAXPATHLEN + 1];
	struct pptr_args	args = {
		.pathbuf	= pathbuf,
	};
	struct xfs_handle	handle;
	void			*hanp = NULL;
	size_t			hlen;
	struct fs_path		*fs;
	char			*p;
	uint64_t		ino = 0;
	uint32_t		gen = 0;
	int			c;
	int			listpath_flag = 0;
	int			ret;
	size_t			ioctl_bufsize = 8192;
	bool			single_path = false;
	static int		tab_init;

	if (!tab_init) {
		tab_init = 1;
		fs_table_initialise(0, NULL, 0, NULL);
	}
	fs = fs_table_lookup(file->name, FS_MOUNT_POINT);
	if (!fs) {
		fprintf(stderr, _("file argument, \"%s\", is not in a mounted XFS filesystem\n"),
			file->name);
		exitcode = 1;
		return 1;
	}
	mntpt = fs->fs_dir;

	while ((c = getopt(argc, argv, "b:i:n:psz")) != EOF) {
		switch (c) {
		case 'b':
			errno = 0;
			ioctl_bufsize = atoi(optarg);
			if (errno) {
				perror(optarg);
				exitcode = 1;
				return 1;
			}
			break;
		case 'i':
			args.filter_ino = strtoull(optarg, &p, 0);
			if (*p != '\0' || args.filter_ino == 0) {
				fprintf(stderr, _("Bad inode number '%s'.\n"),
						optarg);
				exitcode = 1;
				return 1;
			}
			break;
		case 'n':
			args.filter_name = optarg;
			break;
		case 'p':
			listpath_flag = 1;
			break;
		case 's':
			args.shortformat = true;
			break;
		case 'z':
			single_path = true;
			break;
		default:
			return command_usage(&parent_cmd);
		}
	}

	/*
	 * Always initialize the fshandle table because we need it for
	 * the ppaths functions to work.
	 */
	ret = path_to_fshandle((char *)mntpt, &hanp, &hlen);
	if (ret) {
		perror(mntpt);
		return 0;
	}

	if (optind + 2 == argc) {
		ino = strtoull(argv[optind], &p, 0);
		if (*p != '\0' || ino == 0) {
			fprintf(stderr,
				_("Bad inode number '%s'.\n"),
				argv[optind]);
			return 0;
		}
		gen = strtoul(argv[optind + 1], &p, 0);
		if (*p != '\0') {
			fprintf(stderr,
				_("Bad generation number '%s'.\n"),
				argv[optind + 1]);
			return 0;
		}

		handle_from_fshandle(&handle, hanp, hlen);
		handle_from_inogen(&handle, ino, gen);
	} else if (optind != argc) {
		return command_usage(&parent_cmd);
	}

	if (single_path) {
		if (ino)
			ret = handle_to_path(&handle, sizeof(handle),
					ioctl_bufsize, pathbuf, MAXPATHLEN);
		else
			ret = fd_to_path(file->fd, ioctl_bufsize,
					pathbuf, MAXPATHLEN);
		if (!ret)
			printf("%s\n", pathbuf);
	} else if (listpath_flag) {
		if (ino)
			ret = handle_walk_paths(&handle, sizeof(handle),
					ioctl_bufsize, paths_print, &args);
		else
			ret = fd_walk_paths(file->fd, ioctl_bufsize,
					paths_print, &args);
	} else {
		if (ino)
			ret = handle_walk_parents(&handle, sizeof(handle),
					ioctl_bufsize, pptr_print, &args);
		else
			ret = fd_walk_parents(file->fd, ioctl_bufsize,
					pptr_print, &args);
	}

	if (hanp)
		free_handle(hanp, hlen);
	if (ret) {
		exitcode = 1;
		fprintf(stderr, "%s: %s\n", file->name, strerror(ret));
	}
	return 0;
}

static void
parent_help(void)
{
printf(_(
"\n"
" list the current file's parents and their filenames\n"
"\n"
" -b -- use this many bytes to hold parent pointer records\n"
" -i -- Only show parent pointer records containing the given inode\n"
" -n -- Only show parent pointer records containing the given filename\n"
" -p -- list the current file's paths up to the root\n"
" -s -- Print records in short format: ino/gen/namelen/filename\n"
" -z -- print only the first path from the root\n"
"\n"
"If ino and gen are supplied, use them instead.\n"
"\n"));
}

void
parent_init(void)
{
	parent_cmd.name = "parent";
	parent_cmd.cfunc = parent_f;
	parent_cmd.argmin = 0;
	parent_cmd.argmax = -1;
	parent_cmd.args = _("[-psz] [-b bufsize] [-i ino] [-n name] [ino gen]");
	parent_cmd.flags = CMD_NOMAP_OK;
	parent_cmd.oneline = _("print parent inodes");
	parent_cmd.help = parent_help;

	add_command(&parent_cmd);
}
