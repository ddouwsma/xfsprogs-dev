// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "handle.h"
#include "libfrog/fsgeom.h"
#include "libfrog/paths.h"
#include "libfrog/bulkstat.h"
#include "libfrog/fsprops.h"
#include "libfrog/fsproperties.h"

#include <attr/attributes.h>

/*
 * Given an xfd and a mount table path, get us the handle for the root dir so
 * we can set filesystem properties.
 */
int
fsprops_open_handle(
	struct xfs_fd		*xfd,
	struct fs_path		*fs_path,
	struct fsprops_handle	*fph)
{
	struct xfs_bulkstat	bulkstat;
	struct stat		sb;
	int			ret;

	/* fs properties only supported on V5 filesystems */
	if (!(xfd->fsgeom.flags & XFS_FSOP_GEOM_FLAGS_V5SB)) {
		errno = EOPNOTSUPP;
		return -1;
	}

	ret = -xfrog_bulkstat_single(xfd, XFS_BULK_IREQ_SPECIAL_ROOT,
			XFS_BULK_IREQ_SPECIAL, &bulkstat);
	if (ret)
		return ret;

	ret = fstat(xfd->fd, &sb);
	if (ret)
		return ret;

	if (sb.st_ino != bulkstat.bs_ino) {
		errno = ESRMNT;
		return -1;
	}

	return fd_to_handle(xfd->fd, &fph->hanp, &fph->hlen);
}

/* Free a fsproperties handle. */
void
fsprops_free_handle(
	struct fsprops_handle	*fph)
{
	if (fph->hanp)
		free_handle(fph->hanp, fph->hlen);
	fph->hanp = NULL;
	fph->hlen = 0;
}

/* Call the given callback on every fs property accessible through the handle. */
int
fsprops_walk_names(
	struct fsprops_handle	*fph,
	fsprops_name_walk_fn	walk_fn,
	void			*priv)
{
	struct attrlist_cursor	cur = { };
	char			attrbuf[XFS_XATTR_LIST_MAX];
	struct attrlist		*attrlist = (struct attrlist *)attrbuf;
	int			ret;

	memset(attrbuf, 0, XFS_XATTR_LIST_MAX);

	while ((ret = attr_list_by_handle(fph->hanp, fph->hlen, attrbuf,
				XFS_XATTR_LIST_MAX, XFS_IOC_ATTR_ROOT,
				&cur)) == 0) {
		unsigned int	i;

		for (i = 0; i < attrlist->al_count; i++) {
			struct attrlist_ent	*ent = ATTR_ENTRY(attrlist, i);
			const char		*p =
				attr_name_to_fsprop_name(ent->a_name);

			if (p) {
				ret = walk_fn(fph, p, ent->a_valuelen, priv);
				if (ret)
					break;
			}
		}

		if (!attrlist->al_more)
			break;
	}

	return ret;
}

/* Retrieve the value of a specific fileystem property. */
int
fsprops_get(
	struct fsprops_handle	*fph,
	const char		*name,
	void			*valuebuf,
	size_t			*valuelen)
{
	struct xfs_attr_multiop	ops = {
		.am_opcode	= ATTR_OP_GET,
		.am_attrvalue	= valuebuf,
		.am_length	= *valuelen,
		.am_flags	= XFS_IOC_ATTR_ROOT,
	};
	int			ret;

	ret = fsprop_name_to_attr_name(name, (char **)&ops.am_attrname);
	if (ret < 0)
		return ret;

	ret = attr_multi_by_handle(fph->hanp, fph->hlen, &ops, 1, 0);
	if (ret < 0)
		goto out_name;

	if (ops.am_error) {
		errno = -ops.am_error;
		ret = -1;
		goto out_name;
	}

	*valuelen = ops.am_length;
out_name:
	free(ops.am_attrname);
	return ret;
}

/* Set the value of a specific fileystem property. */
int
fsprops_set(
	struct fsprops_handle	*fph,
	const char		*name,
	void			*valuebuf,
	size_t			valuelen)
{
	struct xfs_attr_multiop	ops = {
		.am_opcode	= ATTR_OP_SET,
		.am_attrvalue	= valuebuf,
		.am_length	= valuelen,
		.am_flags	= XFS_IOC_ATTR_ROOT,
	};
	int			ret;

	ret = fsprop_name_to_attr_name(name, (char **)&ops.am_attrname);
	if (ret < 0)
		return ret;

	ret = attr_multi_by_handle(fph->hanp, fph->hlen, &ops, 1, 0);
	if (ret < 0)
		goto out_name;

	if (ops.am_error) {
		errno = -ops.am_error;
		ret = -1;
		goto out_name;
	}

out_name:
	free(ops.am_attrname);
	return ret;
}

/* Unset the value of a specific fileystem property. */
int
fsprops_remove(
	struct fsprops_handle	*fph,
	const char		*name)
{
	struct xfs_attr_multiop	ops = {
		.am_opcode	= ATTR_OP_REMOVE,
		.am_flags	= XFS_IOC_ATTR_ROOT,
	};
	int			ret;

	ret = fsprop_name_to_attr_name(name, (char **)&ops.am_attrname);
	if (ret < 0)
		return ret;

	ret = attr_multi_by_handle(fph->hanp, fph->hlen, &ops, 1, 0);
	if (ret < 0)
		goto out_name;

	if (ops.am_error) {
		errno = -ops.am_error;
		ret = -1;
		goto out_name;
	}

out_name:
	free(ops.am_attrname);
	return ret;
}
