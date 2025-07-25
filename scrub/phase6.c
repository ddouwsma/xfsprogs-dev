// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <linux/fsmap.h>
#include "handle.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "libfrog/bitmap.h"
#include "disk.h"
#include "filemap.h"
#include "fscounters.h"
#include "inodes.h"
#include "read_verify.h"
#include "spacemap.h"
#include "vfs.h"
#include "common.h"
#include "libfrog/bulkstat.h"

/*
 * Phase 6: Verify data file integrity.
 *
 * Identify potential data block extents with GETFSMAP, then feed those
 * extents to the read-verify pool to get the verify commands batched,
 * issued, and (if there are problems) reported back to us.  If there
 * are errors, we'll record the bad regions and (if available) use rmap
 * to tell us if metadata are now corrupt.  Otherwise, we'll scan the
 * whole directory tree looking for files that overlap the bad regions
 * and report the paths of the now corrupt files.
 */

/* Verify disk blocks with GETFSMAP */

struct media_verify_state {
	struct read_verify_pool	*rvp_data;
	struct read_verify_pool	*rvp_log;
	struct read_verify_pool	*rvp_realtime;
	struct bitmap		*d_bad;		/* bytes */
	struct bitmap		*r_bad;		/* bytes */
	bool			d_trunc:1;
	bool			r_trunc:1;
	bool			l_trunc:1;
};

/* Find the fd for a given device identifier. */
static struct read_verify_pool *
dev_to_pool(
	struct scrub_ctx		*ctx,
	struct media_verify_state	*vs,
	dev_t				dev)
{
	if (ctx->mnt.fsgeom.rtstart) {
		if (dev == XFS_DEV_DATA)
			return vs->rvp_data;
		if (dev == XFS_DEV_LOG)
			return vs->rvp_log;
		if (dev == XFS_DEV_RT)
			return vs->rvp_realtime;
	} else {
		if (dev == ctx->fsinfo.fs_datadev)
			return vs->rvp_data;
		if (dev == ctx->fsinfo.fs_logdev)
			return vs->rvp_log;
		if (dev == ctx->fsinfo.fs_rtdev)
			return vs->rvp_realtime;
	}
	abort();
}

/* Find the device major/minor for a given file descriptor. */
static dev_t
disk_to_dev(
	struct scrub_ctx	*ctx,
	struct disk		*disk)
{
	if (ctx->mnt.fsgeom.rtstart) {
		if (disk == ctx->datadev)
			return XFS_DEV_DATA;
		if (disk == ctx->logdev)
			return XFS_DEV_LOG;
		if (disk == ctx->rtdev)
			return XFS_DEV_RT;
	} else {
		if (disk == ctx->datadev)
			return ctx->fsinfo.fs_datadev;
		if (disk == ctx->logdev)
			return ctx->fsinfo.fs_logdev;
		if (disk == ctx->rtdev)
			return ctx->fsinfo.fs_rtdev;
	}
	abort();
}

/* Find the incore bad blocks bitmap for a given disk. */
static struct bitmap *
bitmap_for_disk(
	struct scrub_ctx		*ctx,
	struct disk			*disk,
	struct media_verify_state	*vs)
{
	if (disk == ctx->datadev)
		return vs->d_bad;
	if (disk == ctx->rtdev)
		return vs->r_bad;
	return NULL;
}

struct disk_ioerr_report {
	struct scrub_ctx	*ctx;
	struct disk		*disk;
};

struct owner_decode {
	uint64_t		owner;
	const char		*descr;
};

static const struct owner_decode special_owners[] = {
	{XFS_FMR_OWN_FREE,	"free space"},
	{XFS_FMR_OWN_UNKNOWN,	"unknown owner"},
	{XFS_FMR_OWN_FS,	"static FS metadata"},
	{XFS_FMR_OWN_LOG,	"journalling log"},
	{XFS_FMR_OWN_AG,	"per-AG metadata"},
	{XFS_FMR_OWN_INOBT,	"inode btree blocks"},
	{XFS_FMR_OWN_INODES,	"inodes"},
	{XFS_FMR_OWN_REFC,	"refcount btree"},
	{XFS_FMR_OWN_COW,	"CoW staging"},
	{XFS_FMR_OWN_DEFECTIVE,	"bad blocks"},
	{0, NULL},
};

/* Decode a special owner. */
static const char *
decode_special_owner(
	uint64_t			owner)
{
	const struct owner_decode	*od = special_owners;

	while (od->descr) {
		if (od->owner == owner)
			return od->descr;
		od++;
	}

	return NULL;
}

/* Routines to translate bad physical extents into file paths and offsets. */

struct badfile_report {
	struct scrub_ctx		*ctx;
	const char			*descr;
	struct media_verify_state	*vs;
	struct file_bmap		*bmap;
};

/* Report on bad extents found during a media scan. */
static int
report_badfile(
	uint64_t		start,
	uint64_t		length,
	void			*arg)
{
	struct badfile_report	*br = arg;
	unsigned long long	bad_offset;
	unsigned long long	bad_length;

	/* Clamp the bad region to the file mapping. */
	if (start < br->bmap->bm_physical) {
		length -= br->bmap->bm_physical - start;
		start = br->bmap->bm_physical;
	}
	length = min(length, br->bmap->bm_length);

	/* Figure out how far into the bmap is the bad mapping and report it. */
	bad_offset = start - br->bmap->bm_physical;
	bad_length = min(start + length,
			 br->bmap->bm_physical + br->bmap->bm_length) - start;

	str_unfixable_error(br->ctx, br->descr,
_("media error at data offset %llu length %llu."),
			br->bmap->bm_offset + bad_offset, bad_length);
	return 0;
}

/* Report if this extent overlaps a bad region. */
static int
report_data_loss(
	struct scrub_ctx		*ctx,
	int				fd,
	int				whichfork,
	struct fsxattr			*fsx,
	struct file_bmap		*bmap,
	void				*arg)
{
	struct badfile_report		*br = arg;
	struct media_verify_state	*vs = br->vs;
	struct bitmap			*bmp;

	br->bmap = bmap;

	/* Only report errors for real extents. */
	if (bmap->bm_flags & (BMV_OF_PREALLOC | BMV_OF_DELALLOC))
		return 0;

	if (fsx->fsx_xflags & FS_XFLAG_REALTIME)
		bmp = vs->r_bad;
	else
		bmp = vs->d_bad;

	return -bitmap_iterate_range(bmp, bmap->bm_physical, bmap->bm_length,
			report_badfile, br);
}

/* Report if the extended attribute data overlaps a bad region. */
static int
report_attr_loss(
	struct scrub_ctx		*ctx,
	int				fd,
	int				whichfork,
	struct fsxattr			*fsx,
	struct file_bmap		*bmap,
	void				*arg)
{
	struct badfile_report		*br = arg;
	struct media_verify_state	*vs = br->vs;
	struct bitmap			*bmp = vs->d_bad;

	/* Complain about attr fork extents that don't look right. */
	if (bmap->bm_flags & (BMV_OF_PREALLOC | BMV_OF_DELALLOC)) {
		str_info(ctx, br->descr,
_("found unexpected unwritten/delalloc attr fork extent."));
		return 0;
	}

	if (fsx->fsx_xflags & FS_XFLAG_REALTIME) {
		str_info(ctx, br->descr,
_("found unexpected realtime attr fork extent."));
		return 0;
	}

	if (bitmap_test(bmp, bmap->bm_physical, bmap->bm_length))
		str_corrupt(ctx, br->descr,
_("media error in extended attribute data."));

	return 0;
}

/* Iterate the extent mappings of a file to report errors. */
static int
report_fd_loss(
	struct scrub_ctx		*ctx,
	const char			*descr,
	int				fd,
	void				*arg)
{
	struct badfile_report		br = {
		.ctx			= ctx,
		.vs			= arg,
		.descr			= descr,
	};
	struct file_bmap		key = {0};
	int				ret;

	/* data fork */
	ret = scrub_iterate_filemaps(ctx, fd, XFS_DATA_FORK, &key,
			report_data_loss, &br);
	if (ret) {
		str_liberror(ctx, ret, descr);
		return ret;
	}

	/* attr fork */
	ret = scrub_iterate_filemaps(ctx, fd, XFS_ATTR_FORK, &key,
			report_attr_loss, &br);
	if (ret) {
		str_liberror(ctx, ret, descr);
		return ret;
	}

	return 0;
}

/* Report read verify errors in unlinked (but still open) files. */
static int
report_inode_loss(
	struct scrub_ctx		*ctx,
	struct xfs_handle		*handle,
	struct xfs_bulkstat		*bstat,
	void				*arg)
{
	char				descr[DESCR_BUFSZ];
	int				fd;
	int				error, err2;

	/* Ignore linked files and things we can't open. */
	if (bstat->bs_nlink != 0)
		return 0;
	if (!S_ISREG(bstat->bs_mode) && !S_ISDIR(bstat->bs_mode))
		return 0;

	scrub_render_ino_descr(ctx, descr, DESCR_BUFSZ,
			bstat->bs_ino, bstat->bs_gen, _("(unlinked)"));

	/* Try to open the inode. */
	fd = scrub_open_handle(handle);
	if (fd < 0) {
		/* Handle is stale, try again. */
		if (errno == ESTALE)
			return ESTALE;

		str_error(ctx, descr,
 _("Could not open to report read errors: %s."),
				strerror(errno));
		return 0;
	}

	/* Go find the badness. */
	error = report_fd_loss(ctx, descr, fd, arg);

	err2 = close(fd);
	if (err2)
		str_errno(ctx, descr);

	return error;
}

/* Scan a directory for matches in the read verify error list. */
static int
report_dir_loss(
	struct scrub_ctx	*ctx,
	const char		*path,
	int			dir_fd,
	void			*arg)
{
	return report_fd_loss(ctx, path, dir_fd, arg);
}

/*
 * Scan the inode associated with a directory entry for matches with
 * the read verify error list.
 */
static int
report_dirent_loss(
	struct scrub_ctx	*ctx,
	const char		*path,
	int			dir_fd,
	struct dirent		*dirent,
	struct stat		*sb,
	void			*arg)
{
	int			fd;
	int			error, err2;

	/* Ignore things we can't open. */
	if (!S_ISREG(sb->st_mode) && !S_ISDIR(sb->st_mode))
		return 0;

	/* Ignore . and .. */
	if (!strcmp(".", dirent->d_name) || !strcmp("..", dirent->d_name))
		return 0;

	/*
	 * If we were given a dirent, open the associated file under
	 * dir_fd for badblocks scanning.  If dirent is NULL, then it's
	 * the directory itself we want to scan.
	 */
	fd = openat(dir_fd, dirent->d_name,
			O_RDONLY | O_NOATIME | O_NOFOLLOW | O_NOCTTY);
	if (fd < 0) {
		char		descr[PATH_MAX + 1];

		if (errno == ENOENT)
			return 0;

		snprintf(descr, PATH_MAX, "%s/%s", path, dirent->d_name);
		descr[PATH_MAX] = 0;

		str_error(ctx, descr,
 _("Could not open to report read errors: %s."),
				strerror(errno));
		return 0;
	}

	/* Go find the badness. */
	error = report_fd_loss(ctx, path, fd, arg);

	err2 = close(fd);
	if (err2)
		str_errno(ctx, path);
	if (!error && err2)
		error = err2;

	return error;
}

struct ioerr_filerange {
	uint64_t		physical;
	uint64_t		length;
};

/*
 * If reverse mapping and parent pointers are enabled, we can map media errors
 * directly back to a filename and a file position without needing to walk the
 * directory tree.
 */
static inline bool
can_use_pptrs(
	const struct scrub_ctx	*ctx)
{
	return  (ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_PARENT) &&
		(ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT);
}

/* Use a fsmap to report metadata lost to a media error. */
static int
report_ioerr_fsmap(
	struct scrub_ctx	*ctx,
	struct fsmap		*map,
	void			*arg)
{
	const char		*type;
	struct xfs_bulkstat	bs = { };
	char			buf[DESCR_BUFSZ];
	struct ioerr_filerange	*fr = arg;
	uint64_t		err_off;
	int			ret;

	/* Don't care about unwritten extents. */
	if (map->fmr_flags & FMR_OF_PREALLOC)
		return 0;

	if (fr->physical > map->fmr_physical)
		err_off = fr->physical - map->fmr_physical;
	else
		err_off = 0;

	/* Report special owners */
	if (map->fmr_flags & FMR_OF_SPECIAL_OWNER) {
		snprintf(buf, DESCR_BUFSZ, _("disk offset %"PRIu64),
				(uint64_t)map->fmr_physical + err_off);
		type = decode_special_owner(map->fmr_owner);
		/*
		 * On filesystems that don't store reverse mappings, the
		 * GETFSMAP call returns OWNER_UNKNOWN for allocated space.
		 * We'll have to let the directory tree walker find the file
		 * that lost data.
		 */
		if (!(ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_RMAPBT) &&
		    map->fmr_owner == XFS_FMR_OWN_UNKNOWN) {
			str_info(ctx, buf, _("media error detected."));
		} else {
			str_corrupt(ctx, buf, _("media error in %s."), type);
		}
	}

	if (can_use_pptrs(ctx)) {
		ret = -xfrog_bulkstat_single(&ctx->mnt, map->fmr_owner, 0, &bs);
		if (ret)
			str_liberror(ctx, ret,
					_("bulkstat for media error report"));
	}

	/* Report extent maps */
	if (map->fmr_flags & FMR_OF_EXTENT_MAP) {
		bool		attr = (map->fmr_flags & FMR_OF_ATTR_FORK);

		scrub_render_ino_descr(ctx, buf, DESCR_BUFSZ,
				map->fmr_owner, bs.bs_gen, " %s",
				attr ? _("extended attribute") :
				       _("file data"));
		str_corrupt(ctx, buf, _("media error in extent map"));
	}

	/*
	 * If directory parent pointers are available, use that to find the
	 * pathname to a file, and report that path as having lost its
	 * extended attributes, or the precise offset of the lost file data.
	 */
	if (!can_use_pptrs(ctx))
		return 0;

	scrub_render_ino_descr(ctx, buf, DESCR_BUFSZ, map->fmr_owner,
			bs.bs_gen, NULL);

	if (map->fmr_flags & FMR_OF_ATTR_FORK) {
		str_corrupt(ctx, buf, _("media error in extended attributes"));
		return 0;
	}

	str_unfixable_error(ctx, buf,
 _("media error at data offset %llu length %llu."),
			err_off, fr->length);
	return 0;
}

/*
 * For a range of bad blocks, visit each space mapping that overlaps the bad
 * range so that we can report lost metadata.
 */
static int
report_ioerr(
	uint64_t			start,
	uint64_t			length,
	void				*arg)
{
	struct fsmap			keys[2] = { };
	struct ioerr_filerange		fr = {
		.physical		= start,
		.length			= length,
	};
	struct disk_ioerr_report	*dioerr = arg;

	/* Go figure out which blocks are bad from the fsmap. */
	keys[0].fmr_device = disk_to_dev(dioerr->ctx, dioerr->disk);
	keys[0].fmr_physical = start;
	keys[1].fmr_device = keys[0].fmr_device;
	keys[1].fmr_physical = start + length - 1;
	keys[1].fmr_owner = ULLONG_MAX;
	keys[1].fmr_offset = ULLONG_MAX;
	keys[1].fmr_flags = UINT_MAX;
	return -scrub_iterate_fsmap(dioerr->ctx, keys, report_ioerr_fsmap,
			&fr);
}

/* Report all the media errors found on a disk. */
static int
report_disk_ioerrs(
	struct scrub_ctx		*ctx,
	struct disk			*disk,
	struct media_verify_state	*vs)
{
	struct disk_ioerr_report	dioerr = {
		.ctx			= ctx,
		.disk			= disk,
	};
	struct bitmap			*tree;

	if (!disk)
		return 0;
	tree = bitmap_for_disk(ctx, disk, vs);
	if (!tree)
		return 0;
	return -bitmap_iterate(tree, report_ioerr, &dioerr);
}

/* Given bad extent lists for the data & rtdev, find bad files. */
static int
report_all_media_errors(
	struct scrub_ctx		*ctx,
	struct media_verify_state	*vs)
{
	int				ret;

	if (vs->d_trunc)
		str_corrupt(ctx, ctx->mntpoint, _("data device truncated"));
	if (vs->l_trunc)
		str_corrupt(ctx, ctx->mntpoint, _("log device truncated"));
	if (vs->r_trunc)
		str_corrupt(ctx, ctx->mntpoint, _("rt device truncated"));

	ret = report_disk_ioerrs(ctx, ctx->datadev, vs);
	if (ret) {
		str_liberror(ctx, ret, _("walking datadev io errors"));
		return ret;
	}

	ret = report_disk_ioerrs(ctx, ctx->rtdev, vs);
	if (ret) {
		str_liberror(ctx, ret, _("walking rtdev io errors"));
		return ret;
	}

	/*
	 * Scan the directory tree to get file paths if we didn't already use
	 * directory parent pointers to report the loss.  If parent pointers
	 * are enabled, report_ioerr_fsmap will have already reported file
	 * paths that have lost file data and xattrs.
	 */
	if (can_use_pptrs(ctx))
		return 0;

	ret = scan_fs_tree(ctx, report_dir_loss, report_dirent_loss, vs);
	if (ret)
		return ret;

	/* Scan for unlinked files. */
	return scrub_scan_user_files(ctx, report_inode_loss, vs);
}

/* Schedule a read-verify of a (data block) extent. */
static int
check_rmap(
	struct scrub_ctx		*ctx,
	struct fsmap			*map,
	void				*arg)
{
	struct media_verify_state	*vs = arg;
	struct read_verify_pool		*rvp;
	int				ret;

	rvp = dev_to_pool(ctx, vs, map->fmr_device);

	dbg_printf("rmap dev %d:%d phys %"PRIu64" owner %"PRId64
			" offset %"PRIu64" len %"PRIu64" flags 0x%x\n",
			major(map->fmr_device), minor(map->fmr_device),
			(uint64_t)map->fmr_physical, (int64_t)map->fmr_owner,
			(uint64_t)map->fmr_offset, (uint64_t)map->fmr_length,
			map->fmr_flags);

	/* "Unknown" extents should be verified; they could be data. */
	if ((map->fmr_flags & FMR_OF_SPECIAL_OWNER) &&
			map->fmr_owner == XFS_FMR_OWN_UNKNOWN)
		map->fmr_flags &= ~FMR_OF_SPECIAL_OWNER;

	/*
	 * We only care about read-verifying data extents that have been
	 * written to disk.  This means we can skip "special" owners
	 * (metadata), xattr blocks, unwritten extents, and extent maps.
	 * These should all get checked elsewhere in the scrubber.
	 */
	if (map->fmr_flags & (FMR_OF_PREALLOC | FMR_OF_ATTR_FORK |
			      FMR_OF_EXTENT_MAP | FMR_OF_SPECIAL_OWNER))
		return 0;

	/* XXX: Filter out directory data blocks. */

	/* Schedule the read verify command for (eventual) running. */
	ret = read_verify_schedule_io(rvp, map->fmr_physical, map->fmr_length,
			vs);
	if (ret) {
		str_liberror(ctx, ret, _("scheduling media verify command"));
		return ret;
	}

	return 0;
}

/* Wait for read/verify actions to finish, then return # bytes checked. */
static int
clean_pool(
	struct read_verify_pool	*rvp,
	unsigned long long	*bytes_checked)
{
	uint64_t		pool_checked;
	int			ret;

	if (!rvp)
		return 0;

	ret = read_verify_force_io(rvp);
	if (ret)
		return ret;

	ret = read_verify_pool_flush(rvp);
	if (ret)
		goto out_destroy;

	ret = read_verify_bytes(rvp, &pool_checked);
	if (ret)
		goto out_destroy;

	*bytes_checked += pool_checked;
out_destroy:
	read_verify_pool_destroy(rvp);
	return ret;
}

/* Remember a media error for later. */
static void
remember_ioerr(
	struct scrub_ctx		*ctx,
	struct disk			*disk,
	uint64_t			start,
	uint64_t			length,
	int				error,
	void				*arg)
{
	struct media_verify_state	*vs = arg;
	struct bitmap			*tree;
	int				ret;

	if (!length) {
		if (disk == ctx->datadev)
			vs->d_trunc = true;
		else if (disk == ctx->logdev)
			vs->l_trunc = true;
		else if (disk == ctx->rtdev)
			vs->r_trunc = true;
		return;
	}

	tree = bitmap_for_disk(ctx, disk, vs);
	if (!tree) {
		str_liberror(ctx, ENOENT, _("finding bad block bitmap"));
		return;
	}

	ret = -bitmap_set(tree, start, length);
	if (ret)
		str_liberror(ctx, ret, _("setting bad block bitmap"));
}

/*
 * Read verify all the file data blocks in a filesystem.  Since XFS doesn't
 * do data checksums, we trust that the underlying storage will pass back
 * an IO error if it can't retrieve whatever we previously stored there.
 * If we hit an IO error, we'll record the bad blocks in a bitmap and then
 * scan the extent maps of the entire fs tree to figure (and the unlinked
 * inodes) out which files are now broken.
 */
int
phase6_func(
	struct scrub_ctx		*ctx)
{
	struct media_verify_state	vs = { NULL };
	int				ret, ret2, ret3;

	ret = -bitmap_alloc(&vs.d_bad);
	if (ret) {
		str_liberror(ctx, ret, _("creating datadev badblock bitmap"));
		return ret;
	}

	ret = -bitmap_alloc(&vs.r_bad);
	if (ret) {
		str_liberror(ctx, ret, _("creating realtime badblock bitmap"));
		goto out_dbad;
	}

	ret = read_verify_pool_alloc(ctx, ctx->datadev,
			ctx->mnt.fsgeom.blocksize, remember_ioerr,
			scrub_nproc(ctx), &vs.rvp_data);
	if (ret) {
		str_liberror(ctx, ret, _("creating datadev media verifier"));
		goto out_rbad;
	}
	if (ctx->logdev) {
		ret = read_verify_pool_alloc(ctx, ctx->logdev,
				ctx->mnt.fsgeom.blocksize, remember_ioerr,
				scrub_nproc(ctx), &vs.rvp_log);
		if (ret) {
			str_liberror(ctx, ret,
					_("creating logdev media verifier"));
			goto out_datapool;
		}
	}
	if (ctx->rtdev) {
		ret = read_verify_pool_alloc(ctx, ctx->rtdev,
				ctx->mnt.fsgeom.blocksize, remember_ioerr,
				scrub_nproc(ctx), &vs.rvp_realtime);
		if (ret) {
			str_liberror(ctx, ret,
					_("creating rtdev media verifier"));
			goto out_logpool;
		}
	}
	ret = scrub_scan_all_spacemaps(ctx, check_rmap, &vs);
	if (ret)
		goto out_rtpool;

	ret = clean_pool(vs.rvp_data, &ctx->bytes_checked);
	if (ret)
		str_liberror(ctx, ret, _("flushing datadev verify pool"));

	ret2 = clean_pool(vs.rvp_log, &ctx->bytes_checked);
	if (ret2)
		str_liberror(ctx, ret2, _("flushing logdev verify pool"));

	ret3 = clean_pool(vs.rvp_realtime, &ctx->bytes_checked);
	if (ret3)
		str_liberror(ctx, ret3, _("flushing rtdev verify pool"));

	/*
	 * If the verify flush didn't work or we found no bad blocks, we're
	 * done!  No errors detected.
	 */
	if (ret || ret2 || ret3)
		goto out_rbad;
	if (bitmap_empty(vs.d_bad) && bitmap_empty(vs.r_bad))
		goto out_rbad;

	/* Scan the whole dir tree to see what matches the bad extents. */
	ret = report_all_media_errors(ctx, &vs);

	bitmap_free(&vs.r_bad);
	bitmap_free(&vs.d_bad);
	return ret;

out_rtpool:
	if (vs.rvp_realtime) {
		read_verify_pool_abort(vs.rvp_realtime);
		read_verify_pool_destroy(vs.rvp_realtime);
	}
out_logpool:
	if (vs.rvp_log) {
		read_verify_pool_abort(vs.rvp_log);
		read_verify_pool_destroy(vs.rvp_log);
	}
out_datapool:
	read_verify_pool_abort(vs.rvp_data);
	read_verify_pool_destroy(vs.rvp_data);
out_rbad:
	bitmap_free(&vs.r_bad);
out_dbad:
	bitmap_free(&vs.d_bad);
	return ret;
}

/* Estimate how much work we're going to do. */
int
phase6_estimate(
	struct scrub_ctx	*ctx,
	uint64_t		*items,
	unsigned int		*nr_threads,
	int			*rshift)
{
	unsigned long long	d_blocks;
	unsigned long long	d_bfree;
	unsigned long long	r_blocks;
	unsigned long long	r_bfree;
	unsigned long long	dontcare;
	int			ret;

	ret = scrub_scan_estimate_blocks(ctx, &d_blocks, &d_bfree, &r_blocks,
			&r_bfree, &dontcare);
	if (ret) {
		str_liberror(ctx, ret, _("estimating verify work"));
		return ret;
	}

	*items = cvt_off_fsb_to_b(&ctx->mnt,
			(d_blocks - d_bfree) + (r_blocks - r_bfree));

	/*
	 * Each read-verify pool starts a thread pool, and each worker thread
	 * can contribute to the progress counter.  Hence we need to set
	 * nr_threads appropriately to handle that many threads.
	 */
	*nr_threads = disk_heads(ctx->datadev);
	if (ctx->rtdev)
		*nr_threads += disk_heads(ctx->rtdev);
	if (ctx->logdev)
		*nr_threads += disk_heads(ctx->logdev);
	*rshift = 20;
	return 0;
}
