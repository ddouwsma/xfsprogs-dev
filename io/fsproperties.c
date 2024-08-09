// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "command.h"
#include "init.h"
#include "libfrog/paths.h"
#include "input.h"
#include "libfrog/fsgeom.h"
#include "handle.h"
#include "io.h"
#include "libfrog/fsprops.h"
#include "libfrog/fsproperties.h"

static void
listfsprops_help(void)
{
	printf(_(
"Print the names of the filesystem properties stored in this filesystem.\n"
"\n"));
}

static int
fileio_to_fsprops_handle(
	struct fileio		*file,
	struct fsprops_handle	*fph)
{
	struct xfs_fd		xfd = XFS_FD_INIT(file->fd);
	struct fs_path		*fs;
	void			*hanp = NULL;
	size_t			hlen;
	int			ret;

	/*
	 * Look up the mount point info for the open file, which confirms that
	 * we were passed a mount point.
	 */
	fs = fs_table_lookup(file->name, FS_MOUNT_POINT);
	if (!fs) {
		fprintf(stderr, _("%s: Not a XFS mount point.\n"),
				file->name);
		goto bad;
	}

	/*
	 * Register the mountpoint in the fsfd cache so we can use handle
	 * functions later.
	 */
	ret = path_to_fshandle(fs->fs_dir, &hanp, &hlen);
	if (ret) {
		perror(fs->fs_dir);
		goto bad;
	}

	ret = -xfd_prepare_geometry(&xfd);
	if (ret) {
		perror(file->name);
		goto free_handle;
	}

	ret = fsprops_open_handle(&xfd, &file->fs_path, fph);
	if (ret) {
		if (errno == ESRMNT)
			fprintf(stderr, _("%s: Not a XFS mount point.\n"),
					file->name);
		else
			perror(file->name);
		goto free_handle;
	}

	free_handle(hanp, hlen);
	return 0;
free_handle:
	free_handle(hanp, hlen);
bad:
	exitcode = 1;
	return 1;
}

static int
print_fsprop(
	struct fsprops_handle	*fph,
	const char		*name,
	size_t			unused,
	void			*priv)
{
	bool			*print_values = priv;
	char			valuebuf[FSPROP_MAX_VALUELEN];
	size_t			valuelen = FSPROP_MAX_VALUELEN;
	int			ret;

	if (!*print_values) {
		printf("%s\n", name);
		return 0;
	}

	ret = fsprops_get(fph, name, valuebuf, &valuelen);
	if (ret)
		return ret;

	printf("%s=%.*s\n", name, (int)valuelen, valuebuf);
	return 0;
}

static int
listfsprops_f(
	int			argc,
	char			**argv)
{
	struct fsprops_handle	fph = { };
	bool			print_values = false;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "v")) != EOF) {
		switch (c) {
		case 'v':
			print_values = true;
			break;
		default:
			exitcode = 1;
			listfsprops_help();
			return 0;
		}
	}

	ret = fileio_to_fsprops_handle(file, &fph);
	if (ret)
		return 1;

	ret = fsprops_walk_names(&fph, print_fsprop, &print_values);
	if (ret) {
		perror(file->name);
		exitcode = 1;
	}

	fsprops_free_handle(&fph);
	return 0;
}

static struct cmdinfo listfsprops_cmd = {
	.name		= "listfsprops",
	.cfunc		= listfsprops_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK,
	.args		= "",
	.help		= listfsprops_help,
};

static void
getfsprops_help(void)
{
	printf(_(
"Print the values of filesystem properties stored in this filesystem.\n"
"\n"
"Pass property names as the arguments.\n"
"\n"));
}

static int
getfsprops_f(
	int			argc,
	char			**argv)
{
	struct fsprops_handle	fph = { };
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "")) != EOF) {
		switch (c) {
		default:
			exitcode = 1;
			getfsprops_help();
			return 0;
		}
	}

	ret = fileio_to_fsprops_handle(file, &fph);
	if (ret)
		return ret;

	for (c = optind; c < argc; c++) {
		char		valuebuf[FSPROP_MAX_VALUELEN];
		size_t		valuelen = FSPROP_MAX_VALUELEN;

		ret = fsprops_get(&fph, argv[c], valuebuf, &valuelen);
		if (ret) {
			perror(argv[c]);
			exitcode = 1;
			break;
		}

		printf("%s=%.*s\n", argv[c], (int)valuelen, valuebuf);
	}

	fsprops_free_handle(&fph);
	return 0;
}

static struct cmdinfo getfsprops_cmd = {
	.name		= "getfsprops",
	.cfunc		= getfsprops_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK,
	.args		= "",
	.help		= getfsprops_help,
};

static void
setfsprops_help(void)
{
	printf(_(
"Set values of filesystem properties stored in this filesystem.\n"
"\n"
" -f    Do not try to validate property value.\n"
"\n"
"Provide name=value tuples as the arguments.\n"
"\n"));
}

static int
setfsprops_f(
	int			argc,
	char			**argv)
{
	struct fsprops_handle	fph = { };
	bool			force = false;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "f")) != EOF) {
		switch (c) {
		case 'f':
			force = true;
			break;
		default:
			exitcode = 1;
			getfsprops_help();
			return 0;
		}
	}

	ret = fileio_to_fsprops_handle(file, &fph);
	if (ret)
		return ret;

	for (c = optind; c < argc; c ++) {
		char	*equals = strchr(argv[c], '=');

		if (!equals) {
			fprintf(stderr, _("%s: property value required.\n"),
					argv[c]);
			exitcode = 1;
			break;
		}

		*equals = 0;

		if (!force && !fsprop_validate(argv[c], equals + 1)) {
			fprintf(stderr, _("%s: invalid value \"%s\".\n"),
					argv[c], equals + 1);
			*equals = '=';
			exitcode = 1;
			break;
		}

		ret = fsprops_set(&fph, argv[c], equals + 1,
				strlen(equals + 1));
		if (ret) {
			perror(argv[c]);
			*equals = '=';
			exitcode = 1;
			break;
		}

		printf("%s=%s\n", argv[c], equals + 1);
		*equals = '=';
	}

	fsprops_free_handle(&fph);
	return 0;
}

static struct cmdinfo setfsprops_cmd = {
	.name		= "setfsprops",
	.cfunc		= setfsprops_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK,
	.args		= "",
	.help		= setfsprops_help,
};

static void
removefsprops_help(void)
{
	printf(_(
"Unset a filesystem property.\n"
"\n"
"Pass property names as the arguments.\n"
"\n"));
}

static int
removefsprops_f(
	int			argc,
	char			**argv)
{
	struct fsprops_handle	fph = { };
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "")) != EOF) {
		switch (c) {
		default:
			exitcode = 1;
			getfsprops_help();
			return 0;
		}
	}

	ret = fileio_to_fsprops_handle(file, &fph);
	if (ret)
		return ret;

	for (c = optind; c < argc; c++) {
		ret = fsprops_remove(&fph, argv[c]);
		if (ret) {
			perror(argv[c]);
			exitcode = 1;
			break;
		}
	}

	fsprops_free_handle(&fph);
	return 0;
}

static struct cmdinfo removefsprops_cmd = {
	.name		= "removefsprops",
	.cfunc		= removefsprops_f,
	.argmin		= 0,
	.argmax		= -1,
	.flags		= CMD_NOMAP_OK,
	.args		= "",
	.help		= removefsprops_help,
};

void
fsprops_init(void)
{
	listfsprops_cmd.oneline = _("list file system properties");
	getfsprops_cmd.oneline = _("print file system properties");
	setfsprops_cmd.oneline = _("set file system properties");
	removefsprops_cmd.oneline = _("unset file system properties");

	add_command(&listfsprops_cmd);
	add_command(&getfsprops_cmd);
	add_command(&setfsprops_cmd);
	add_command(&removefsprops_cmd);
}
