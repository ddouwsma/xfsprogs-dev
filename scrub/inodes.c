// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2018-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include "platform_defs.h"
#include "xfs_arch.h"
#include "handle.h"
#include "libfrog/paths.h"
#include "libfrog/workqueue.h"
#include "xfs_scrub.h"
#include "common.h"
#include "inodes.h"
#include "descr.h"
#include "libfrog/fsgeom.h"
#include "libfrog/bulkstat.h"
#include "libfrog/handle_priv.h"

/*
 * Iterate a range of inodes.
 *
 * This is a little more involved than repeatedly asking BULKSTAT for a
 * buffer's worth of stat data for some number of inodes.  We want to scan as
 * many of the inodes that the inobt thinks there are, so we use the INUMBERS
 * ioctl to walk all the inobt records in the filesystem and spawn a worker to
 * bulkstat and iterate.  The worker starts with an inumbers record that can
 * look like this:
 *
 * {startino = S, allocmask = 0b11011}
 *
 * Given a starting inumber S and count C=64, bulkstat will return a sorted
 * array of stat information.  The bs_ino of those array elements can look like
 * any of the following:
 *
 * 0. [S, S+1, S+3, S+4]
 * 1. [S+e, S+e+1, S+e+3, S+e+4, S+e+C+1...], where e >= 0
 * 2. [S+e+n], where n >= 0
 * 3. []
 * 4. [], errno == EFSCORRUPTED
 *
 * We know that bulkstat scanned the entire inode range between S and bs_ino of
 * the last array element, even though it only fills out an array element for
 * allocated inodes.  Therefore, we can say in cases 0-2 that S was filled,
 * even if there is no bstat[] record for S.  In turn, we can create a bitmask
 * of inodes that we have seen, and set bits 0 through (bstat[-1].bs_ino - S),
 * being careful not to set any bits past S+C.
 *
 * In case (0) we find that seen mask matches the inumber record
 * exactly, so the caller can walk the stat records and move on.  In case (1)
 * this is also true, but we must be careful to reduce the array length to
 * avoid scanning inodes that are not in the inumber chunk.  In case (3) we
 * conclude that there were no inodes left to scan and terminate.
 *
 * Inodes that are set in the allocmask but not set in the seen mask are the
 * corrupt inodes.  For each of these cases, we try to populate the bulkstat
 * array one inode at a time.  If the kernel returns a matching record we can
 * use it; if instead we receive an error, we synthesize enough of a record
 * to be able to run online scrub by handle.
 *
 * If the iteration function returns ESTALE, that means that the inode has
 * been deleted and possibly recreated since the BULKSTAT call.  We wil
 * refresh the stat information and try again up to 30 times before reporting
 * the staleness as an error.
 */

/*
 * Return the inumber of the highest inode in the bulkstat data, assuming the
 * records are sorted in inumber order.
 */
static inline uint64_t last_bstat_ino(const struct xfs_bulkstat_req *b)
{
	return b->hdr.ocount ? b->bulkstat[b->hdr.ocount - 1].bs_ino : 0;
}

/*
 * Deduce the bitmask of the inodes in inums that were seen by bulkstat.  If
 * the inode is present in the bstat array this is trivially true; or if it is
 * not in the array but higher inumbers are present, then it was freed.
 */
static __u64
seen_mask_from_bulkstat(
	const struct xfs_inumbers	*inums,
	__u64				breq_startino,
	const struct xfs_bulkstat_req	*breq)
{
	const __u64			limit_ino =
		inums->xi_startino + LIBFROG_BULKSTAT_CHUNKSIZE;
	const __u64			last = last_bstat_ino(breq);
	__u64				ret = 0;
	int				i, maxi;

	/* Ignore the bulkstat results if they don't cover inumbers */
	if (breq_startino > limit_ino || last < inums->xi_startino)
		return 0;

	maxi = min(LIBFROG_BULKSTAT_CHUNKSIZE, last - inums->xi_startino + 1);
	for (i = breq_startino - inums->xi_startino; i < maxi; i++)
		ret |= 1ULL << i;

	return ret;
}

#define cmp_int(l, r)		((l > r) - (l < r))

/* Compare two bulkstat records by inumber. */
static int
compare_bstat(
	const void		*a,
	const void		*b)
{
	const struct xfs_bulkstat *ba = a;
	const struct xfs_bulkstat *bb = b;

	return cmp_int(ba->bs_ino, bb->bs_ino);
}

/*
 * Run bulkstat on an entire inode allocation group, then check that we got
 * exactly the inodes we expected.  If not, load them one at a time (or fake
 * it) into the bulkstat data.
 */
static void
bulkstat_for_inumbers(
	struct scrub_ctx		*ctx,
	const struct xfs_inumbers	*inumbers,
	struct xfs_bulkstat_req		*breq)
{
	struct xfs_bulkstat		*bs = NULL;
	const uint64_t			limit_ino =
		inumbers->xi_startino + LIBFROG_BULKSTAT_CHUNKSIZE;
	uint64_t			seen_mask = 0;
	int				i;
	int				error;

	assert(inumbers->xi_allocmask != 0);

	/* First we try regular bulkstat, for speed. */
	breq->hdr.ino = inumbers->xi_startino;
	error = -xfrog_bulkstat(&ctx->mnt, breq);
	if (!error) {
		if (!breq->hdr.ocount)
			return;
		seen_mask |= seen_mask_from_bulkstat(inumbers,
					inumbers->xi_startino, breq);
	}

	/*
	 * Bulkstat might return inodes beyond xi_startino + CHUNKSIZE.  Reduce
	 * ocount to ignore inodes not described by the inumbers record.
	 */
	for (i = breq->hdr.ocount - 1; i >= 0; i--) {
		if (breq->bulkstat[i].bs_ino < limit_ino)
			break;
		breq->hdr.ocount--;
	}

	/*
	 * Walk the xi_allocmask looking for set bits that aren't present in
	 * the fill mask.  For each such inode, fill the entries at the end of
	 * the array with stat information one at a time, synthesizing them if
	 * necessary.  At this point, (xi_allocmask & ~seen_mask) should be the
	 * corrupt inodes.
	 */
	for (i = 0; i < LIBFROG_BULKSTAT_CHUNKSIZE; i++) {
		/*
		 * Don't single-step if inumbers said it wasn't allocated or
		 * bulkstat actually filled it.
		 */
		if (!(inumbers->xi_allocmask & (1ULL << i)))
			continue;
		if (seen_mask & (1ULL << i))
			continue;

		assert(breq->hdr.ocount < LIBFROG_BULKSTAT_CHUNKSIZE);

		if (!bs)
			bs = &breq->bulkstat[breq->hdr.ocount];

		/*
		 * Didn't get desired stat data and we've hit the end of the
		 * returned data.  We can't distinguish between the inode being
		 * freed vs. the inode being to corrupt to load, so try a
		 * bulkstat single to see if we can load the inode.
		 */
		error = -xfrog_bulkstat_single(&ctx->mnt,
				inumbers->xi_startino + i, breq->hdr.flags, bs);
		if (error || bs->bs_ino != inumbers->xi_startino + i) {
			memset(bs, 0, sizeof(struct xfs_bulkstat));
			bs->bs_ino = inumbers->xi_startino + i;
			bs->bs_blksize = ctx->mnt_sv.f_frsize;
		}

		breq->hdr.ocount++;
		bs++;
	}

	/* If we added any entries, re-sort the array. */
	if (bs)
		qsort(breq->bulkstat, breq->hdr.ocount,
				sizeof(struct xfs_bulkstat), compare_bstat);
}

/* BULKSTAT wrapper routines. */
struct scan_inodes {
	struct workqueue	wq_bulkstat;
	scrub_inode_iter_fn	fn;
	void			*arg;
	unsigned int		nr_threads;
	bool			aborted;
};

/*
 * A single unit of inode scan work.  This contains a pointer to the parent
 * information, followed by an INUMBERS request structure, followed by a
 * BULKSTAT request structure.  The last two are VLAs, so we can't represent
 * them here.
 */
struct scan_ichunk {
	struct scan_inodes	*si;
};

static inline struct xfs_inumbers_req *
ichunk_to_inumbers(
	struct scan_ichunk	*ichunk)
{
	char			*p = (char *)ichunk;

	return (struct xfs_inumbers_req *)(p + sizeof(struct scan_ichunk));
}

static inline struct xfs_bulkstat_req *
ichunk_to_bulkstat(
	struct scan_ichunk	*ichunk)
{
	char			*p = (char *)ichunk_to_inumbers(ichunk);

	return (struct xfs_bulkstat_req *)(p + XFS_INUMBERS_REQ_SIZE(1));
}

static inline int
alloc_ichunk(
	struct scrub_ctx	*ctx,
	struct scan_inodes	*si,
	uint32_t		agno,
	uint64_t		startino,
	struct scan_ichunk	**ichunkp)
{
	struct scan_ichunk	*ichunk;
	struct xfs_inumbers_req	*ireq;
	struct xfs_bulkstat_req	*breq;

	ichunk = calloc(1, sizeof(struct scan_ichunk) +
			   XFS_INUMBERS_REQ_SIZE(1) +
			   XFS_BULKSTAT_REQ_SIZE(LIBFROG_BULKSTAT_CHUNKSIZE));
	if (!ichunk)
		return -errno;

	ichunk->si = si;

	ireq = ichunk_to_inumbers(ichunk);
	ireq->hdr.icount = 1;
	ireq->hdr.ino = startino;
	ireq->hdr.agno = agno;
	ireq->hdr.flags |= XFS_BULK_IREQ_AGNO;

	breq = ichunk_to_bulkstat(ichunk);
	breq->hdr.icount = LIBFROG_BULKSTAT_CHUNKSIZE;

	/* Scan the metadata directory tree too. */
	if (ctx->mnt.fsgeom.flags & XFS_FSOP_GEOM_FLAGS_METADIR)
		breq->hdr.flags |= XFS_BULK_IREQ_METADIR;

	*ichunkp = ichunk;
	return 0;
}

static int
render_ino_from_bulkstat(
	struct scrub_ctx	*ctx,
	char			*buf,
	size_t			buflen,
	void			*data)
{
	struct xfs_bulkstat	*bstat = data;

	return scrub_render_ino_descr(ctx, buf, buflen, bstat->bs_ino,
			bstat->bs_gen, NULL);
}

static int
render_inumbers_from_agno(
	struct scrub_ctx	*ctx,
	char			*buf,
	size_t			buflen,
	void			*data)
{
	xfs_agnumber_t		*agno = data;

	return snprintf(buf, buflen, _("dev %d:%d AG %u inodes"),
				major(ctx->fsinfo.fs_datadev),
				minor(ctx->fsinfo.fs_datadev),
				*agno);
}

/*
 * Call BULKSTAT for information on a single chunk's worth of inodes and call
 * our iterator function.  We'll try to fill the bulkstat information in
 * batches, but we also can detect iget failures.
 */
static void
scan_ag_bulkstat(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct xfs_handle	handle;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct scan_ichunk	*ichunk = arg;
	struct xfs_inumbers_req	*ireq = ichunk_to_inumbers(ichunk);
	struct xfs_bulkstat_req	*breq = ichunk_to_bulkstat(ichunk);
	struct scan_inodes	*si = ichunk->si;
	struct xfs_bulkstat	*bs = &breq->bulkstat[0];
	struct xfs_inumbers	*inumbers = &ireq->inumbers[0];
	uint64_t		last_ino = 0;
	int			i;
	int			error;
	int			stale_count = 0;
	DEFINE_DESCR(dsc_bulkstat, ctx, render_ino_from_bulkstat);
	DEFINE_DESCR(dsc_inumbers, ctx, render_inumbers_from_agno);

	descr_set(&dsc_inumbers, &agno);
	handle_from_fshandle(&handle, ctx->fshandle, ctx->fshandle_len);
retry:
	bulkstat_for_inumbers(ctx, inumbers, breq);

	/* Iterate all the inodes. */
	for (i = 0; !si->aborted && i < breq->hdr.ocount; i++, bs++) {
		uint64_t	scan_ino = bs->bs_ino;

		/* ensure forward progress if we retried */
		if (scan_ino < last_ino)
			continue;

		descr_set(&dsc_bulkstat, bs);
		handle_from_bulkstat(&handle, bs);
		error = si->fn(ctx, &handle, bs, si->arg);
		switch (error) {
		case 0:
			break;
		case ESTALE: {
			stale_count++;
			if (stale_count < 30) {
				uint64_t	old_startino;

				ireq->hdr.ino = old_startino =
					inumbers->xi_startino;
				error = -xfrog_inumbers(&ctx->mnt, ireq);
				if (error)
					goto err;
				/*
				 * Retry only if inumbers returns the same
				 * inobt record as the previous record and
				 * there are allocated inodes in it.
				 */
				if (!si->aborted &&
				    ireq->hdr.ocount > 0 &&
				    inumbers->xi_alloccount > 0 &&
				    inumbers->xi_startino == old_startino)
					goto retry;
				goto out;
			}
			str_info(ctx, descr_render(&dsc_bulkstat),
_("Changed too many times during scan; giving up."));
			si->aborted = true;
			goto out;
		}
		case ECANCELED:
			error = 0;
			fallthrough;
		default:
			goto err;
		}
		if (scrub_excessive_errors(ctx)) {
			si->aborted = true;
			goto out;
		}
		last_ino = scan_ino;
	}

err:
	if (error) {
		str_liberror(ctx, error, descr_render(&dsc_bulkstat));
		si->aborted = true;
	}
out:
	free(ichunk);
}

/*
 * Call INUMBERS for information about inode chunks, then queue the inumbers
 * responses in the bulkstat workqueue.  This helps us maximize CPU parallelism
 * if the filesystem AGs are not evenly loaded.
 */
static void
scan_ag_inumbers(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct scan_ichunk	*ichunk = NULL;
	struct scan_inodes	*si = arg;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct xfs_inumbers_req	*ireq;
	uint64_t		nextino = cvt_agino_to_ino(&ctx->mnt, agno, 0);
	int			error;
	DEFINE_DESCR(dsc, ctx, render_inumbers_from_agno);

	descr_set(&dsc, &agno);

	error = alloc_ichunk(ctx, si, agno, 0, &ichunk);
	if (error)
		goto err;
	ireq = ichunk_to_inumbers(ichunk);

	/* Find the inode chunk & alloc mask */
	error = -xfrog_inumbers(&ctx->mnt, ireq);
	while (!error && !si->aborted && ireq->hdr.ocount > 0) {
		/*
		 * Make sure that we always make forward progress while we
		 * scan the inode btree.
		 */
		if (nextino > ireq->inumbers[0].xi_startino) {
			str_corrupt(ctx, descr_render(&dsc),
	_("AG %u inode btree is corrupt near agino %lu, got %lu"), agno,
				cvt_ino_to_agino(&ctx->mnt, nextino),
				cvt_ino_to_agino(&ctx->mnt,
						ireq->inumbers[0].xi_startino));
			si->aborted = true;
			break;
		}
		nextino = ireq->hdr.ino;

		if (ireq->inumbers[0].xi_alloccount == 0) {
			/*
			 * We can have totally empty inode chunks on
			 * filesystems where there are more than 64 inodes per
			 * block.  Skip these.
			 */
			;
		} else if (si->nr_threads > 0) {
			/* Queue this inode chunk on the bulkstat workqueue. */
			error = -workqueue_add(&si->wq_bulkstat,
					scan_ag_bulkstat, agno, ichunk);
			if (error) {
				si->aborted = true;
				str_liberror(ctx, error,
						_("queueing bulkstat work"));
				goto out;
			}
			ichunk = NULL;
		} else {
			/*
			 * Only one thread, call bulkstat directly.  Remember,
			 * ichunk is freed by the worker before returning.
			 */
			scan_ag_bulkstat(wq, agno, ichunk);
			ichunk = NULL;
			if (si->aborted)
				break;
		}

		if (!ichunk) {
			error = alloc_ichunk(ctx, si, agno, nextino, &ichunk);
			if (error)
				goto err;
		}
		ireq = ichunk_to_inumbers(ichunk);

		error = -xfrog_inumbers(&ctx->mnt, ireq);
	}

err:
	if (error) {
		str_liberror(ctx, error, descr_render(&dsc));
		si->aborted = true;
	}
out:
	if (ichunk)
		free(ichunk);
}

/*
 * Scan all the inodes in a filesystem, including metadata directory files and
 * broken files.  On error, this function will log an error message and return
 * -1.
 */
int
scrub_scan_all_inodes(
	struct scrub_ctx	*ctx,
	scrub_inode_iter_fn	fn,
	void			*arg)
{
	struct scan_inodes	si = {
		.fn		= fn,
		.arg		= arg,
		.nr_threads	= scrub_nproc_workqueue(ctx),
	};
	xfs_agnumber_t		agno;
	struct workqueue	wq_inumbers;
	unsigned int		max_bulkstat;
	int			ret;

	/*
	 * The bulkstat workqueue should queue at most one inobt block's worth
	 * of inode chunk records per worker thread.  If we're running in
	 * single thread mode (nr_threads==0) then we skip the workqueues.
	 */
	max_bulkstat = si.nr_threads * (ctx->mnt.fsgeom.blocksize / 16);

	ret = -workqueue_create_bound(&si.wq_bulkstat, (struct xfs_mount *)ctx,
			si.nr_threads, max_bulkstat);
	if (ret) {
		str_liberror(ctx, ret, _("creating bulkstat workqueue"));
		return -1;
	}

	ret = -workqueue_create(&wq_inumbers, (struct xfs_mount *)ctx,
			si.nr_threads);
	if (ret) {
		str_liberror(ctx, ret, _("creating inumbers workqueue"));
		si.aborted = true;
		goto kill_bulkstat;
	}

	for (agno = 0; agno < ctx->mnt.fsgeom.agcount; agno++) {
		ret = -workqueue_add(&wq_inumbers, scan_ag_inumbers, agno, &si);
		if (ret) {
			si.aborted = true;
			str_liberror(ctx, ret, _("queueing inumbers work"));
			break;
		}
	}

	ret = -workqueue_terminate(&wq_inumbers);
	if (ret) {
		si.aborted = true;
		str_liberror(ctx, ret, _("finishing inumbers work"));
	}
	workqueue_destroy(&wq_inumbers);

kill_bulkstat:
	ret = -workqueue_terminate(&si.wq_bulkstat);
	if (ret) {
		si.aborted = true;
		str_liberror(ctx, ret, _("finishing bulkstat work"));
	}
	workqueue_destroy(&si.wq_bulkstat);

	return si.aborted ? -1 : 0;
}

struct user_bulkstat {
	struct scan_inodes	*si;

	/* vla, must be last */
	struct xfs_bulkstat_req	breq;
};

/* Iterate all the user files returned by a bulkstat. */
static void
scan_user_files(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct xfs_handle	handle;
	struct scrub_ctx	*ctx = (struct scrub_ctx *)wq->wq_ctx;
	struct user_bulkstat	*ureq = arg;
	struct xfs_bulkstat	*bs = &ureq->breq.bulkstat[0];
	struct scan_inodes	*si = ureq->si;
	int			i;
	int			error = 0;
	DEFINE_DESCR(dsc_bulkstat, ctx, render_ino_from_bulkstat);

	handle_from_fshandle(&handle, ctx->fshandle, ctx->fshandle_len);

	for (i = 0; !si->aborted && i < ureq->breq.hdr.ocount; i++, bs++) {
		descr_set(&dsc_bulkstat, bs);
		handle_from_bulkstat(&handle, bs);
		error = si->fn(ctx, &handle, bs, si->arg);
		switch (error) {
		case 0:
			break;
		case ESTALE:
		case ECANCELED:
			error = 0;
			fallthrough;
		default:
			goto err;
		}
		if (scrub_excessive_errors(ctx)) {
			si->aborted = true;
			goto out;
		}
	}

err:
	if (error) {
		str_liberror(ctx, error, descr_render(&dsc_bulkstat));
		si->aborted = true;
	}
out:
	free(ureq);
}

/*
 * Run one step of the user files bulkstat scan and schedule background
 * processing of the stat data returned.  Returns 1 to keep going, or 0 to
 * stop.
 */
static int
scan_user_bulkstat(
	struct scrub_ctx	*ctx,
	struct scan_inodes	*si,
	uint64_t		*cursor)
{
	struct user_bulkstat	*ureq;
	const char		*what = NULL;
	int			ret;

	ureq = calloc(1, sizeof(struct user_bulkstat) +
			 XFS_BULKSTAT_REQ_SIZE(LIBFROG_BULKSTAT_CHUNKSIZE));
	if (!ureq) {
		ret = ENOMEM;
		what = _("creating bulkstat work item");
		goto err;
	}
	ureq->si = si;
	ureq->breq.hdr.icount = LIBFROG_BULKSTAT_CHUNKSIZE;
	ureq->breq.hdr.ino = *cursor;

	ret = -xfrog_bulkstat(&ctx->mnt, &ureq->breq);
	if (ret) {
		what = _("user files bulkstat");
		goto err_ureq;
	}
	if (ureq->breq.hdr.ocount == 0) {
		*cursor = NULLFSINO;
		free(ureq);
		return 0;
	}

	*cursor = ureq->breq.hdr.ino;

	/* scan_user_files frees ureq; do not access it */
	ret = -workqueue_add(&si->wq_bulkstat, scan_user_files, 0, ureq);
	if (ret) {
		what = _("queueing bulkstat work");
		goto err_ureq;
	}
	ureq = NULL;

	return 1;

err_ureq:
	free(ureq);
err:
	si->aborted = true;
	str_liberror(ctx, ret, what);
	return 0;
}

/*
 * Scan all the user files in a filesystem in inumber order.  On error, this
 * function will log an error message and return -1.
 */
int
scrub_scan_user_files(
	struct scrub_ctx	*ctx,
	scrub_inode_iter_fn	fn,
	void			*arg)
{
	struct scan_inodes	si = {
		.fn		= fn,
		.arg		= arg,
		.nr_threads	= scrub_nproc_workqueue(ctx),
	};
	uint64_t		ino = 0;
	int			ret;

	/* Queue up to four bulkstat result sets per thread. */
	ret = -workqueue_create_bound(&si.wq_bulkstat, (struct xfs_mount *)ctx,
			si.nr_threads, si.nr_threads * 4);
	if (ret) {
		str_liberror(ctx, ret, _("creating bulkstat workqueue"));
		return -1;
	}

	while ((ret = scan_user_bulkstat(ctx, &si, &ino)) == 1) {
		/* empty */
	}

	ret = -workqueue_terminate(&si.wq_bulkstat);
	if (ret) {
		si.aborted = true;
		str_liberror(ctx, ret, _("finishing bulkstat work"));
	}
	workqueue_destroy(&si.wq_bulkstat);

	return si.aborted ? -1 : 0;
}

/* Open a file by handle, returning either the fd or -1 on error. */
int
scrub_open_handle(
	struct xfs_handle	*handle)
{
	return open_by_fshandle(handle, sizeof(*handle),
			O_RDONLY | O_NOATIME | O_NOFOLLOW | O_NOCTTY);
}
