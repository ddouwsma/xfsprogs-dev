// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2017-2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "platform_defs.h"
#include "xfs.h"
#include "xfs_arch.h"
#include "list.h"
#include "paths.h"
#include "handle.h"
#include "libfrog/getparents.h"

/* Allocate a buffer for the xfs_getparent_rec array. */
static void *
alloc_records(
	struct xfs_getparents	*gp,
	size_t			bufsize)
{
	void			*buf;

	if (bufsize >= UINT32_MAX) {
		errno = ENOMEM;
		return NULL;
	} else if (!bufsize) {
		bufsize = XFS_XATTR_LIST_MAX;
	}

	buf = malloc(bufsize);
	if (!buf)
		return NULL;

	gp->gp_buffer = (uintptr_t)buf;
	gp->gp_bufsize = bufsize;
	return buf;
}

/* Copy a file handle. */
static inline void
copy_handle(
	struct xfs_handle	*dest,
	const struct xfs_handle	*src)
{
	memcpy(dest, src, sizeof(struct xfs_handle));
}

/* Initiate a callback for each parent pointer. */
static int
walk_parent_records(
	struct xfs_getparents	*gp,
	walk_parent_fn		fn,
	void			*arg)
{
	struct xfs_getparents_rec *gpr;
	int			ret;

	if (gp->gp_oflags & XFS_GETPARENTS_OFLAG_ROOT) {
		struct parent_rec	rec = {
			.p_flags	= PARENTREC_FILE_IS_ROOT,
		};

		return fn(&rec, arg);
	}

	for (gpr = xfs_getparents_first_rec(gp);
	     gpr != NULL;
	     gpr = xfs_getparents_next_rec(gp, gpr)) {
		struct parent_rec	rec = { };

		if (gpr->gpr_name[0] == 0)
			break;

		copy_handle(&rec.p_handle, &gpr->gpr_parent);
		rec.p_name = gpr->gpr_name;

		ret = fn(&rec, arg);
		if (ret)
			return ret;
	}

	return 0;
}

/* Walk all parent pointers of this fd.  Returns 0 or positive errno. */
int
fd_walk_parents(
	int			fd,
	size_t			bufsize,
	walk_parent_fn		fn,
	void			*arg)
{
	struct xfs_getparents	gp = { };
	void			*buf;
	int			ret;

	buf = alloc_records(&gp, bufsize);
	if (!buf)
		return errno;

	while ((ret = ioctl(fd, XFS_IOC_GETPARENTS, &gp)) == 0) {
		ret = walk_parent_records(&gp, fn, arg);
		if (ret)
			goto out_buf;
		if (gp.gp_oflags & XFS_GETPARENTS_OFLAG_DONE)
			break;
	}
	if (ret)
		ret = errno;

out_buf:
	free(buf);
	return ret;
}

/* Walk all parent pointers of this handle.  Returns 0 or positive errno. */
int
handle_walk_parents(
	const void		*hanp,
	size_t			hlen,
	size_t			bufsize,
	walk_parent_fn		fn,
	void			*arg)
{
	struct xfs_getparents_by_handle	gph = { };
	void			*buf;
	char			*mntpt;
	int			fd;
	int			ret;

	if (hlen != sizeof(struct xfs_handle))
		return EINVAL;

	/*
	 * This function doesn't modify the handle, but we don't want to have
	 * to bump the libhandle major version just to change that.
	 */
	fd = handle_to_fsfd((void *)hanp, &mntpt);
	if (fd < 0)
		return errno;

	buf = alloc_records(&gph.gph_request, bufsize);
	if (!buf)
		return errno;

	copy_handle(&gph.gph_handle, hanp);
	while ((ret = ioctl(fd, XFS_IOC_GETPARENTS_BY_HANDLE, &gph)) == 0) {
		ret = walk_parent_records(&gph.gph_request, fn, arg);
		if (ret)
			goto out_buf;
		if (gph.gph_request.gp_oflags & XFS_GETPARENTS_OFLAG_DONE)
			break;
	}
	if (ret)
		ret = errno;

out_buf:
	free(buf);
	return ret;
}

struct walk_ppaths_info {
	/* Callback */
	walk_path_fn		fn;
	void			*arg;

	/* Mountpoint of this filesystem. */
	char			*mntpt;

	/* Path that we're constructing. */
	struct path_list	*path;

	size_t			ioctl_bufsize;
};

/*
 * Recursively walk upwards through the directory tree, changing out the path
 * components as needed.  Call the callback when we have a complete path.
 */
static int
find_parent_component(
	const struct parent_rec	*rec,
	void			*arg)
{
	struct walk_ppaths_info	*wpi = arg;
	struct path_component	*pc;
	int			ret;

	if (rec->p_flags & PARENTREC_FILE_IS_ROOT)
		return wpi->fn(wpi->mntpt, wpi->path, wpi->arg);

	/*
	 * If we detect a directory tree cycle, give up.  We never made any
	 * guarantees about concurrent tree updates.
	 */
	if (path_will_loop(wpi->path, rec->p_handle.ha_fid.fid_ino))
		return 0;

	pc = path_component_init(rec->p_name, rec->p_handle.ha_fid.fid_ino);
	if (!pc)
		return errno;
	path_list_add_parent_component(wpi->path, pc);

	ret = handle_walk_parents(&rec->p_handle, sizeof(rec->p_handle),
			wpi->ioctl_bufsize, find_parent_component, wpi);

	path_list_del_component(wpi->path, pc);
	path_component_free(pc);
	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * described in the handle.  Returns 0 for success or positive errno.
 */
int
handle_walk_paths(
	const void		*hanp,
	size_t			hlen,
	size_t			ioctl_bufsize,
	walk_path_fn		fn,
	void			*arg)
{
	struct walk_ppaths_info	wpi = {
		.ioctl_bufsize	= ioctl_bufsize,
	};
	int			ret;

	/*
	 * This function doesn't modify the handle, but we don't want to have
	 * to bump the libhandle major version just to change that.
	 */
	ret = handle_to_fsfd((void *)hanp, &wpi.mntpt);
	if (ret < 0)
		return errno;

	wpi.path = path_list_init();
	if (!wpi.path)
		return errno;
	wpi.fn = fn;
	wpi.arg = arg;

	ret = handle_walk_parents(hanp, hlen, ioctl_bufsize,
			find_parent_component, &wpi);

	path_list_free(wpi.path);
	return ret;
}

/*
 * Call the given function on all known paths from the vfs root to the inode
 * referred to by the file description.  Returns 0 or positive errno.
 */
int
fd_walk_paths(
	int			fd,
	size_t			ioctl_bufsize,
	walk_path_fn		fn,
	void			*arg)
{
	void			*hanp;
	size_t			hlen;
	int			ret;

	ret = fd_to_handle(fd, &hanp, &hlen);
	if (ret)
		return errno;

	ret = handle_walk_paths(hanp, hlen, ioctl_bufsize, fn, arg);
	free_handle(hanp, hlen);
	return ret;
}

struct gather_path_info {
	char			*buf;
	size_t			len;
	size_t			written;
};

/* Helper that stringifies the first full path that we find. */
static int
path_to_string(
	const char		*mntpt,
	const struct path_list	*path,
	void			*arg)
{
	struct gather_path_info	*gpi = arg;
	int			mntpt_len = strlen(mntpt);
	int			ret;

	/* Trim trailing slashes from the mountpoint */
	while (mntpt_len > 0 && mntpt[mntpt_len - 1] == '/')
		mntpt_len--;

	ret = snprintf(gpi->buf, gpi->len, "%.*s", mntpt_len, mntpt);
	if (ret != mntpt_len)
		return ENAMETOOLONG;
	gpi->written += ret;

	ret = path_list_to_string(path, gpi->buf + ret, gpi->len - ret);
	if (ret < 0)
		return ENAMETOOLONG;

	gpi->written += ret;
	return ECANCELED;
}

/*
 * Return any eligible path to this file handle.  Returns 0 for success or
 * positive errno.
 */
int
handle_to_path(
	const void		*hanp,
	size_t			hlen,
	size_t			ioctl_bufsize,
	char			*path,
	size_t			pathlen)
{
	struct gather_path_info	gpi = { .buf = path, .len = pathlen };
	int			ret;

	ret = handle_walk_paths(hanp, hlen, ioctl_bufsize, path_to_string,
			&gpi);
	if (ret && ret != ECANCELED)
		return ret;
	if (!gpi.written)
		return ENODATA;

	path[gpi.written] = 0;
	return 0;
}

/*
 * Return any eligible path to this file description.  Returns 0 for success
 * or positive errno.
 */
int
fd_to_path(
	int			fd,
	size_t			ioctl_bufsize,
	char			*path,
	size_t			pathlen)
{
	struct gather_path_info	gpi = { .buf = path, .len = pathlen };
	int			ret;

	ret = fd_walk_paths(fd, ioctl_bufsize, path_to_string, &gpi);
	if (ret && ret != ECANCELED)
		return ret;
	if (!gpi.written)
		return ENODATA;

	path[gpi.written] = 0;
	return 0;
}
