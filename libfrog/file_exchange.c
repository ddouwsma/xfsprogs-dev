// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2020-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include "xfs.h"
#include "fsgeom.h"
#include "bulkstat.h"
#include "libfrog/file_exchange.h"

/* Prepare for a file contents exchange. */
void
xfrog_exchangerange_prep(
	struct xfs_exchange_range	*fxr,
	off_t				file2_offset,
	int				file1_fd,
	off_t				file1_offset,
	uint64_t			length)
{
	memset(fxr, 0, sizeof(*fxr));

	fxr->file1_fd			= file1_fd;
	fxr->file1_offset		= file1_offset;
	fxr->length			= length;
	fxr->file2_offset		= file2_offset;
}

/*
 * Execute an exchange-range operation.  Returns 0 for success or a negative
 * errno.
 */
int
xfrog_exchangerange(
	int				file2_fd,
	struct xfs_exchange_range	*fxr,
	uint64_t			flags)
{
	int				ret;

	fxr->flags = flags;

	ret = ioctl(file2_fd, XFS_IOC_EXCHANGE_RANGE, fxr);
	if (ret)
		return -errno;

	return 0;
}
