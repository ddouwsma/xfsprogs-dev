// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <pthread.h>
#include "libfrog/util.h"
#include "libfrog/workqueue.h"
#include "input.h"
#include "libfrog/paths.h"
#include "handle.h"
#include "bitops.h"
#include "libfrog/avl64.h"
#include "list.h"
#include "xfs_scrub.h"
#include "common.h"
#include "disk.h"
#include "scrub.h"
#include "repair.h"
#include "libfrog/fsgeom.h"
#include "xfs_errortag.h"
#include "libfrog/fsprops.h"
#include "libfrog/fsproperties.h"

/* Phase 1: Find filesystem geometry (and clean up after) */

/* Shut down the filesystem. */
void
xfs_shutdown_fs(
	struct scrub_ctx		*ctx)
{
	int				flag;

	flag = XFS_FSOP_GOING_FLAGS_LOGFLUSH;
	str_info(ctx, ctx->mntpoint, _("Shutting down filesystem!"));
	if (ioctl(ctx->mnt.fd, XFS_IOC_GOINGDOWN, &flag))
		str_errno(ctx, ctx->mntpoint);
}

/*
 * If we haven't found /any/ problems at all, tell the kernel that we're giving
 * the filesystem a clean bill of health.
 */
static int
report_to_kernel(
	struct scrub_ctx	*ctx)
{
	struct scrub_item	sri;
	int			ret;

	if (!ctx->scrub_setup_succeeded || ctx->corruptions_found ||
	    ctx->runtime_errors || ctx->unfixable_errors ||
	    ctx->warnings_found)
		return 0;

	scrub_item_init_fs(&sri);
	scrub_item_schedule(&sri, XFS_SCRUB_TYPE_HEALTHY);
	ret = scrub_item_check(ctx, &sri);
	if (ret)
		return ret;

	/*
	 * Complain if we cannot fail the clean bill of health, unless we're
	 * just testing repairs.
	 */
	if (repair_item_count_needsrepair(&sri) != 0 &&
	    !debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		str_info(ctx, _("Couldn't upload clean bill of health."), NULL);
	}

	return 0;
}

/* Clean up the XFS-specific state data. */
int
scrub_cleanup(
	struct scrub_ctx	*ctx)
{
	int			error;

	error = report_to_kernel(ctx);
	if (error)
		return error;

	action_list_free(&ctx->file_repair_list);
	action_list_free(&ctx->fs_repair_list);

	if (ctx->fshandle)
		free_handle(ctx->fshandle, ctx->fshandle_len);
	if (ctx->rtdev)
		disk_close(ctx->rtdev);
	if (ctx->logdev)
		disk_close(ctx->logdev);
	if (ctx->datadev)
		disk_close(ctx->datadev);
	fshandle_destroy();
	error = -xfd_close(&ctx->mnt);
	if (error)
		str_liberror(ctx, error, _("closing mountpoint fd"));
	fs_table_destroy();

	return error;
}

/* Decide if we're using FORCE_REBUILD or injecting FORCE_REPAIR. */
static int
enable_force_repair(
	struct scrub_ctx		*ctx)
{
	struct xfs_error_injection	inject = {
		.fd			= ctx->mnt.fd,
		.errtag			= XFS_ERRTAG_FORCE_SCRUB_REPAIR,
	};
	int				error;

	use_force_rebuild = can_force_rebuild(ctx);
	if (use_force_rebuild)
		return 0;

	error = ioctl(ctx->mnt.fd, XFS_IOC_ERROR_INJECTION, &inject);
	if (error)
		str_errno(ctx, _("force_repair"));
	return error;
}

/*
 * Decide the operating mode from the autofsck fs property.  No fs property or
 * system errors means we check the fs if rmapbt or pptrs are enabled, or none
 * if it doesn't.
 */
static void
mode_from_autofsck(
	struct scrub_ctx	*ctx)
{
	struct fsprops_handle	fph = { };
	char			valuebuf[FSPROP_MAX_VALUELEN + 1] = { 0 };
	size_t			valuelen = FSPROP_MAX_VALUELEN;
	enum fsprop_autofsck	shval;
	int			ret;

	ret = fsprops_open_handle(&ctx->mnt, &ctx->fsinfo, &fph);
	if (ret)
		goto no_property;

	ret = fsprops_get(&fph, FSPROP_AUTOFSCK_NAME, valuebuf, &valuelen);
	if (ret)
		goto no_property;

	shval = fsprop_autofsck_read(valuebuf);
	switch (shval) {
	case FSPROP_AUTOFSCK_NONE:
		ctx->mode = SCRUB_MODE_NONE;
		break;
	case FSPROP_AUTOFSCK_OPTIMIZE:
		ctx->mode = SCRUB_MODE_PREEN;
		break;
	case FSPROP_AUTOFSCK_REPAIR:
		ctx->mode = SCRUB_MODE_REPAIR;
		break;
	case FSPROP_AUTOFSCK_UNSET:
		str_info(ctx, ctx->mntpoint,
 _("Unknown autofsck directive \"%s\"."),
				valuebuf);
		goto no_property;
	case FSPROP_AUTOFSCK_CHECK:
		ctx->mode = SCRUB_MODE_DRY_RUN;
		break;
	}

	fsprops_free_handle(&fph);

summarize:
	switch (ctx->mode) {
	case SCRUB_MODE_NONE:
		str_info(ctx, ctx->mntpoint,
 _("Disabling scrub per autofsck directive."));
		break;
	case SCRUB_MODE_DRY_RUN:
		str_info(ctx, ctx->mntpoint,
 _("Checking per autofsck directive."));
		break;
	case SCRUB_MODE_PREEN:
		str_info(ctx, ctx->mntpoint,
 _("Optimizing per autofsck directive."));
		break;
	case SCRUB_MODE_REPAIR:
		str_info(ctx, ctx->mntpoint,
 _("Checking and repairing per autofsck directive."));
		break;
	}

	return;
no_property:
	/*
	 * If we don't find an autofsck property, check the metadata if any
	 * backrefs are available for cross-referencing.  Otherwise do no
	 * checking.
	 */
	if (ctx->mnt.fsgeom.flags & (XFS_FSOP_GEOM_FLAGS_PARENT |
				     XFS_FSOP_GEOM_FLAGS_RMAPBT))
		ctx->mode = SCRUB_MODE_DRY_RUN;
	else
		ctx->mode = SCRUB_MODE_NONE;
	goto summarize;
}

/*
 * Bind to the mountpoint, read the XFS geometry, bind to the block devices.
 * Anything we've already built will be cleaned up by scrub_cleanup.
 */
int
phase1_func(
	struct scrub_ctx		*ctx)
{
	int				error;

	/*
	 * Open the directory with O_NOATIME.  For mountpoints owned
	 * by root, this should be sufficient to ensure that we have
	 * CAP_SYS_ADMIN, which we probably need to do anything fancy
	 * with the (XFS driver) kernel.
	 */
	error = -xfd_open(&ctx->mnt, ctx->actual_mntpoint,
			O_RDONLY | O_NOATIME | O_DIRECTORY);
	if (error) {
		if (error == EPERM)
			str_error(ctx, ctx->mntpoint,
_("Must be root to run scrub."));
		else if (error == ENOTTY)
			str_error(ctx, ctx->mntpoint,
_("Not an XFS filesystem."));
		else
			str_liberror(ctx, error, ctx->mntpoint);
		return error;
	}

	error = fstat(ctx->mnt.fd, &ctx->mnt_sb);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}
	error = fstatvfs(ctx->mnt.fd, &ctx->mnt_sv);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}
	error = fstatfs(ctx->mnt.fd, &ctx->mnt_sf);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}

	/*
	 * Flush everything out to disk before we start checking.
	 * This seems to reduce the incidence of stale file handle
	 * errors when we open things by handle.
	 */
	error = syncfs(ctx->mnt.fd);
	if (error) {
		str_errno(ctx, ctx->mntpoint);
		return error;
	}

	error = action_list_alloc(&ctx->fs_repair_list);
	if (error) {
		str_liberror(ctx, error, _("allocating fs repair list"));
		return error;
	}

	error = action_list_alloc(&ctx->file_repair_list);
	if (error) {
		str_liberror(ctx, error, _("allocating file repair list"));
		return error;
	}

	error = path_to_fshandle(ctx->actual_mntpoint, &ctx->fshandle,
			&ctx->fshandle_len);
	if (error) {
		str_errno(ctx, _("getting fshandle"));
		return error;
	}

	/*
	 * If we've been instructed to decide the operating mode from the
	 * autofsck fs property, do that now before we start downgrading based
	 * on actual fs/kernel capabilities.
	 */
	if (ctx->mode == SCRUB_MODE_NONE)
		mode_from_autofsck(ctx);

	/* Do we have kernel-assisted metadata scrubbing? */
	if (!can_scrub_fs_metadata(ctx) || !can_scrub_inode(ctx) ||
	    !can_scrub_bmap(ctx) || !can_scrub_dir(ctx) ||
	    !can_scrub_attr(ctx) || !can_scrub_symlink(ctx) ||
	    !can_scrub_parent(ctx)) {
		str_error(ctx, ctx->mntpoint,
_("Kernel metadata scrubbing facility is not available."));
		return ECANCELED;
	}

	check_scrubv(ctx);

	/*
	 * Normally, callers are required to pass -n if the provided path is a
	 * readonly filesystem or the kernel wasn't built with online repair
	 * enabled.  However, systemd services are not scripts and cannot
	 * determine either of these conditions programmatically.  Change the
	 * behavior to dry-run mode if either condition is detected.
	 */
	if (repair_want_service_downgrade(ctx)) {
		str_info(ctx, ctx->mntpoint,
_("Filesystem cannot be repaired in service mode, downgrading to dry-run mode."));
		ctx->mode = SCRUB_MODE_DRY_RUN;
	}

	/* Do we need kernel-assisted metadata repair? */
	if (ctx->mode != SCRUB_MODE_DRY_RUN && !can_repair(ctx)) {
		str_error(ctx, ctx->mntpoint,
_("Kernel metadata repair facility is not available.  Use -n to scrub."));
		return ECANCELED;
	}

	if (debug_tweak_on("XFS_SCRUB_FORCE_REPAIR")) {
		error = enable_force_repair(ctx);
		if (error)
			return error;
	}

	/* Did we find the log and rt devices, if they're present? */
	if (ctx->mnt.fsgeom.logstart == 0 && ctx->fsinfo.fs_log == NULL) {
		str_error(ctx, ctx->mntpoint,
_("Unable to find log device path."));
		return ECANCELED;
	}
	if (ctx->mnt.fsgeom.rtblocks && ctx->fsinfo.fs_rt == NULL &&
	    !ctx->mnt.fsgeom.rtstart) {
		str_error(ctx, ctx->mntpoint,
_("Unable to find realtime device path."));
		return ECANCELED;
	}

	/* Open the raw devices. */
	ctx->datadev = disk_open(ctx->fsinfo.fs_name);
	if (!ctx->datadev) {
		str_error(ctx, ctx->mntpoint, _("Unable to open data device."));
		return ECANCELED;
	}

	ctx->nr_io_threads = disk_heads(ctx->datadev);
	if (verbose) {
		fprintf(stdout, _("%s: using %d threads to scrub.\n"),
				ctx->mntpoint, scrub_nproc(ctx));
		fflush(stdout);
	}

	if (ctx->fsinfo.fs_log) {
		ctx->logdev = disk_open(ctx->fsinfo.fs_log);
		if (!ctx->logdev) {
			str_error(ctx, ctx->mntpoint,
				_("Unable to open external log device."));
			return ECANCELED;
		}
	}
	if (ctx->fsinfo.fs_rt) {
		ctx->rtdev = disk_open(ctx->fsinfo.fs_rt);
		if (!ctx->rtdev) {
			str_error(ctx, ctx->mntpoint,
				_("Unable to open realtime device."));
			return ECANCELED;
		}
	}

	/*
	 * Everything's set up, which means any failures recorded after
	 * this point are most probably corruption errors (as opposed to
	 * purely setup errors).
	 */
	log_info(ctx, _("Invoking online scrub."), ctx);
	ctx->scrub_setup_succeeded = true;
	return 0;
}
