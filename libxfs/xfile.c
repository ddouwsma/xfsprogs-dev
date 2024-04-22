// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs_priv.h"
#include "libxfs.h"
#include "libxfs/xfile.h"
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Swappable Temporary Memory
 * ==========================
 *
 * Offline checking sometimes needs to be able to stage a large amount of data
 * in memory.  This information might not fit in the available memory and it
 * doesn't all need to be accessible at all times.  In other words, we want an
 * indexed data buffer to store data that can be paged out.
 *
 * memfd files meet those requirements.  Therefore, the xfile mechanism uses
 * one to store our staging data.  The xfile must be freed with xfile_destroy.
 *
 * xfiles assume that the caller will handle all required concurrency
 * management; file locks are not taken.
 */

/*
 * Starting with Linux 6.3, there's a new MFD_NOEXEC_SEAL flag that disables
 * the longstanding memfd behavior that files are created with the executable
 * bit set, and seals the file against it being turned back on.
 */
#ifndef MFD_NOEXEC_SEAL
# define MFD_NOEXEC_SEAL	(0x0008U)
#endif

/*
 * Open a memory-backed fd to back an xfile.  We require close-on-exec here,
 * because these memfd files function as windowed RAM and hence should never
 * be shared with other processes.
 */
static int
xfile_create_fd(
	const char		*description)
{
	int			fd = -1;
	int			ret;

	/*
	 * memfd_create was added to kernel 3.17 (2014).  MFD_NOEXEC_SEAL
	 * causes -EINVAL on old kernels, so fall back to omitting it so that
	 * new xfs_repair can run on an older recovery cd kernel.
	 */
	fd = memfd_create(description, MFD_CLOEXEC | MFD_NOEXEC_SEAL);
	if (fd >= 0)
		goto got_fd;
	fd = memfd_create(description, MFD_CLOEXEC);
	if (fd >= 0)
		goto got_fd;

	/*
	 * O_TMPFILE exists as of kernel 3.11 (2013), which means that if we
	 * find it, we're pretty safe in assuming O_CLOEXEC exists too.
	 */
	fd = open("/dev/shm", O_TMPFILE | O_CLOEXEC | O_RDWR, 0600);
	if (fd >= 0)
		goto got_fd;

	fd = open("/tmp", O_TMPFILE | O_CLOEXEC | O_RDWR, 0600);
	if (fd >= 0)
		goto got_fd;

	/*
	 * mkostemp exists as of glibc 2.7 (2007) and O_CLOEXEC exists as of
	 * kernel 2.6.23 (2007).
	 */
	fd = mkostemp("libxfsXXXXXX", O_CLOEXEC);
	if (fd >= 0)
		goto got_fd;

	if (!errno)
		errno = EOPNOTSUPP;
	return -1;
got_fd:
	/*
	 * Turn off mode bits we don't want -- group members and others should
	 * not have access to the xfile, nor it be executable.  memfds are
	 * created with mode 0777, but we'll be careful just in case the other
	 * implementations fail to set 0600.
	 */
	ret = fchmod(fd, 0600);
	if (ret)
		perror("disabling xfile executable bit");

	return fd;
}

/*
 * Create an xfile of the given size.  The description will be used in the
 * trace output.
 */
int
xfile_create(
	const char		*description,
	struct xfile		**xfilep)
{
	struct xfile		*xf;
	int			error;

	xf = kmalloc(sizeof(struct xfile), 0);
	if (!xf)
		return -ENOMEM;

	xf->fd = xfile_create_fd(description);
	if (xf->fd < 0) {
		error = -errno;
		kfree(xf);
		return error;
	}

	*xfilep = xf;
	return 0;
}

/* Close the file and release all resources. */
void
xfile_destroy(
	struct xfile		*xf)
{
	close(xf->fd);
	kfree(xf);
}

static inline loff_t
xfile_maxbytes(
	struct xfile		*xf)
{
	if (sizeof(loff_t) == 8)
		return LLONG_MAX;
	return LONG_MAX;
}

/*
 * Load an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
ssize_t
xfile_load(
	struct xfile		*xf,
	void			*buf,
	size_t			count,
	loff_t			pos)
{
	ssize_t			ret;

	if (count > INT_MAX)
		return -ENOMEM;
	if (xfile_maxbytes(xf) - pos < count)
		return -ENOMEM;

	ret = pread(xf->fd, buf, count, pos);
	if (ret < 0)
		return -errno;
	if (ret != count)
		return -ENOMEM;
	return 0;
}

/*
 * Store an object.  Since we're treating this file as "memory", any error or
 * short IO is treated as a failure to allocate memory.
 */
ssize_t
xfile_store(
	struct xfile		*xf,
	const void		*buf,
	size_t			count,
	loff_t			pos)
{
	ssize_t			ret;

	if (count > INT_MAX)
		return -E2BIG;
	if (xfile_maxbytes(xf) - pos < count)
		return -EFBIG;

	ret = pwrite(xf->fd, buf, count, pos);
	if (ret < 0)
		return -errno;
	if (ret != count)
		return -ENOMEM;
	return 0;
}

/* Compute the number of bytes used by a xfile. */
unsigned long long
xfile_bytes(
	struct xfile		*xf)
{
	struct stat		statbuf;
	int			error;

	error = fstat(xf->fd, &statbuf);
	if (error)
		return -errno;

	return (unsigned long long)statbuf.st_blocks << 9;
}
