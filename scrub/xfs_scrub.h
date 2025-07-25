// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef XFS_SCRUB_XFS_SCRUB_H_
#define XFS_SCRUB_XFS_SCRUB_H_

#include "libfrog/fsgeom.h"
#include "libfrog/histogram.h"

extern char *progname;

#define _PATH_PROC_MOUNTS	"/proc/mounts"

extern unsigned int		force_nr_threads;
extern unsigned int		bg_mode;
extern unsigned int		debug;
extern bool			verbose;
extern long			page_size;
extern bool			want_fstrim;
extern bool			stderr_isatty;
extern bool			stdout_isatty;
extern bool			is_service;
extern bool			use_force_rebuild;
extern bool			info_is_warning;

enum scrub_mode {
	/*
	 * Prior to phase 1, this means that xfs_scrub should read the
	 * "autofsck" fs property from the mount and set the value
	 * appropriate.  If it's still set after phase 1, this means we should
	 * exit without doing anything.
	 */
	SCRUB_MODE_NONE,
	SCRUB_MODE_DRY_RUN,
	SCRUB_MODE_PREEN,
	SCRUB_MODE_REPAIR,
};

enum error_action {
	ERRORS_CONTINUE,
	ERRORS_SHUTDOWN,
};

struct scrub_ctx {
	/* Immutable scrub state. */

	/* Mountpoint we use for presentation */
	char			*mntpoint;

	/* Actual VFS path to the filesystem */
	char			*actual_mntpoint;

	/* Mountpoint info */
	struct stat		mnt_sb;
	struct statvfs		mnt_sv;
	struct statfs		mnt_sf;

	/* Open block devices */
	struct disk		*datadev;
	struct disk		*logdev;
	struct disk		*rtdev;

	/* What does the user want us to do? */
	enum scrub_mode		mode;

	/* How does the user want us to react to errors? */
	enum error_action	error_action;

	/* xfrog context for the mount point */
	struct xfs_fd		mnt;

	/* Number of threads for metadata scrubbing */
	unsigned int		nr_io_threads;

	/* XFS specific geometry */
	struct fs_path		fsinfo;
	void			*fshandle;
	size_t			fshandle_len;

	/* Data block read verification buffer */
	void			*readbuf;

	/* Mutable scrub state; use lock. */
	pthread_mutex_t		lock;
	struct action_list	*fs_repair_list;
	struct action_list	*file_repair_list;
	unsigned long long	max_errors;
	unsigned long long	runtime_errors;
	unsigned long long	corruptions_found;
	unsigned long long	unfixable_errors;
	unsigned long long	warnings_found;
	unsigned long long	inodes_checked;
	unsigned long long	bytes_checked;
	unsigned long long	naming_warnings;
	unsigned long long	repairs;
	unsigned long long	preens;
	bool			scrub_setup_succeeded;
	bool			preen_triggers[XFS_SCRUB_TYPE_NR];

	/* Free space histograms, in fsb */
	struct histogram	datadev_hist;
	struct histogram	rtdev_hist;

	/*
	 * Pick the largest value for fstrim minlen such that we trim at least
	 * this much space per volume.
	 */
	double			fstrim_block_pct;
};

/*
 * Trim only enough free space extents (in order of decreasing length) to
 * ensure that this percentage of the free space is trimmed.
 */
#define FSTRIM_BLOCK_PCT_DEFAULT	(99.0 / 100.0)

/* Phase helper functions */
void xfs_shutdown_fs(struct scrub_ctx *ctx);
int scrub_cleanup(struct scrub_ctx *ctx);
int phase1_func(struct scrub_ctx *ctx);
int phase2_func(struct scrub_ctx *ctx);
int phase3_func(struct scrub_ctx *ctx);
int phase4_func(struct scrub_ctx *ctx);
int phase5_func(struct scrub_ctx *ctx);
int phase6_func(struct scrub_ctx *ctx);
int phase7_func(struct scrub_ctx *ctx);
int phase8_func(struct scrub_ctx *ctx);

/* Progress estimator functions */
unsigned int scrub_estimate_ag_work(struct scrub_ctx *ctx);
unsigned int scrub_estimate_iscan_work(struct scrub_ctx *ctx);
int phase2_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);
int phase3_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);
int phase4_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);
int phase5_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);
int phase6_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);
int phase8_estimate(struct scrub_ctx *ctx, uint64_t *items,
		    unsigned int *nr_threads, int *rshift);

#endif /* XFS_SCRUB_XFS_SCRUB_H_ */
