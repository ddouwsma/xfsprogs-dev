// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Oracle.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "init.h"
#include "io.h"
#include "libfrog/fsgeom.h"
#include "libfrog/logging.h"

static cmdinfo_t fsuuid_cmd;
static cmdinfo_t sysfspath_cmd;

static int
fsuuid_f(
	int			argc,
	char			**argv)
{
	struct xfs_fsop_geom	fsgeo;
	int			ret;
	char			bp[40];

	ret = -xfrog_geometry(file->fd, &fsgeo);

	if (ret) {
		xfrog_perror(ret, "XFS_IOC_FSGEOMETRY");
		exitcode = 1;
	} else {
		platform_uuid_unparse((uuid_t *)fsgeo.uuid, bp);
		printf("UUID = %s\n", bp);
	}

	return 0;
}

#ifndef FS_IOC_GETFSSYSFSPATH
struct fs_sysfs_path {
	__u8			len;
	__u8			name[128];
};
#define FS_IOC_GETFSSYSFSPATH		_IOR(0x15, 1, struct fs_sysfs_path)
#endif

static void
sysfspath_help(void)
{
	printf(_(
"\n"
" print the sysfs path for the open file\n"
"\n"
" Prints the path in sysfs where one might find information about the\n"
" filesystem backing the open files.  The path is not required to exist.\n"
" -d	-- return the path in debugfs, if any\n"
"\n"));
}

static int
sysfspath_f(
	int			argc,
	char			**argv)
{
	struct fs_sysfs_path	path;
	bool			debugfs = false;
	int			c;
	int			ret;

	while ((c = getopt(argc, argv, "d")) != EOF) {
		switch (c) {
		case 'd':
			debugfs = true;
			break;
		default:
			exitcode = 1;
			return command_usage(&sysfspath_cmd);
		}
	}

	ret = ioctl(file->fd, FS_IOC_GETFSSYSFSPATH, &path);
	if (ret) {
		xfrog_perror(ret, "FS_IOC_GETSYSFSPATH");
		exitcode = 1;
		return 0;
	}

	if (debugfs)
		printf("/sys/kernel/debug/%.*s\n", path.len, path.name);
	else
		printf("/sys/fs/%.*s\n", path.len, path.name);
	return 0;
}

void
fsuuid_init(void)
{
	fsuuid_cmd.name = "fsuuid";
	fsuuid_cmd.cfunc = fsuuid_f;
	fsuuid_cmd.argmin = 0;
	fsuuid_cmd.argmax = 0;
	fsuuid_cmd.flags = CMD_FLAG_ONESHOT | CMD_NOMAP_OK;
	fsuuid_cmd.oneline = _("get mounted filesystem UUID");

	add_command(&fsuuid_cmd);

	sysfspath_cmd.name = "sysfspath";
	sysfspath_cmd.cfunc = sysfspath_f;
	sysfspath_cmd.argmin = 0;
	sysfspath_cmd.argmax = -1;
	sysfspath_cmd.args = _("-d");
	sysfspath_cmd.flags = CMD_NOMAP_OK | CMD_FLAG_FOREIGN_OK;
	sysfspath_cmd.oneline = _("get mounted filesystem sysfs path");
	sysfspath_cmd.help = sysfspath_help;

	add_command(&sysfspath_cmd);
}
