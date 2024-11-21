// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include <sys/stat.h>
#include <sys/xattr.h>
#include <linux/xattr.h>
#include "libfrog/convert.h"
#include "proto.h"

/*
 * Prototypes for internal functions.
 */
static char *getstr(char **pp);
static void fail(char *msg, int i);
static struct xfs_trans * getres(struct xfs_mount *mp, uint blocks);
static void rsvfile(xfs_mount_t *mp, xfs_inode_t *ip, long long len);
static int newregfile(char **pp, char **fname);
static void rtinit(xfs_mount_t *mp);
static long filesize(int fd);
static int slashes_are_spaces;

/*
 * Use this for block reservations needed for mkfs's conditions
 * (basically no fragmentation).
 */
#define	MKFS_BLOCKRES_INODE	\
	((uint)(M_IGEO(mp)->ialloc_blks + (M_IGEO(mp)->inobt_maxlevels - 1)))
#define	MKFS_BLOCKRES(rb)	\
	((uint)(MKFS_BLOCKRES_INODE + XFS_DA_NODE_MAXDEPTH + \
	(XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK) - 1) + (rb)))

static long long
getnum(
	const char	*str,
	unsigned int	blksize,
	unsigned int	sectsize,
	bool		convert)
{
	long long	i;
	char		*sp;

	if (convert)
		return cvtnum(blksize, sectsize, str);

	i = strtoll(str, &sp, 0);
	if (i == 0 && sp == str)
		return -1LL;
	if (*sp != '\0')
		return -1LL; /* trailing garbage */
	return i;
}

char *
setup_proto(
	char	*fname)
{
	char		*buf = NULL;
	static char	dflt[] = "d--755 0 0 $";
	int		fd;
	long		size;

	if (!fname)
		return dflt;
	if ((fd = open(fname, O_RDONLY)) < 0 || (size = filesize(fd)) < 0) {
		fprintf(stderr, _("%s: failed to open %s: %s\n"),
			progname, fname, strerror(errno));
		goto out_fail;
	}

	buf = malloc(size + 1);
	if (read(fd, buf, size) < size) {
		fprintf(stderr, _("%s: read failed on %s: %s\n"),
			progname, fname, strerror(errno));
		goto out_fail;
	}
	if (buf[size - 1] != '\n') {
		fprintf(stderr, _("%s: proto file %s premature EOF\n"),
			progname, fname);
		goto out_fail;
	}
	buf[size] = '\0';
	/*
	 * Skip past the stuff there for compatibility, a string and 2 numbers.
	 */
	(void)getstr(&buf);	/* boot image name */
	(void)getnum(getstr(&buf), 0, 0, false);	/* block count */
	(void)getnum(getstr(&buf), 0, 0, false);	/* inode count */
	close(fd);
	return buf;

out_fail:
	if (fd >= 0)
		close(fd);
	free(buf);
	exit(1);
}

static void
fail(
	char	*msg,
	int	i)
{
	fprintf(stderr, "%s: %s [%d - %s]\n", progname, msg, i, strerror(i));
	exit(1);
}

void
res_failed(
	int	i)
{
	fail(_("cannot reserve space"), i);
}

static struct xfs_trans *
getres(
	struct xfs_mount *mp,
	uint		blocks)
{
	struct xfs_trans *tp;
	int		i;
	uint		r;

	for (i = 0, r = MKFS_BLOCKRES(blocks); r >= blocks; r--) {
		i = -libxfs_trans_alloc_rollable(mp, r, &tp);
		if (i == 0)
			return tp;
	}
	res_failed(i);
	/* NOTREACHED */
	return NULL;
}

static char *
getstr(
	char	**pp)
{
	char	c;
	char	*p;
	char	*rval;

	p = *pp;
	while ((c = *p)) {
		switch (c) {
		case ' ':
		case '\t':
		case '\n':
			p++;
			continue;
		case ':':
			p++;
			while (*p++ != '\n')
				;
			continue;
		default:
			rval = p;
			while (c != ' ' && c != '\t' && c != '\n' && c != '\0')
				c = *++p;
			*p++ = '\0';
			*pp = p;
			return rval;
		}
	}
	if (c != '\0') {
		fprintf(stderr, _("%s: premature EOF in prototype file\n"),
			progname);
		exit(1);
	}
	return NULL;
}

/* Extract directory entry name from a protofile. */
static char *
getdirentname(
	char	**pp)
{
	char	*p = getstr(pp);
	char	*c = p;

	if (!p)
		return NULL;

	if (!slashes_are_spaces)
		return p;

	/* Replace slash with space because slashes aren't allowed. */
	while (*c) {
		if (*c == '/')
			*c = ' ';
		c++;
	}

	return p;
}

static void
rsvfile(
	xfs_mount_t	*mp,
	xfs_inode_t	*ip,
	long long	llen)
{
	int		error;
	xfs_trans_t	*tp;

	error = -libxfs_alloc_file_space(ip, 0, llen, XFS_BMAPI_PREALLOC);

	if (error) {
		fail(_("error reserving space for a file"), error);
		exit(1);
	}

	/*
	 * update the inode timestamp, mode, and prealloc flag bits
	 */
	error = -libxfs_trans_alloc_rollable(mp, 0, &tp);
	if (error)
		fail(_("allocating transaction for a file"), error);
	libxfs_trans_ijoin(tp, ip, 0);

	VFS_I(ip)->i_mode &= ~S_ISUID;

	/*
	 * Note that we don't have to worry about mandatory
	 * file locking being disabled here because we only
	 * clear the S_ISGID bit if the Group execute bit is
	 * on, but if it was on then mandatory locking wouldn't
	 * have been enabled.
	 */
	if (VFS_I(ip)->i_mode & S_IXGRP)
		VFS_I(ip)->i_mode &= ~S_ISGID;

	libxfs_trans_ichgtime(tp, ip, XFS_ICHGTIME_MOD | XFS_ICHGTIME_CHG);

	ip->i_diflags |= XFS_DIFLAG_PREALLOC;

	libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = -libxfs_trans_commit(tp);
	if (error)
		fail(_("committing space for a file failed"), error);
}

static void
writesymlink(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	char			*buf,
	int			len)
{
	struct xfs_mount	*mp = tp->t_mountp;
	xfs_extlen_t		nb = XFS_B_TO_FSB(mp, len);
	int			error;

	error = -libxfs_symlink_write_target(tp, ip, ip->i_ino, buf, len, nb,
			nb);
	if (error) {
		fprintf(stderr,
	_("%s: error %d creating symlink to '%s'.\n"), progname, error, buf);
		exit(1);
	}
}

static void
writefile_range(
	struct xfs_inode	*ip,
	const char		*fname,
	int			fd,
	off_t			pos,
	uint64_t		len)
{
	static char		buf[131072];
	int			error;

	if (XFS_IS_REALTIME_INODE(ip)) {
		fprintf(stderr,
 _("%s: creating realtime files from proto file not supported.\n"),
				progname);
		exit(1);
	}

	while (len > 0) {
		ssize_t		read_len;

		read_len = pread(fd, buf, min(len, sizeof(buf)), pos);
		if (read_len < 0) {
			fprintf(stderr, _("%s: read failed on %s: %s\n"),
					progname, fname, strerror(errno));
			exit(1);
		}

		error = -libxfs_alloc_file_space(ip, pos, read_len, 0);
		if (error)
			fail(_("error allocating space for a file"), error);

		error = -libxfs_file_write(ip, buf, pos, read_len);
		if (error)
			fail(_("error writing file"), error);

		pos += read_len;
		len -= read_len;
	}
}

static void
writefile(
	struct xfs_inode	*ip,
	const char		*fname,
	int			fd)
{
	struct xfs_trans	*tp;
	struct xfs_mount	*mp = ip->i_mount;
	struct stat		statbuf;
	off_t			data_pos;
	off_t			eof = 0;
	int			error;

	/* do not try to read from non-regular files */
	error = fstat(fd, &statbuf);
	if (error < 0)
		fail(_("unable to stat file to copyin"), errno);

	if (!S_ISREG(statbuf.st_mode))
		return;

	data_pos = lseek(fd, 0, SEEK_DATA);
	while (data_pos >= 0) {
		off_t		hole_pos;

		hole_pos = lseek(fd, data_pos, SEEK_HOLE);
		if (hole_pos < 0) {
			/* save error, break */
			data_pos = hole_pos;
			break;
		}
		if (hole_pos <= data_pos) {
			/* shouldn't happen??? */
			break;
		}

		writefile_range(ip, fname, fd, data_pos, hole_pos - data_pos);
		eof = hole_pos;

		data_pos = lseek(fd, hole_pos, SEEK_DATA);
	}
	if (data_pos < 0 && errno != ENXIO)
		fail(_("error finding file data to import"), errno);

	/* extend EOF only after writing all the file data */
	error = -libxfs_trans_alloc_inode(ip, &M_RES(mp)->tr_ichange, 0, 0,
			false, &tp);
	if (error)
		fail(_("error creating isize transaction"), error);

	libxfs_trans_ijoin(tp, ip, 0);
	ip->i_disk_size = eof;
	libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = -libxfs_trans_commit(tp);
	if (error)
		fail(_("error committing isize transaction"), error);
}

static void
writeattr(
	struct xfs_inode	*ip,
	const char		*fname,
	int			fd,
	const char		*attrname,
	char			*valuebuf,
	size_t			valuelen)
{
	struct xfs_da_args	args = {
		.dp		= ip,
		.geo		= ip->i_mount->m_attr_geo,
		.owner		= ip->i_ino,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
		.value		= valuebuf,
	};
	ssize_t			ret;
	int			error;

	ret = fgetxattr(fd, attrname, valuebuf, valuelen);
	if (ret < 0) {
		if (errno == EOPNOTSUPP)
			return;
		fail(_("error collecting xattr value"), errno);
	}
	if (ret == 0)
		return;

	if (!strncmp(attrname, XATTR_TRUSTED_PREFIX, XATTR_TRUSTED_PREFIX_LEN)) {
		args.name = (unsigned char *)attrname + XATTR_TRUSTED_PREFIX_LEN;
		args.attr_filter = LIBXFS_ATTR_ROOT;
	} else if (!strncmp(attrname, XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN)) {
		args.name = (unsigned char *)attrname + XATTR_SECURITY_PREFIX_LEN;
		args.attr_filter = LIBXFS_ATTR_SECURE;
	} else if (!strncmp(attrname, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN)) {
		args.name = (unsigned char *)attrname + XATTR_USER_PREFIX_LEN;
		args.attr_filter = 0;
	} else {
		args.name = (unsigned char *)attrname;
		args.attr_filter = 0;
	}
	args.namelen = strlen((char *)args.name);

	args.valuelen = ret;
	libxfs_attr_sethash(&args);

	error = -libxfs_attr_set(&args, XFS_ATTRUPDATE_UPSERT, false);
	if (error)
		fail(_("setting xattr value"), error);
}

static void
writeattrs(
	struct xfs_inode	*ip,
	const char		*fname,
	int			fd)
{
	char			*namebuf, *p, *end;
	char			*valuebuf = NULL;
	ssize_t			ret;

	namebuf = malloc(XATTR_LIST_MAX);
	if (!namebuf)
		fail(_("error allocating xattr name buffer"), errno);

	ret = flistxattr(fd, namebuf, XATTR_LIST_MAX);
	if (ret < 0) {
		if (errno == EOPNOTSUPP)
			goto out_namebuf;
		fail(_("error collecting xattr names"), errno);
	}

	p = namebuf;
	end = namebuf + ret;
	for (p = namebuf; p < end; p += strlen(p) + 1) {
		if (!valuebuf) {
			valuebuf = malloc(ATTR_MAX_VALUELEN);
			if (!valuebuf)
				fail(_("error allocating xattr value buffer"),
						errno);
		}

		writeattr(ip, fname, fd, p, valuebuf, ATTR_MAX_VALUELEN);
	}

	free(valuebuf);
out_namebuf:
	free(namebuf);
}

static int
newregfile(
	char		**pp,
	char		**fname)
{
	int		fd;
	off_t		size;

	*fname = getstr(pp);
	if ((fd = open(*fname, O_RDONLY)) < 0 || (size = filesize(fd)) < 0) {
		fprintf(stderr, _("%s: cannot open %s: %s\n"),
			progname, *fname, strerror(errno));
		exit(1);
	}

	return fd;
}

static void
newdirent(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	struct xfs_inode	*pip,
	struct xfs_name		*name,
	struct xfs_inode	*ip,
	struct xfs_parent_args	*ppargs)
{
	int	error;
	int	rsv;

	if (!libxfs_dir2_namecheck(name->name, name->len)) {
		fprintf(stderr, _("%.*s: invalid directory entry name\n"),
				name->len, name->name);
		exit(1);
	}

	rsv = XFS_DIRENTER_SPACE_RES(mp, name->len);

	error = -libxfs_dir_createname(tp, pip, name, ip->i_ino, rsv);
	if (error)
		fail(_("directory createname error"), error);

	if (ppargs) {
		error = -libxfs_parent_addname(tp, ppargs, pip, name, ip);
		if (error)
			fail(_("parent addname error"), error);
	}
}

static void
newdirectory(
	xfs_mount_t	*mp,
	xfs_trans_t	*tp,
	xfs_inode_t	*dp,
	xfs_inode_t	*pdp)
{
	int	error;

	error = -libxfs_dir_init(tp, dp, pdp);
	if (error)
		fail(_("directory create error"), error);
}

static struct xfs_parent_args *
newpptr(
	struct xfs_mount	*mp)
{
	struct xfs_parent_args	*ret;
	int			error;

	error = -libxfs_parent_start(mp, &ret);
	if (error)
		fail(_("initializing parent pointer"), error);

	return ret;
}

struct cred {
	uid_t			cr_uid;
	gid_t			cr_gid;
};

static int
creatproto(
	struct xfs_trans	**tpp,
	struct xfs_inode	*dp,
	mode_t			mode,
	xfs_dev_t		rdev,
	struct cred		*cr,
	struct fsxattr		*fsx,
	struct xfs_inode	**ipp)
{
	struct xfs_icreate_args	args = {
		.idmap		= libxfs_nop_idmap,
		.pip		= dp,
		.rdev		= rdev,
		.mode		= mode,
	};
	struct xfs_inode	*ip;
	struct inode		*inode;
	xfs_ino_t		ino;
	int			error;

	/* Root directories cannot be linked to a parent. */
	if (!dp)
		args.flags |= XFS_ICREATE_UNLINKABLE;

	/*
	 * Call the space management code to pick the on-disk inode to be
	 * allocated.
	 */
	error = -libxfs_dialloc(tpp, &args, &ino);
	if (error)
		return error;

	error = -libxfs_icreate(*tpp, ino, &args, &ip);
	if (error)
		return error;

	inode = VFS_I(ip);
	i_uid_write(inode, cr->cr_uid);
	i_gid_write(inode, cr->cr_gid);

	/* If there is no parent dir, initialize the file from fsxattr data. */
	if (dp == NULL) {
		ip->i_projid = fsx->fsx_projid;
		ip->i_extsize = fsx->fsx_extsize;
		ip->i_diflags = xfs_flags2diflags(ip, fsx->fsx_xflags);

		if (xfs_has_v3inodes(ip->i_mount)) {
			ip->i_diflags2 = xfs_flags2diflags2(ip,
							fsx->fsx_xflags);
			ip->i_cowextsize = fsx->fsx_cowextsize;
		}

		/* xfsdump breaks if the root dir has a nonzero generation */
		inode->i_generation = 0;
	}

	libxfs_trans_log_inode(*tpp, ip, XFS_ILOG_CORE);
	*ipp = ip;
	return 0;
}

/* Create a new metadata root directory. */
static int
create_metadir(
	struct xfs_mount	*mp)
{
	struct xfs_inode	*ip = NULL;
	struct xfs_trans	*tp;
	int			error;
	struct xfs_icreate_args	args = {
		.mode		= S_IFDIR,
		.flags		= XFS_ICREATE_UNLINKABLE,
	};
	xfs_ino_t		ino;

	if (!xfs_has_metadir(mp))
		return 0;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_create,
			libxfs_create_space_res(mp, MAXNAMELEN), 0, 0, &tp);
	if (error)
		return error;

	/*
	 * Create a new inode and set the sb pointer.  The primary super is
	 * still marked inprogress, so we do not need to log the metadirino
	 * change ourselves.
	 */
	error = -libxfs_dialloc(&tp, &args, &ino);
	if (error)
		goto out_cancel;
	error = -libxfs_icreate(tp, ino, &args, &ip);
	if (error)
		goto out_cancel;
	mp->m_sb.sb_metadirino = ino;

	/*
	 * Initialize the root directory.  There are no ILOCKs in userspace
	 * so we do not need to drop it here.
	 */
	libxfs_metafile_set_iflag(tp, ip, XFS_METAFILE_DIR);
	error = -libxfs_dir_init(tp, ip, ip);
	if (error)
		goto out_cancel;

	error = -libxfs_trans_commit(tp);
	if (error)
		goto out_rele;

	mp->m_metadirip = ip;
	return 0;

out_cancel:
	libxfs_trans_cancel(tp);
out_rele:
	if (ip)
		libxfs_irele(ip);
	return error;
}

static void
parseproto(
	xfs_mount_t	*mp,
	xfs_inode_t	*pip,
	struct fsxattr	*fsxp,
	char		**pp,
	char		*name)
{
#define	IF_REGULAR	0
#define	IF_RESERVED	1
#define	IF_BLOCK	2
#define	IF_CHAR		3
#define	IF_DIRECTORY	4
#define	IF_SYMLINK	5
#define	IF_FIFO		6

	char		*buf;
	int		error;
	int		flags;
	int		fmt;
	int		i;
	xfs_inode_t	*ip;
	int		fd = -1;
	off_t		len;
	long long	llen;
	int		majdev;
	int		mindev;
	int		mode;
	char		*mstr;
	xfs_trans_t	*tp;
	int		val;
	int		isroot = 0;
	struct cred	creds;
	char		*value;
	char		*fname = NULL;
	struct xfs_name	xname;
	struct xfs_parent_args *ppargs = NULL;

	memset(&creds, 0, sizeof(creds));
	mstr = getstr(pp);
	switch (mstr[0]) {
	case '-':
		fmt = IF_REGULAR;
		break;
	case 'r':
		fmt = IF_RESERVED;
		break;
	case 'b':
		fmt = IF_BLOCK;
		break;
	case 'c':
		fmt = IF_CHAR;
		break;
	case 'd':
		fmt = IF_DIRECTORY;
		break;
	case 'l':
		fmt = IF_SYMLINK;
		break;
	case 'p':
		fmt = IF_FIFO;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	mode = 0;
	switch (mstr[1]) {
	case '-':
		break;
	case 'u':
		mode |= S_ISUID;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	switch (mstr[2]) {
	case '-':
		break;
	case 'g':
		mode |= S_ISGID;
		break;
	default:
		fprintf(stderr, _("%s: bad format string %s\n"),
			progname, mstr);
		exit(1);
	}
	val = 0;
	for (i = 3; i < 6; i++) {
		if (mstr[i] < '0' || mstr[i] > '7') {
			fprintf(stderr, _("%s: bad format string %s\n"),
				progname, mstr);
			exit(1);
		}
		val = val * 8 + mstr[i] - '0';
	}
	mode |= val;
	creds.cr_uid = (int)getnum(getstr(pp), 0, 0, false);
	creds.cr_gid = (int)getnum(getstr(pp), 0, 0, false);
	xname.name = (unsigned char *)name;
	xname.len = name ? strlen(name) : 0;
	xname.type = 0;
	flags = XFS_ILOG_CORE;
	switch (fmt) {
	case IF_REGULAR:
		fd = newregfile(pp, &fname);
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFREG, 0, &creds, fsxp,
				&ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_REG_FILE;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;

	case IF_RESERVED:			/* pre-allocated space only */
		value = getstr(pp);
		llen = getnum(value, mp->m_sb.sb_blocksize,
			      mp->m_sb.sb_sectsize, true);
		if (llen < 0) {
			fprintf(stderr,
				_("%s: Bad value %s for proto file %s\n"),
				progname, value, name);
			exit(1);
		}
		tp = getres(mp, XFS_B_TO_FSB(mp, llen));
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFREG, 0, &creds, fsxp,
				&ip);
		if (error)
			fail(_("Inode pre-allocation failed"), error);

		libxfs_trans_ijoin(tp, pip, 0);

		xname.type = XFS_DIR3_FT_REG_FILE;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		libxfs_trans_log_inode(tp, ip, flags);
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Space preallocation failed."), error);
		libxfs_parent_finish(mp, ppargs);
		rsvfile(mp, ip, llen);
		libxfs_irele(ip);
		return;

	case IF_BLOCK:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		majdev = getnum(getstr(pp), 0, 0, false);
		mindev = getnum(getstr(pp), 0, 0, false);
		error = creatproto(&tp, pip, mode | S_IFBLK,
				IRIX_MKDEV(majdev, mindev), &creds, fsxp, &ip);
		if (error) {
			fail(_("Inode allocation failed"), error);
		}
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_BLKDEV;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		flags |= XFS_ILOG_DEV;
		break;

	case IF_CHAR:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		majdev = getnum(getstr(pp), 0, 0, false);
		mindev = getnum(getstr(pp), 0, 0, false);
		error = creatproto(&tp, pip, mode | S_IFCHR,
				IRIX_MKDEV(majdev, mindev), &creds, fsxp, &ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_CHRDEV;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		flags |= XFS_ILOG_DEV;
		break;

	case IF_FIFO:
		tp = getres(mp, 0);
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFIFO, 0, &creds, fsxp,
				&ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_FIFO;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;
	case IF_SYMLINK:
		buf = getstr(pp);
		len = (int)strlen(buf);
		tp = getres(mp, XFS_B_TO_FSB(mp, len));
		ppargs = newpptr(mp);
		error = creatproto(&tp, pip, mode | S_IFLNK, 0, &creds, fsxp,
				&ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		writesymlink(tp, ip, buf, len);
		libxfs_trans_ijoin(tp, pip, 0);
		xname.type = XFS_DIR3_FT_SYMLINK;
		newdirent(mp, tp, pip, &xname, ip, ppargs);
		break;
	case IF_DIRECTORY:
		tp = getres(mp, 0);
		error = creatproto(&tp, pip, mode | S_IFDIR, 0, &creds, fsxp,
				&ip);
		if (error)
			fail(_("Inode allocation failed"), error);
		if (!pip) {
			pip = ip;
			mp->m_sb.sb_rootino = ip->i_ino;
			libxfs_log_sb(tp);
			isroot = 1;
		} else {
			ppargs = newpptr(mp);
			libxfs_trans_ijoin(tp, pip, 0);
			xname.type = XFS_DIR3_FT_DIR;
			newdirent(mp, tp, pip, &xname, ip, ppargs);
			libxfs_bumplink(tp, pip);
			libxfs_trans_log_inode(tp, pip, XFS_ILOG_CORE);
		}
		newdirectory(mp, tp, ip, pip);
		libxfs_trans_log_inode(tp, ip, flags);
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Directory inode allocation failed."), error);

		libxfs_parent_finish(mp, ppargs);

		/*
		 * RT initialization.  Do this here to ensure that
		 * the RT inodes get placed after the root inode.
		 */
		if (isroot) {
			error = create_metadir(mp);
			if (error)
				fail(
	_("Creation of the metadata directory inode failed"),
					error);

			rtinit(mp);
		}
		tp = NULL;
		for (;;) {
			name = getdirentname(pp);
			if (!name)
				break;
			if (strcmp(name, "$") == 0)
				break;
			parseproto(mp, ip, fsxp, pp, name);
		}
		libxfs_irele(ip);
		return;
	default:
		ASSERT(0);
		fail(_("Unknown format"), EINVAL);
	}
	libxfs_trans_log_inode(tp, ip, flags);
	error = -libxfs_trans_commit(tp);
	if (error) {
		fail(_("Error encountered creating file from prototype file"),
			error);
	}

	libxfs_parent_finish(mp, ppargs);
	if (fmt == IF_REGULAR) {
		writefile(ip, fname, fd);
		writeattrs(ip, fname, fd);
		close(fd);
	}
	libxfs_irele(ip);
}

void
parse_proto(
	xfs_mount_t	*mp,
	struct fsxattr	*fsx,
	char		**pp,
	int		proto_slashes_are_spaces)
{
	slashes_are_spaces = proto_slashes_are_spaces;
	parseproto(mp, NULL, fsx, pp, NULL);
}

/* Create a sb-rooted metadata file. */
static void
create_sb_metadata_file(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type,
	int			(*create)(struct xfs_rtgroup *rtg,
					  struct xfs_inode *ip,
					  struct xfs_trans *tp,
					  bool init))
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	struct xfs_icreate_args	args = {
		.mode		= S_IFREG,
		.flags		= XFS_ICREATE_UNLINKABLE,
	};
	struct xfs_trans	*tp;
	struct xfs_inode	*ip = NULL;
	xfs_ino_t		ino;
	int			error;

	error = -libxfs_trans_alloc_rollable(mp, MKFS_BLOCKRES_INODE, &tp);
	if (error)
		res_failed(error);

	error = -libxfs_dialloc(&tp, &args, &ino);
	if (error)
		goto fail;

	error = -libxfs_icreate(tp, ino, &args, &ip);
	if (error)
		goto fail;

	error = create(rtg, ip, tp, true);
	if (error < 0)
		error = -error;
	if (error)
		goto fail;

	switch (type) {
	case XFS_RTGI_BITMAP:
		mp->m_sb.sb_rbmino = ip->i_ino;
		break;
	case XFS_RTGI_SUMMARY:
		mp->m_sb.sb_rsumino = ip->i_ino;
		break;
	default:
		error = EFSCORRUPTED;
		goto fail;
	}
	libxfs_log_sb(tp);

	error = -libxfs_trans_commit(tp);
	if (error)
		goto fail;
	rtg->rtg_inodes[type] = ip;
	return;

fail:
	if (ip)
		libxfs_irele(ip);
	if (error)
		fail(_("Realtime inode allocation failed"), error);
}

/*
 * Free the whole realtime area using transactions.
 * Do one transaction per bitmap block.
 */
static void
rtfreesp_init(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	struct xfs_trans	*tp;
	xfs_rtxnum_t		rtx;
	xfs_rtxnum_t		ertx;
	int			error;

	/*
	 * First zero the realtime bitmap and summary files.
	 */
	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_BITMAP, 0,
			mp->m_sb.sb_rbmblocks, NULL);
	if (error)
		fail(_("Initialization of rtbitmap inode failed"), error);

	error = -libxfs_rtfile_initialize_blocks(rtg, XFS_RTGI_SUMMARY, 0,
			mp->m_rsumblocks, NULL);
	if (error)
		fail(_("Initialization of rtsummary inode failed"), error);

	/*
	 * Then free the blocks into the allocator, one bitmap block at a time.
	 */
	for (rtx = 0; rtx < mp->m_sb.sb_rextents; rtx = ertx) {
		error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_itruncate,
				0, 0, 0, &tp);
		if (error)
			res_failed(error);

		libxfs_trans_ijoin(tp, rtg->rtg_inodes[XFS_RTGI_BITMAP], 0);
		ertx = min(mp->m_sb.sb_rextents,
			   rtx + NBBY * mp->m_sb.sb_blocksize);

		error = -libxfs_rtfree_extent(tp, rtg, rtx,
				(xfs_rtxlen_t)(ertx - rtx));
		if (error) {
			fail(_("Error initializing the realtime space"),
				error);
		}
		error = -libxfs_trans_commit(tp);
		if (error)
			fail(_("Initialization of the realtime space failed"),
					error);
	}
}

/*
 * Allocate the realtime bitmap and summary inodes, and fill in data if any.
 */
static void
rtinit(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;

	while ((rtg = xfs_rtgroup_next(mp, rtg))) {
		create_sb_metadata_file(rtg, XFS_RTGI_BITMAP,
				libxfs_rtbitmap_create);
		create_sb_metadata_file(rtg, XFS_RTGI_SUMMARY,
				libxfs_rtsummary_create);

		rtfreesp_init(rtg);
	}
}

static off_t
filesize(
	int		fd)
{
	struct stat	stb;

	if (fstat(fd, &stb) < 0)
		return -1;
	return stb.st_size;
}
