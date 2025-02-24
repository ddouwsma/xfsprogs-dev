// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 Oracle.  All Rights Reserved.
 * Author: Catherine Hoang <catherine.hoang@oracle.com>
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "libxfs.h"
#include "command.h"
#include "output.h"
#include "init.h"
#include "io.h"
#include "namei.h"
#include "type.h"
#include "input.h"
#include "faddr.h"
#include "fprint.h"
#include "field.h"
#include "inode.h"
#include "listxattr.h"
#include <sys/xattr.h>
#include <linux/xattr.h>

static bool strict_errors;

/* file attributes that we might have lost */
#define LOST_OWNER		(1U << 0)
#define LOST_MODE		(1U << 1)
#define LOST_TIME		(1U << 2)
#define LOST_SOME_FSXATTR	(1U << 3)
#define LOST_FSXATTR		(1U << 4)
#define LOST_XATTR		(1U << 5)
#define LOST_ACL		(1U << 6)

static unsigned int lost_mask;

static void
rdump_help(void)
{
	dbprintf(_(
"\n"
" Recover files out of the filesystem into a directory.\n"
"\n"
" Options:\n"
"   -s      -- Fail on errors when reading content from the filesystem.\n"
"   paths   -- Copy only these paths.  If no paths are given, copy everything.\n"
"   destdir -- The destination into which files are recovered.\n"
	));
}

struct destdir {
	int	fd;
	char	*path;
	char	*sep;
};

struct pathbuf {
	size_t	len;
	char	path[PATH_MAX + 1];
};

static int rdump_file(struct xfs_trans *tp, xfs_ino_t ino,
		const struct destdir *destdir, struct pathbuf *pbuf);

static inline unsigned int xflags2getflags(const struct fsxattr *fa)
{
	unsigned int	ret = 0;

	if (fa->fsx_xflags & FS_XFLAG_IMMUTABLE)
		ret |= FS_IMMUTABLE_FL;
	if (fa->fsx_xflags & FS_XFLAG_APPEND)
		ret |= FS_APPEND_FL;
	if (fa->fsx_xflags & FS_XFLAG_SYNC)
		ret |= FS_SYNC_FL;
	if (fa->fsx_xflags & FS_XFLAG_NOATIME)
		ret |= FS_NOATIME_FL;
	if (fa->fsx_xflags & FS_XFLAG_NODUMP)
		ret |= FS_NODUMP_FL;
	if (fa->fsx_xflags & FS_XFLAG_DAX)
		ret |= FS_DAX_FL;
	if (fa->fsx_xflags & FS_XFLAG_PROJINHERIT)
		ret |= FS_PROJINHERIT_FL;
	return ret;
}

/* Copy common file attributes to this fd */
static int
rdump_fileattrs_fd(
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf,
	int			fd)
{
	struct fsxattr		fsxattr = {
		.fsx_extsize	= ip->i_extsize,
		.fsx_projid	= ip->i_projid,
		.fsx_cowextsize	= ip->i_cowextsize,
		.fsx_xflags	= xfs_ip2xflags(ip),
	};
	int			ret;

	ret = fchmod(fd, VFS_I(ip)->i_mode & ~S_IFMT);
	if (ret) {
		if (errno == EPERM)
			lost_mask |= LOST_MODE;
		else
			dbprintf(_("%s%s%s: fchmod %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	ret = fchown(fd, i_uid_read(VFS_I(ip)), i_gid_read(VFS_I(ip)));
	if (ret) {
		if (errno == EPERM)
			lost_mask |= LOST_OWNER;
		else
			dbprintf(_("%s%s%s: fchown %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	ret = ioctl(fd, XFS_IOC_FSSETXATTR, &fsxattr);
	if (ret) {
		unsigned int	getflags = xflags2getflags(&fsxattr);

		/* try to use setflags if the target is not xfs */
		if (errno == EOPNOTSUPP || errno == ENOTTY) {
			lost_mask |= LOST_SOME_FSXATTR;
			ret = ioctl(fd, FS_IOC_SETFLAGS, &getflags);
		}

		if (errno == EOPNOTSUPP || errno == EPERM || errno == ENOTTY)
			lost_mask |= LOST_FSXATTR;
		else
			dbprintf(_("%s%s%s: fssetxattr %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	return 0;
}

/* Copy common file attributes to this path */
static int
rdump_fileattrs_path(
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf)
{
	int			ret;

	ret = fchmodat(destdir->fd, pbuf->path, VFS_I(ip)->i_mode & ~S_IFMT,
			AT_SYMLINK_NOFOLLOW);
	if (ret) {
		/* fchmodat on a symlink is not supported */
		if (errno == EPERM || errno == EOPNOTSUPP)
			lost_mask |= LOST_MODE;
		else
			dbprintf(_("%s%s%s: fchmodat %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	ret = fchownat(destdir->fd, pbuf->path, i_uid_read(VFS_I(ip)),
			i_gid_read(VFS_I(ip)), AT_SYMLINK_NOFOLLOW);
	if (ret) {
		if (errno == EPERM)
			lost_mask |= LOST_OWNER;
		else
			dbprintf(_("%s%s%s: fchownat %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	/* Cannot copy fsxattrs until setfsxattrat gets merged */

	return 0;
}

/* Copy access and modification timestamps to this fd. */
static int
rdump_timestamps_fd(
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf,
	int			fd)
{
	struct timespec		times[2] = {
		[0] = {
			.tv_sec  = inode_get_atime_sec(VFS_I(ip)),
			.tv_nsec = inode_get_atime_nsec(VFS_I(ip)),
		},
		[1] = {
			.tv_sec  = inode_get_mtime_sec(VFS_I(ip)),
			.tv_nsec = inode_get_mtime_nsec(VFS_I(ip)),
		},
	};
	int			ret;

	/* Cannot set ctime or btime */

	ret = futimens(fd, times);
	if (ret) {
		if (errno == EPERM)
			lost_mask |= LOST_TIME;
		else
			dbprintf(_("%s%s%s: futimens %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	return 0;
}

/* Copy access and modification timestamps to this path. */
static int
rdump_timestamps_path(
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf)
{
	struct timespec		times[2] = {
		[0] = {
			.tv_sec  = inode_get_atime_sec(VFS_I(ip)),
			.tv_nsec = inode_get_atime_nsec(VFS_I(ip)),
		},
		[1] = {
			.tv_sec  = inode_get_mtime_sec(VFS_I(ip)),
			.tv_nsec = inode_get_mtime_nsec(VFS_I(ip)),
		},
	};
	int			ret;

	/* Cannot set ctime or btime */

	ret = utimensat(destdir->fd, pbuf->path, times, AT_SYMLINK_NOFOLLOW);
	if (ret) {
		if (errno == EPERM)
			lost_mask |= LOST_TIME;
		else
			dbprintf(_("%s%s%s: utimensat %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		if (strict_errors)
			return 1;
	}

	return 0;
}

struct copyxattr {
	const struct destdir	*destdir;
	const struct pathbuf	*pbuf;
	int			fd;
	char			name[XATTR_NAME_MAX +
				     XATTR_SECURITY_PREFIX_LEN + 1];
	char			value[XATTR_SIZE_MAX];
};

/*
 * ACL xattrs can be copied verbatim to another XFS filesystem without issue
 * because the name of the ondisk xattr is not that of the magic POSIX ACL
 * xattr.  However, if we're rdumping to another filesystem type, we need to
 * warn the user that the ACLs most likely won't work because we don't want to
 * introduce a hard dep on libacl to perform a translation for a foreign fs.
 */
static inline bool
cannot_translate_acl(
	int			fd,
	const char		*name,
	unsigned int		namelen)
{
	struct statfs		statfsbuf;
	int			ret;

	if (namelen == SGI_ACL_FILE_SIZE &&
	    !strncmp(name, SGI_ACL_FILE, SGI_ACL_FILE_SIZE))
		return false;

	if (namelen == SGI_ACL_DEFAULT_SIZE &&
	    !strncmp(name, SGI_ACL_DEFAULT, SGI_ACL_DEFAULT_SIZE))
		return false;

	ret = fstatfs(fd, &statfsbuf);
	if (ret)
		return false;

	return statfsbuf.f_type != XFS_SUPER_MAGIC;
}

/* Copy one extended attribute */
static int
rdump_xattr(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct copyxattr	*cx = priv;
	const char		*namespace;
	char			*realname = (char *)name;
	const size_t		remaining = sizeof(cx->name);
	ssize_t			added;
	int			realnamelen = namelen;
	int			ret;

	switch (attr_flags & XFS_ATTR_NSP_ONDISK_MASK) {
	case XFS_ATTR_PARENT:
		return 0;
	case XFS_ATTR_ROOT:
		namespace = XATTR_TRUSTED_PREFIX;
		if (!(lost_mask & LOST_ACL) &&
		    cannot_translate_acl(cx->fd, (const char *)name, namelen))
			lost_mask |= LOST_ACL;
		break;
	case XFS_ATTR_SECURE:
		namespace = XATTR_SECURITY_PREFIX;
		break;
	case 0: /* user xattrs */
		namespace = XATTR_USER_PREFIX;
		break;
	default:
		dbprintf(_("%s%s%s: unknown xattr namespace 0x%x\n"),
				cx->destdir->path, cx->destdir->sep,
				cx->pbuf->path,
				attr_flags & XFS_ATTR_NSP_ONDISK_MASK);
		return strict_errors ? ECANCELED : 0;
	}
	added = snprintf(cx->name, remaining, "%s%.*s", namespace,
			realnamelen, realname);
	if (added > remaining) {
		dbprintf(
 _("%s%s%s: ran out of space formatting xattr name %s%.*s\n"),
				cx->destdir->path, cx->destdir->sep,
				cx->pbuf->path, namespace, realnamelen,
				realname);
		return strict_errors ? ECANCELED : 0;
	}

	/* Retrieve xattr value if needed */
	if (valuelen > 0 && !value) {
		struct xfs_da_args	args = {
			.trans		= tp,
			.dp		= ip,
			.geo		= mp->m_attr_geo,
			.owner		= ip->i_ino,
			.attr_filter	= attr_flags & XFS_ATTR_NSP_ONDISK_MASK,
			.namelen	= namelen,
			.name		= name,
			.value		= cx->value,
			.valuelen	= valuelen,
		};

		ret = -libxfs_attr_rmtval_get(&args);
		if (ret) {
			dbprintf(_("%s: reading xattr \"%s\" value %s\n"),
					cx->pbuf->path, cx->name,
					strerror(ret));
			return strict_errors ? ECANCELED : 0;
		}

		value = cx->value;
	}

	ret = fsetxattr(cx->fd, cx->name, value, valuelen, 0);
	if (ret) {
		if (ret == EOPNOTSUPP)
			lost_mask |= LOST_XATTR;
		else
			dbprintf(_("%s%s%s: fsetxattr \"%s\" %s\n"),
					cx->destdir->path, cx->destdir->sep,
					cx->pbuf->path, cx->name,
					strerror(errno));
		if (strict_errors)
			return ECANCELED;
	}

	return 0;
}

/* Copy extended attributes */
static int
rdump_xattrs(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf,
	int			fd)
{
	struct copyxattr	*cx;
	int			ret;

	cx = calloc(1, sizeof(struct copyxattr));
	if (!cx) {
		dbprintf(_("%s%s%s: allocating xattr buffer %s\n"),
				destdir->path, destdir->sep, pbuf->path,
				strerror(errno));
		return 1;
	}
	cx->destdir = destdir;
	cx->pbuf = pbuf;
	cx->fd = fd;

	ret = xattr_walk(tp, ip, rdump_xattr, cx);
	if (ret && ret != ECANCELED) {
		dbprintf(_("%s%s%s: listxattr %s\n"), destdir->path,
				destdir->sep, pbuf->path, strerror(errno));
		if (strict_errors)
			return 1;
	}

	free(cx);
	return 0;
}

struct copydirent {
	const struct destdir	*destdir;
	struct pathbuf		*pbuf;
	int			fd;
};

/* Copy a directory entry. */
static int
rdump_dirent(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	xfs_dir2_dataptr_t	off,
	char			*name,
	ssize_t			namelen,
	xfs_ino_t		ino,
	uint8_t			dtype,
	void			*private)
{
	struct copydirent	*cd = private;
	size_t			oldlen = cd->pbuf->len;
	const size_t		remaining = PATH_MAX + 1 - oldlen;
	ssize_t			added;
	int			ret;

	/* Negative length means name is null-terminated */
	if (namelen < 0)
		namelen = -namelen;

	/* Ignore dot and dotdot */
	if (namelen == 1 && name[0] == '.')
		return 0;
	if (namelen == 2 && name[0] == '.' && name[1] == '.')
		return 0;

	if (namelen > FILENAME_MAX) {
		dbprintf(_("%s%s%s: %s\n"),
				cd->destdir->path, cd->destdir->sep,
				cd->pbuf->path, strerror(ENAMETOOLONG));
		return strict_errors ? ECANCELED : 0;
	}

	added = snprintf(&cd->pbuf->path[oldlen], remaining, "%s%.*s",
			oldlen ? "/" : "", (int)namelen, name);
	if (added > remaining) {
		dbprintf(_("%s%s%s: ran out of space formatting file name\n"),
				cd->destdir->path, cd->destdir->sep,
				cd->pbuf->path);
		cd->pbuf->path[oldlen] = 0;
		return strict_errors ? ECANCELED : 0;
	}

	cd->pbuf->len += added;
	ret = rdump_file(tp, ino, cd->destdir, cd->pbuf);
	cd->pbuf->len = oldlen;
	cd->pbuf->path[oldlen] = 0;
	return ret;
}

/* Copy a directory */
static int
rdump_directory(
	struct xfs_trans	*tp,
	struct xfs_inode	*dp,
	const struct destdir	*destdir,
	struct pathbuf		*pbuf)
{
	struct copydirent	*cd;
	int			ret, ret2;

	cd = calloc(1, sizeof(struct copydirent));
	if (!cd) {
		dbprintf(_("%s%s%s: %s\n"), destdir->path, destdir->sep,
				pbuf->path, strerror(errno));
		return 1;
	}
	cd->destdir = destdir;
	cd->pbuf = pbuf;

	if (pbuf->len) {
		/*
		 * If path is non-empty, we want to create a child somewhere
		 * underneath the target directory.
		 */
		ret = mkdirat(destdir->fd, pbuf->path, 0600);
		if (ret && errno != EEXIST) {
			dbprintf(_("%s%s%s: %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
			goto out_cd;
		}

		cd->fd = openat(destdir->fd, pbuf->path,
				O_RDONLY | O_DIRECTORY);
		if (cd->fd < 0) {
			dbprintf(_("%s%s%s: %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
			ret = 1;
			goto out_cd;
		}
	} else {
		/*
		 * If path is empty, then we're copying the children of a
		 * directory into the target directory.
		 */
		cd->fd = destdir->fd;
	}

	ret = rdump_fileattrs_fd(dp, destdir, pbuf, cd->fd);
	if (ret && strict_errors)
		goto out_close;

	if (xfs_inode_has_attr_fork(dp)) {
		ret = rdump_xattrs(tp, dp, destdir, pbuf, cd->fd);
		if (ret && strict_errors)
			goto out_close;
	}

	ret = listdir(tp, dp, rdump_dirent, cd);
	if (ret && ret != ECANCELED) {
		dbprintf(_("%s%s%s: readdir %s\n"), destdir->path,
				destdir->sep, pbuf->path, strerror(ret));
		if (strict_errors)
			goto out_close;
	}

	ret = rdump_timestamps_fd(dp, destdir, pbuf, cd->fd);
	if (ret && strict_errors)
		goto out_close;

	ret = 0;

out_close:
	if (cd->fd != destdir->fd) {
		ret2 = close(cd->fd);
		if (ret2) {
			if (!ret)
				ret = ret2;
			dbprintf(_("%s%s%s: %s\n"), destdir->path,
					destdir->sep, pbuf->path,
					strerror(errno));
		}
	}

out_cd:
	free(cd);
	return ret;
}

/* Copy file data */
static int
rdump_regfile_data(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf,
	int			fd)
{
	struct xfs_bmbt_irec	irec = { };
	struct xfs_buftarg	*btp;
	int			nmaps;
	off_t			pos = 0;
	const off_t		isize = ip->i_disk_size;
	int			ret;

	if (XFS_IS_REALTIME_INODE(ip))
		btp = ip->i_mount->m_rtdev_targp;
	else
		btp = ip->i_mount->m_ddev_targp;

	for (;
	     pos < isize;
	     pos = XFS_FSB_TO_B(mp, irec.br_startoff + irec.br_blockcount)) {
		struct xfs_buf	*bp;
		off_t		buf_pos;
		off_t		fd_pos;
		xfs_fileoff_t	off = XFS_B_TO_FSBT(mp, pos);
		xfs_filblks_t	max_read = XFS_B_TO_FSB(mp, 1048576);
		xfs_daddr_t	daddr;
		size_t		count;

		nmaps = 1;
		ret = -libxfs_bmapi_read(ip, off, max_read, &irec, &nmaps, 0);
		if (ret) {
			dbprintf(_("%s: %s\n"), pbuf->path, strerror(ret));
			if (strict_errors)
				return 1;
			continue;
		}
		if (!nmaps)
			break;

		if (!xfs_bmap_is_written_extent(&irec))
			continue;

		fd_pos = XFS_FSB_TO_B(mp, irec.br_startoff);
		if (XFS_IS_REALTIME_INODE(ip))
			daddr =  xfs_rtb_to_daddr(mp, irec.br_startblock);
		else
			daddr = XFS_FSB_TO_DADDR(mp, irec.br_startblock);

		ret = -libxfs_buf_read_uncached(btp, daddr,
				XFS_FSB_TO_BB(mp, irec.br_blockcount), 0, &bp,
				NULL);
		if (ret) {
			dbprintf(_("%s: reading pos 0x%llx %s\n"), pbuf->path,
					fd_pos, strerror(ret));
			if (strict_errors)
				return 1;
			continue;
		}

		count = XFS_FSB_TO_B(mp, irec.br_blockcount);
		if (fd_pos + count > isize)
			count = isize - fd_pos;

		buf_pos = 0;
		while (count > 0) {
			ssize_t	written;

			written = pwrite(fd, bp->b_addr + buf_pos, count,
					fd_pos);
			if (written < 0) {
				libxfs_buf_relse(bp);
				dbprintf(_("%s%s%s: writing pos 0x%llx %s\n"),
						destdir->path, destdir->sep,
						pbuf->path, fd_pos,
						strerror(errno));
				return 1;
			}
			if (!written) {
				libxfs_buf_relse(bp);
				dbprintf(
 _("%s%s%s: wrote zero at pos 0x%llx %s\n"),
						destdir->path, destdir->sep,
						pbuf->path, fd_pos);
				return 1;
			}

			fd_pos += written;
			buf_pos += written;
			count -= written;
		}

		libxfs_buf_relse(bp);
	}

	ret = ftruncate(fd, isize);
	if (ret) {
		dbprintf(_("%s%s%s: setting file length 0x%llx %s\n"),
				destdir->path, destdir->sep, pbuf->path,
				isize, strerror(errno));
		return 1;
	}

	return 0;
}

/* Copy a regular file */
static int
rdump_regfile(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf)
{
	int			fd;
	int			ret, ret2;

	fd = openat(destdir->fd, pbuf->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		dbprintf(_("%s%s%s: %s\n"), destdir->path, destdir->sep,
				pbuf->path, strerror(errno));
		return 1;
	}

	ret = rdump_fileattrs_fd(ip, destdir, pbuf, fd);
	if (ret && strict_errors)
		goto out_close;

	if (xfs_inode_has_attr_fork(ip)) {
		ret = rdump_xattrs(tp, ip, destdir, pbuf, fd);
		if (ret && strict_errors)
			goto out_close;
	}

	ret = rdump_regfile_data(tp, ip, destdir, pbuf, fd);
	if (ret && strict_errors)
		goto out_close;

	ret = rdump_timestamps_fd(ip, destdir, pbuf, fd);
	if (ret && strict_errors)
		goto out_close;

out_close:
	ret2 = close(fd);
	if (ret2) {
		if (!ret)
			ret = ret2;
		dbprintf(_("%s%s%s: %s\n"), destdir->path, destdir->sep,
				pbuf->path, strerror(errno));
		return 1;
	}

	return ret;
}

/* Copy a symlink */
static int
rdump_symlink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf)
{
	char			target[XFS_SYMLINK_MAXLEN + 1];
	struct xfs_ifork	*ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	unsigned int		targetlen = ip->i_disk_size;
	int			ret;

	if (ifp->if_format == XFS_DINODE_FMT_LOCAL) {
		memcpy(target, ifp->if_data, targetlen);
	} else {
		ret = -libxfs_symlink_remote_read(ip, target);
		if (ret) {
			dbprintf(_("%s: %s\n"), pbuf->path, strerror(ret));
			return strict_errors ? 1 : 0;
		}
	}
	target[targetlen] = 0;

	ret = symlinkat(target, destdir->fd, pbuf->path);
	if (ret) {
		dbprintf(_("%s%s%s: %s\n"), destdir->path, destdir->sep,
				pbuf->path, strerror(errno));
		return 1;
	}

	ret = rdump_fileattrs_path(ip, destdir, pbuf);
	if (ret && strict_errors)
		goto out;

	ret = rdump_timestamps_path(ip, destdir, pbuf);
	if (ret && strict_errors)
		goto out;

	ret = 0;
out:
	return ret;
}

/* Copy a special file */
static int
rdump_special(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	const struct destdir	*destdir,
	const struct pathbuf	*pbuf)
{
	const unsigned int	major = IRIX_DEV_MAJOR(VFS_I(ip)->i_rdev);
	const unsigned int	minor = IRIX_DEV_MINOR(VFS_I(ip)->i_rdev);
	int			ret;

	ret = mknodat(destdir->fd, pbuf->path, VFS_I(ip)->i_mode & S_IFMT,
			makedev(major, minor));
	if (ret) {
		dbprintf(_("%s%s%s: %s\n"), destdir->path, destdir->sep,
				pbuf->path, strerror(errno));
		return 1;
	}

	ret = rdump_fileattrs_path(ip, destdir, pbuf);
	if (ret && strict_errors)
		goto out;

	ret = rdump_timestamps_path(ip, destdir, pbuf);
	if (ret && strict_errors)
		goto out;

	ret = 0;
out:
	return ret;
}

/* Dump some kind of file. */
static int
rdump_file(
	struct xfs_trans	*tp,
	xfs_ino_t		ino,
	const struct destdir	*destdir,
	struct pathbuf		*pbuf)
{
	struct xfs_inode	*ip;
	int			ret;

	ret = -libxfs_iget(mp, tp, ino, 0, &ip);
	if (ret) {
		dbprintf(_("%s: %s\n"), pbuf->path, strerror(ret));
		return strict_errors ? ret : 0;
	}

	switch(VFS_I(ip)->i_mode & S_IFMT) {
	case S_IFDIR:
		ret = rdump_directory(tp, ip, destdir, pbuf);
		break;
	case S_IFREG:
		ret = rdump_regfile(tp, ip, destdir, pbuf);
		break;
	case S_IFLNK:
		ret = rdump_symlink(tp, ip, destdir, pbuf);
		break;
	default:
		ret = rdump_special(tp, ip, destdir, pbuf);
		break;
	}

	libxfs_irele(ip);
	return ret;
}

/* Copy one path out of the filesystem. */
static int
rdump_path(
	struct xfs_mount	*mp,
	bool			sole_path,
	const char		*path,
	const struct destdir	*destdir)
{
	struct xfs_trans	*tp;
	struct pathbuf		*pbuf;
	const char		*basename;
	ssize_t			pathlen = strlen(path);
	int			ret = 1;

	/* Set up destination path data */
	if (pathlen > PATH_MAX) {
		dbprintf(_("%s: %s\n"), path, strerror(ENAMETOOLONG));
		return 1;
	}

	pbuf = calloc(1, sizeof(struct pathbuf));
	if (!pbuf) {
		dbprintf(_("allocating path buf: %s\n"), strerror(errno));
		return 1;
	}
	basename = strrchr(path, '/');
	if (basename) {
		pbuf->len = pathlen - (basename + 1 - path);
		memcpy(pbuf->path, basename + 1, pbuf->len);
	} else {
		pbuf->len = pathlen;
		memcpy(pbuf->path, path, pbuf->len);
	}
	pbuf->path[pbuf->len] = 0;

	/* Dump the inode referenced. */
	if (pathlen) {
		ret = path_walk(mp->m_sb.sb_rootino, path);
		if (ret) {
			dbprintf(_("%s: %s\n"), path, strerror(ret));
			return 1;
		}

		if (sole_path) {
			struct xfs_dinode	*dip = iocur_top->data;

			/*
			 * If this is the only path to copy out and it's a dir,
			 * then we can copy the children directly into the
			 * target.
			 */
			if (S_ISDIR(be16_to_cpu(dip->di_mode))) {
				pbuf->len = 0;
				pbuf->path[0] = 0;
			}
		}
	} else {
		set_cur_inode(mp->m_sb.sb_rootino);
	}

	ret = -libxfs_trans_alloc_empty(mp, &tp);
	if (ret) {
		dbprintf(_("allocating state: %s\n"), strerror(ret));
		goto out_pbuf;
	}

	ret = rdump_file(tp, iocur_top->ino, destdir, pbuf);
	libxfs_trans_cancel(tp);
out_pbuf:
	free(pbuf);
	return ret;
}

static int
rdump_f(
	int		argc,
	char		*argv[])
{
	struct destdir	destdir;
	int		i;
	int		c;
	int		ret;

	lost_mask = 0;
	strict_errors = false;
	while ((c = getopt(argc, argv, "s")) != -1) {
		switch (c) {
		case 's':
			strict_errors = true;
			break;
		default:
			rdump_help();
			return 0;
		}
	}

	if (argc < optind + 1) {
		dbprintf(
 _("Must supply destination directory.\n"));
		return 0;
	}

	/* Create and open destination directory */
	destdir.path = argv[argc - 1];
	ret = mkdir(destdir.path, 0755);
	if (ret && errno != EEXIST) {
		dbprintf(_("%s: %s\n"), destdir.path, strerror(errno));
		exitcode = 1;
		return 0;
	}

	if (destdir.path[0] == 0) {
		dbprintf(
 _("Destination dir must be at least one character.\n"));
		exitcode = 1;
		return 0;
	}

	if (destdir.path[strlen(destdir.path) - 1] != '/')
		destdir.sep = "/";
	else
		destdir.sep = "";
	destdir.fd = open(destdir.path, O_DIRECTORY | O_RDONLY);
	if (destdir.fd < 0) {
		dbprintf(_("%s: %s\n"), destdir.path, strerror(errno));
		exitcode = 1;
		return 0;
	}

	if (optind == argc - 1) {
		/* no dirs given, just do the whole fs */
		push_cur();
		ret = rdump_path(mp, false, "", &destdir);
		pop_cur();
		if (ret)
			exitcode = 1;
		goto out_close;
	}

	for (i = optind; i < argc - 1; i++) {
		size_t	len = strlen(argv[i]);

		/* trim trailing slashes */
		while (len && argv[i][len - 1] == '/')
			len--;
		argv[i][len] = 0;

		push_cur();
		ret = rdump_path(mp, argc == optind + 2, argv[i], &destdir);
		pop_cur();

		if (ret) {
			exitcode = 1;
			if (strict_errors)
				break;
		}
	}

out_close:
	ret = close(destdir.fd);
	if (ret) {
		dbprintf(_("%s: %s\n"), destdir.path, strerror(errno));
		exitcode = 1;
	}

	if (lost_mask & LOST_OWNER)
		dbprintf(_("%s: some uid/gid could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_MODE)
		dbprintf(_("%s: some file modes could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_TIME)
		dbprintf(_("%s: some timestamps could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_SOME_FSXATTR)
		dbprintf(_("%s: some xfs file attr bits could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_FSXATTR)
		dbprintf(_("%s: some xfs file attrs could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_XATTR)
		dbprintf(_("%s: some extended xattrs could not be set\n"),
				destdir.path);
	if (lost_mask & LOST_ACL)
		dbprintf(_("%s: some ACLs could not be translated\n"),
				destdir.path);

	return 0;
}

static struct cmdinfo rdump_cmd = {
	.name		= "rdump",
	.cfunc		= rdump_f,
	.argmin		= 0,
	.argmax		= -1,
	.canpush	= 0,
	.args		= "[-s] [paths...] dest_directory",
	.help		= rdump_help,
};

void
rdump_init(void)
{
	rdump_cmd.oneline = _("recover files out of a filesystem");
	add_command(&rdump_cmd);
}
