// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "threads.h"
#include "threads.h"
#include "prefetch.h"
#include "avl.h"
#include "globals.h"
#include "agheader.h"
#include "incore.h"
#include "dir2.h"
#include "protos.h"
#include "err_protos.h"
#include "dinode.h"
#include "progress.h"
#include "versions.h"
#include "repair/pptr.h"
#include "repair/rt.h"
#include "repair/quotacheck.h"
#include "repair/slab.h"
#include "repair/rmap.h"

static xfs_ino_t		orphanage_ino;

/*
 * Data structures used to keep track of directories where the ".."
 * entries are updated. These must be rebuilt after the initial pass
 */
typedef struct dotdot_update {
	struct list_head	list;
	ino_tree_node_t		*irec;
	xfs_agnumber_t		agno;
	int			ino_offset;
} dotdot_update_t;

static LIST_HEAD(dotdot_update_list);
static int			dotdot_update;

static void
add_dotdot_update(
	xfs_agnumber_t		agno,
	ino_tree_node_t		*irec,
	int			ino_offset)
{
	dotdot_update_t		*dir = malloc(sizeof(dotdot_update_t));

	if (!dir)
		do_error(_("malloc failed add_dotdot_update (%zu bytes)\n"),
			sizeof(dotdot_update_t));

	INIT_LIST_HEAD(&dir->list);
	dir->irec = irec;
	dir->agno = agno;
	dir->ino_offset = ino_offset;

	list_add(&dir->list, &dotdot_update_list);
}

/*
 * Data structures and routines to keep track of directory entries
 * and whether their leaf entry has been seen. Also used for name
 * duplicate checking and rebuilding step if required.
 */
struct dir_hash_ent {
	struct dir_hash_ent	*nextbyhash;	/* next in name bucket */
	struct dir_hash_ent	*nextbyorder;	/* next in order added */
	xfs_dahash_t		hashval;	/* hash value of name */
	uint32_t		address;	/* offset of data entry */
	xfs_ino_t		inum;		/* inode num of entry */
	short			junkit;		/* name starts with / */
	short			seen;		/* have seen leaf entry */
	struct xfs_name		name;
	unsigned char		namebuf[];
};

struct dir_hash_tab {
	int			size;		/* size of hash tables */
	struct dir_hash_ent	*first;		/* ptr to first added entry */
	struct dir_hash_ent	*last;		/* ptr to last added entry */
	struct dir_hash_ent	**byhash;	/* ptr to name hash buckets */
#define HT_UNSEEN		1
	struct radix_tree_root	byaddr;
};

#define	DIR_HASH_TAB_SIZE(n)	\
	(sizeof(struct dir_hash_tab) + (sizeof(struct dir_hash_ent *) * (n)))
#define	DIR_HASH_FUNC(t,a)	((a) % (t)->size)

/*
 * Track the contents of the freespace table in a directory.
 */
typedef struct freetab {
	int			naents;	/* expected number of data blocks */
	int			nents;	/* number of data blocks processed */
	struct freetab_ent {
		xfs_dir2_data_off_t	v;
		short			s;
	} ents[1];
} freetab_t;
#define	FREETAB_SIZE(n)	\
	(offsetof(freetab_t, ents) + (sizeof(struct freetab_ent) * (n)))

#define	DIR_HASH_CK_OK		0
#define	DIR_HASH_CK_DUPLEAF	1
#define	DIR_HASH_CK_BADHASH	2
#define	DIR_HASH_CK_NODATA	3
#define	DIR_HASH_CK_NOLEAF	4
#define	DIR_HASH_CK_BADSTALE	5
#define	DIR_HASH_CK_TOTAL	6

/*
 * Need to handle CRC and validation errors specially here. If there is a
 * validator error, re-read without the verifier so that we get a buffer we can
 * check and repair. Re-attach the ops to the buffer after the read so that when
 * it is rewritten the CRC is recalculated.
 *
 * If the buffer was not read, we return an error. If the buffer was read but
 * had a CRC or corruption error, we reread it without the verifier and if it is
 * read successfully we increment *crc_error and return 0. Otherwise we
 * return the read error.
 */
static int
dir_read_buf(
	struct xfs_inode	*ip,
	xfs_dablk_t		bno,
	struct xfs_buf		**bpp,
	const struct xfs_buf_ops *ops,
	int			*crc_error)
{
	int error;
	int error2;

	error = -libxfs_da_read_buf(NULL, ip, bno, 0, bpp, XFS_DATA_FORK, ops);

	if (error != EFSBADCRC && error != EFSCORRUPTED)
		return error;

	error2 = -libxfs_da_read_buf(NULL, ip, bno, 0, bpp, XFS_DATA_FORK,
			NULL);
	if (error2)
		return error2;

	(*crc_error)++;
	(*bpp)->b_ops = ops;
	return 0;
}

/*
 * Returns inode number of original file if the name already exists
 * (ie. a duplicate)
 */
static xfs_ino_t
dir_hash_add(
	struct xfs_mount	*mp,
	struct dir_hash_tab	*hashtab,
	uint32_t		addr,
	xfs_ino_t		inum,
	int			namelen,
	unsigned char		*name,
	uint8_t			ftype)
{
	xfs_dahash_t		hash = 0;
	int			byhash = 0;
	struct dir_hash_ent	*p;
	xfs_ino_t		dup_inum;
	short			junk;
	struct xfs_name		xname;
	int			error;

	xname.name = name;
	xname.len = namelen;
	xname.type = ftype;

	junk = name[0] == '/';
	dup_inum = NULLFSINO;

	if (!junk) {
		hash = libxfs_dir2_hashname(mp, &xname);
		byhash = DIR_HASH_FUNC(hashtab, hash);

		/*
		 * search hash bucket for existing name.
		 */
		for (p = hashtab->byhash[byhash]; p; p = p->nextbyhash) {
			if (p->hashval == hash && p->name.len == namelen) {
				if (memcmp(p->name.name, name, namelen) == 0) {
					dup_inum = p->inum;
					junk = 1;
					break;
				}
			}
		}
	}

	/*
	 * Allocate enough space for the hash entry and the name in a single
	 * allocation so we can store our own copy of the name for later use.
	 */
	p = calloc(1, sizeof(*p) + namelen + 1);
	if (!p)
		do_error(_("malloc failed in dir_hash_add (%zu bytes)\n"),
			sizeof(*p));

	error = radix_tree_insert(&hashtab->byaddr, addr, p);
	if (error == EEXIST) {
		do_warn(_("duplicate addrs %u in directory!\n"), addr);
		free(p);
		return 0;
	}
	radix_tree_tag_set(&hashtab->byaddr, addr, HT_UNSEEN);

	if (hashtab->last)
		hashtab->last->nextbyorder = p;
	else
		hashtab->first = p;
	p->nextbyorder = NULL;
	hashtab->last = p;

	if (!(p->junkit = junk)) {
		p->hashval = hash;
		p->nextbyhash = hashtab->byhash[byhash];
		hashtab->byhash[byhash] = p;
	}
	p->address = addr;
	p->inum = inum;
	p->seen = 0;

	/* Set up the name in the region trailing the hash entry. */
	memcpy(p->namebuf, name, namelen);
	p->name.name = p->namebuf;
	p->name.len = namelen;
	p->name.type = ftype;
	return dup_inum;
}

/* Mark an existing directory hashtable entry as junk. */
static void
dir_hash_junkit(
	struct dir_hash_tab	*hashtab,
	xfs_dir2_dataptr_t	addr)
{
	struct dir_hash_ent	*p;

	p = radix_tree_lookup(&hashtab->byaddr, addr);
	assert(p != NULL);

	p->junkit = 1;
	p->namebuf[0] = '/';
}

static int
dir_hash_check(
	struct dir_hash_tab	*hashtab,
	struct xfs_inode	*ip,
	int			seeval)
{
	static char		*seevalstr[DIR_HASH_CK_TOTAL];
	static int		done;

	if (!done) {
		seevalstr[DIR_HASH_CK_OK] = _("ok");
		seevalstr[DIR_HASH_CK_DUPLEAF] = _("duplicate leaf");
		seevalstr[DIR_HASH_CK_BADHASH] = _("hash value mismatch");
		seevalstr[DIR_HASH_CK_NODATA] = _("no data entry");
		seevalstr[DIR_HASH_CK_NOLEAF] = _("no leaf entry");
		seevalstr[DIR_HASH_CK_BADSTALE] = _("bad stale count");
		done = 1;
	}

	if (seeval == DIR_HASH_CK_OK &&
	    radix_tree_tagged(&hashtab->byaddr, HT_UNSEEN))
		seeval = DIR_HASH_CK_NOLEAF;
	if (seeval == DIR_HASH_CK_OK)
		return 0;
	do_warn(_("bad hash table for directory inode %" PRIu64 " (%s): "),
		ip->i_ino, seevalstr[seeval]);
	if (!no_modify)
		do_warn(_("rebuilding\n"));
	else
		do_warn(_("would rebuild\n"));
	return 1;
}

static void
dir_hash_done(
	struct dir_hash_tab	*hashtab)
{
	int			i;
	struct dir_hash_ent	*n;
	struct dir_hash_ent	*p;

	for (i = 0; i < hashtab->size; i++) {
		for (p = hashtab->byhash[i]; p; p = n) {
			n = p->nextbyhash;
			radix_tree_delete(&hashtab->byaddr, p->address);
			free(p);
		}
	}
	free(hashtab);
}

/*
 * Create a directory hash index structure based on the size of the directory we
 * are about to try to repair. The size passed in is the size of the data
 * segment of the directory in bytes, so we don't really know exactly how many
 * entries are in it. Hence assume an entry size of around 64 bytes - that's a
 * name length of 40+ bytes so should cover a most situations with really large
 * directories.
 */
static struct dir_hash_tab *
dir_hash_init(
	xfs_fsize_t		size)
{
	struct dir_hash_tab	*hashtab = NULL;
	int			hsize;

	hsize = size / 64;
	if (hsize < 16)
		hsize = 16;

	/*
	 * Try to allocate as large a hash table as possible. Failure to
	 * allocate isn't fatal, it will just result in slower performance as we
	 * reduce the size of the table.
	 */
	while (hsize >= 16) {
		hashtab = calloc(DIR_HASH_TAB_SIZE(hsize), 1);
		if (hashtab)
			break;
		hsize /= 2;
	}
	if (!hashtab)
		do_error(_("calloc failed in dir_hash_init\n"));
	hashtab->size = hsize;
	hashtab->byhash = (struct dir_hash_ent **)((char *)hashtab +
		sizeof(struct dir_hash_tab));
	INIT_RADIX_TREE(&hashtab->byaddr, 0);
	return hashtab;
}

static int
dir_hash_see(
	struct dir_hash_tab	*hashtab,
	xfs_dahash_t		hash,
	xfs_dir2_dataptr_t	addr)
{
	struct dir_hash_ent	*p;

	p = radix_tree_lookup(&hashtab->byaddr, addr);
	if (!p)
		return DIR_HASH_CK_NODATA;
	if (!radix_tree_tag_get(&hashtab->byaddr, addr, HT_UNSEEN))
		return DIR_HASH_CK_DUPLEAF;
	if (p->junkit == 0 && p->hashval != hash)
		return DIR_HASH_CK_BADHASH;
	radix_tree_tag_clear(&hashtab->byaddr, addr, HT_UNSEEN);
	return DIR_HASH_CK_OK;
}

static void
dir_hash_update_ftype(
	struct dir_hash_tab	*hashtab,
	xfs_dir2_dataptr_t	addr,
	uint8_t			ftype)
{
	struct dir_hash_ent	*p;

	p = radix_tree_lookup(&hashtab->byaddr, addr);
	if (!p)
		return;
	p->name.type = ftype;
}

/*
 * checks to make sure leafs match a data entry, and that the stale
 * count is valid.
 */
static int
dir_hash_see_all(
	struct dir_hash_tab	*hashtab,
	xfs_dir2_leaf_entry_t	*ents,
	int			count,
	int			stale)
{
	int			i;
	int			j;
	int			rval;

	for (i = j = 0; i < count; i++) {
		if (be32_to_cpu(ents[i].address) == XFS_DIR2_NULL_DATAPTR) {
			j++;
			continue;
		}
		rval = dir_hash_see(hashtab, be32_to_cpu(ents[i].hashval),
					be32_to_cpu(ents[i].address));
		if (rval != DIR_HASH_CK_OK)
			return rval;
	}
	return j == stale ? DIR_HASH_CK_OK : DIR_HASH_CK_BADSTALE;
}

/*
 * Given a block number in a fork, return the next valid block number (not a
 * hole).  If this is the last block number then NULLFILEOFF is returned.
 */
static int
bmap_next_offset(
	struct xfs_inode	*ip,
	xfs_fileoff_t		*bnop)
{
	xfs_fileoff_t		bno;
	int			error;
	struct xfs_bmbt_irec	got;
	struct xfs_iext_cursor	icur;

	switch (ip->i_df.if_format) {
	case XFS_DINODE_FMT_LOCAL:
		*bnop = NULLFILEOFF;
		return 0;
	case XFS_DINODE_FMT_BTREE:
	case XFS_DINODE_FMT_EXTENTS:
		break;
	default:
		return EIO;
	}

        /* Read extent map. */
	error = -libxfs_iread_extents(NULL, ip, XFS_DATA_FORK);
	if (error)
		return error;

	bno = *bnop + 1;
	if (!libxfs_iext_lookup_extent(ip, &ip->i_df, bno, &icur, &got))
		*bnop = NULLFILEOFF;
	else
		*bnop = got.br_startoff < bno ? bno : got.br_startoff;
	return 0;
}

static void
res_failed(
	int	err)
{
	if (err == ENOSPC) {
		do_error(_("ran out of disk space!\n"));
	} else
		do_error(_("xfs_trans_reserve returned %d\n"), err);
}

/*
 * Forcibly reinitialize a file that is a child of the superblock and has a
 * statically defined inumber.  These files are the root of a directory tree or
 * the realtime free space inodes.  The inode must not otherwise be in use; the
 * data fork must be empty, and the attr fork will be reset.
 */
static void
reset_sbroot_ino(
	struct xfs_trans	*tp,
	umode_t			mode,
	struct xfs_inode	*ip)
{
	struct xfs_icreate_args	args = {
		.idmap		= libxfs_nop_idmap,
		.mode		= mode,
		/* Root directories cannot be linked to a parent. */
		.flags		= XFS_ICREATE_UNLINKABLE,
	};

	/* Erase the attr fork since libxfs_inode_init won't do it for us. */
	ip->i_forkoff = 0;
	libxfs_ifork_zap_attr(ip);

	libxfs_trans_ijoin(tp, ip, 0);
	libxfs_inode_init(tp, &args, ip);
}

/*
 * Mark a newly allocated inode as metadata in the incore bitmap.  Callers
 * must have already called mark_ino_inuse to ensure there is an incore record.
 */
static void
mark_ino_metadata(
	struct xfs_mount	*mp,
	xfs_ino_t		ino)
{
	struct ino_tree_node	*irec =
		find_inode_rec(mp, XFS_INO_TO_AGNO(mp, ino),
				   XFS_INO_TO_AGINO(mp, ino));

	set_inode_is_meta(irec, get_inode_offset(mp, ino, irec));
}

/* (Re)create a missing sb-rooted rt freespace inode. */
static void
mk_rtino(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	struct xfs_inode	*ip = rtg->rtg_inodes[type];
	struct xfs_trans	*tp;
	enum xfs_metafile_type	metafile_type =
		libxfs_rtginode_metafile_type(type);
	int			error;

	error = -libxfs_trans_alloc_rollable(mp, 10, &tp);
	if (error)
		res_failed(error);

	if (!ip) {
		xfs_ino_t	rootino = mp->m_sb.sb_rootino;
		xfs_ino_t	ino = NULLFSINO;

		if (xfs_has_metadir(mp))
			rootino++;

		switch (type) {
		case XFS_RTGI_BITMAP:
			mp->m_sb.sb_rbmino = rootino + 1;
			ino = mp->m_sb.sb_rbmino;
			break;
		case XFS_RTGI_SUMMARY:
			mp->m_sb.sb_rsumino = rootino + 2;
			ino = mp->m_sb.sb_rsumino;
			break;
		default:
			break;
		}

		/*
		 * Don't use metafile iget here because we're resetting
		 * sb-rooted inodes that live at fixed inumbers, but these
		 * inodes could be in an arbitrary state.
		 */
		error = -libxfs_iget(mp, tp, ino, 0, &ip);
		if (error) {
			do_error(
_("couldn't iget realtime %s inode -- error - %d\n"),
					libxfs_rtginode_name(type),
					error);
		}

		rtg->rtg_inodes[type] = ip;
	}

	reset_sbroot_ino(tp, S_IFREG, ip);
	if (xfs_has_metadir(mp))
		libxfs_metafile_set_iflag(tp, ip, metafile_type);

	switch (type) {
	case XFS_RTGI_BITMAP:
		error = -libxfs_rtbitmap_create(rtg, ip, tp, false);
		break;
	case XFS_RTGI_SUMMARY:
		error = -libxfs_rtsummary_create(rtg, ip, tp, false);
		break;
	default:
		error = EINVAL;
	}

	if (error)
		do_error(_("%s inode re-initialization failed for rtgroup %u\n"),
			libxfs_rtginode_name(type), rtg_rgno(rtg));

	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(_("%s: commit failed, error %d\n"), __func__, error);
}

/* Mark a newly allocated inode in use in the incore bitmap. */
static void
mark_ino_inuse(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	int			mode,
	xfs_ino_t		parent)
{
	struct ino_tree_node	*irec;
	int			ino_offset;
	int			i;

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, ino),
			XFS_INO_TO_AGINO(mp, ino));

	if (irec == NULL) {
		/*
		 * This inode is allocated from a newly created inode
		 * chunk and therefore did not exist when inode chunks
		 * were processed in phase3. Add this group of inodes to
		 * the entry avl tree as if they were discovered in phase3.
		 */
		irec = set_inode_free_alloc(mp,
				XFS_INO_TO_AGNO(mp, ino),
				XFS_INO_TO_AGINO(mp, ino));
		alloc_ex_data(irec);

		for (i = 0; i < XFS_INODES_PER_CHUNK; i++)
			set_inode_free(irec, i);
	}

	ino_offset = get_inode_offset(mp, ino, irec);

	/*
	 * Mark the inode allocated so it is not skipped in phase 7.  We'll
	 * find it with the directory traverser soon, so we don't need to
	 * mark it reached.
	 */
	set_inode_used(irec, ino_offset);
	set_inode_ftype(irec, ino_offset, libxfs_mode_to_ftype(mode));
	set_inode_parent(irec, ino_offset, parent);
	if (S_ISDIR(mode))
		set_inode_isadir(irec, ino_offset);
}

static bool
ensure_rtgroup_file(
	struct xfs_rtgroup	*rtg,
	enum xfs_rtg_inodes	type)
{
	struct xfs_mount	*mp = rtg_mount(rtg);
	struct xfs_inode	*ip = rtg->rtg_inodes[type];
	const char		*name = libxfs_rtginode_name(type);
	int			error;

	if (!xfs_rtginode_enabled(rtg, type))
		return false;

	if (no_modify) {
		if (rtgroup_inodes_were_bad(type))
			do_warn(_("would reset rtgroup %u %s inode\n"),
				rtg_rgno(rtg), name);
		return false;
	}

	if (rtgroup_inodes_were_bad(type)) {
		/*
		 * The inode was bad or missing, state that we'll make a new
		 * one even though we always create a new one.
		 */
		do_warn(_("resetting rtgroup %u %s inode\n"),
			rtg_rgno(rtg), name);
	}

	error = -libxfs_rtginode_create(rtg, type, false);
	if (error)
		do_error(
_("Couldn't create rtgroup %u %s inode, error %d\n"),
			rtg_rgno(rtg), name, error);

	ip = rtg->rtg_inodes[type];

	/* Mark the inode in use. */
	mark_ino_inuse(mp, ip->i_ino, S_IFREG, mp->m_rtdirip->i_ino);
	mark_ino_metadata(mp, ip->i_ino);
	return true;
}

static void
ensure_rtgroup_bitmap(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_mount	*mp = rtg_mount(rtg);

	if (!xfs_has_rtgroups(mp))
		return;
	if (!ensure_rtgroup_file(rtg, XFS_RTGI_BITMAP))
		return;

	fill_rtbitmap(rtg);
}

static void
ensure_rtgroup_summary(
	struct xfs_rtgroup	*rtg)
{
	struct xfs_mount	*mp = rtg_mount(rtg);

	if (!xfs_has_rtgroups(mp))
		return;
	if (!ensure_rtgroup_file(rtg, XFS_RTGI_SUMMARY))
		return;

	fill_rtsummary(rtg);
}

static void
ensure_rtgroup_rmapbt(
	struct xfs_rtgroup	*rtg,
	xfs_filblks_t		est_fdblocks)
{
	if (ensure_rtgroup_file(rtg, XFS_RTGI_RMAP))
		populate_rtgroup_rmapbt(rtg, est_fdblocks);
}

static void
ensure_rtgroup_refcountbt(
	struct xfs_rtgroup	*rtg,
	xfs_filblks_t		est_fdblocks)
{
	if (ensure_rtgroup_file(rtg, XFS_RTGI_REFCOUNT))
		populate_rtgroup_refcountbt(rtg, est_fdblocks);
}

/* Initialize a root directory. */
static int
init_fs_root_dir(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	mode_t			mode,
	struct xfs_inode	**ipp)
{
	struct xfs_trans	*tp;
	struct xfs_inode	*ip = NULL;
	struct ino_tree_node	*irec;
	int			error;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_ichange, 10, 0, 0, &tp);
	if (error)
		return error;

	error = -libxfs_iget(mp, tp, ino, 0, &ip);
	if (error) {
		libxfs_trans_cancel(tp);
		return error;
	}

	/* Reset the root directory. */
	reset_sbroot_ino(tp, mode | S_IFDIR, ip);
	libxfs_dir_init(tp, ip, ip);

	error = -libxfs_trans_commit(tp);
	if (error)
		return error;

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, ino),
				XFS_INO_TO_AGINO(mp, ino));
	set_inode_isadir(irec, XFS_INO_TO_AGINO(mp, ino) - irec->ino_startnum);
	*ipp = ip;
	return 0;
}

/*
 * makes a new root directory.
 */
static void
mk_root_dir(xfs_mount_t *mp)
{
	struct xfs_inode	*ip = NULL;
	int			error;

	error = init_fs_root_dir(mp, mp->m_sb.sb_rootino, 0755, &ip);
	if (error)
		do_error(
	_("Could not reinitialize root directory inode, error %d\n"),
			error);

	libxfs_irele(ip);
}

/* Create a new metadata directory root. */
static void
mk_metadir(
	struct xfs_mount	*mp)
{
	struct xfs_trans	*tp;
	int			error;

	libxfs_rtginode_irele(&mp->m_rtdirip);

	error = init_fs_root_dir(mp, mp->m_sb.sb_metadirino, 0,
			&mp->m_metadirip);
	if (error)
		do_error(
	_("Initialization of the metadata root directory failed, error %d\n"),
			error);

	/* Mark the new metadata root dir as metadata. */
	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_ichange, 0, 0, 0, &tp);
	if (error)
		do_error(
	_("Marking metadata root directory failed"));

	libxfs_trans_ijoin(tp, mp->m_metadirip, 0);
	libxfs_metafile_set_iflag(tp, mp->m_metadirip, XFS_METAFILE_DIR);
	mark_ino_metadata(mp, mp->m_metadirip->i_ino);

	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(
	_("Marking metadata root directory failed, error %d\n"), error);
}

/*
 * orphanage name == lost+found
 */
static xfs_ino_t
mk_orphanage(
	struct xfs_mount	*mp)
{
	struct xfs_icreate_args	args = {
		.idmap		= libxfs_nop_idmap,
		.mode		= S_IFDIR | 0755,
	};
	struct xfs_name		xname = {
		.name		= (unsigned char *)ORPHANAGE,
		.len		= strlen(ORPHANAGE),
		.type		= XFS_DIR3_FT_DIR,
	};
	struct xfs_dir_update	du = {
		.name		= &xname,
	};
	struct xfs_trans	*tp;
	struct ino_tree_node	*irec;
	xfs_ino_t		ino;
	int			ino_offset = 0;
	int			i;
	int			error;
	int			nres;

	i = -libxfs_parent_start(mp, &du.ppargs);
	if (i)
		do_error(_("%d - couldn't allocate parent pointer for %s\n"),
			i, ORPHANAGE);

	/*
	 * check for an existing lost+found first, if it exists, return
	 * its inode. Otherwise, we can create it. Bad lost+found inodes
	 * would have been cleared in phase3 and phase4.
	 */

	i = -libxfs_iget(mp, NULL, mp->m_sb.sb_rootino, 0, &du.dp);
	if (i)
		do_error(_("%d - couldn't iget root inode to obtain %s\n"),
			i, ORPHANAGE);

	args.pip = du.dp;

	/* If the lookup of /lost+found succeeds, return the inumber. */
	error = -libxfs_dir_lookup(NULL, du.dp, &xname, &ino, NULL);
	if (error == 0)
		goto out_pip;

	/*
	 * could not be found, create it
	 */
	nres = libxfs_mkdir_space_res(mp, xname.len);
	i = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_mkdir, nres, 0, 0, &tp);
	if (i)
		res_failed(i);

	error = -libxfs_dialloc(&tp, &args, &ino);
	if (error)
		do_error(_("%s inode allocation failed %d\n"),
			ORPHANAGE, error);

	error = -libxfs_icreate(tp, ino, &args, &du.ip);
	if (error)
		do_error(_("%s inode initialization failed %d\n"),
			ORPHANAGE, error);

	irec = find_inode_rec(mp,
			XFS_INO_TO_AGNO(mp, ino),
			XFS_INO_TO_AGINO(mp, ino));

	if (irec == NULL) {
		/*
		 * This inode is allocated from a newly created inode
		 * chunk and therefore did not exist when inode chunks
		 * were processed in phase3. Add this group of inodes to
		 * the entry avl tree as if they were discovered in phase3.
		 */
		irec = set_inode_free_alloc(mp, XFS_INO_TO_AGNO(mp, ino),
					    XFS_INO_TO_AGINO(mp, ino));
		alloc_ex_data(irec);

		for (i = 0; i < XFS_INODES_PER_CHUNK; i++)
			set_inode_free(irec, i);
	}

	ino_offset = get_inode_offset(mp, ino, irec);

	/*
	 * Mark the inode allocated to lost+found as used in the AVL tree
	 * so it is not skipped in phase 7
	 */
	set_inode_used(irec, ino_offset);
	add_inode_ref(irec, ino_offset);
	add_inode_reached(irec, ino_offset);

	/*
	 * now that we know the transaction will stay around,
	 * add the root inode to it
	 */
	libxfs_trans_ijoin(tp, du.dp, 0);

	/*
	 * create the actual entry
	 */
	error = -libxfs_dir_create_child(tp, nres, &du);
	if (error)
		do_error(
		_("can't make %s, createname error %d\n"),
			ORPHANAGE, error);
	add_parent_ptr(du.ip->i_ino, (unsigned char *)ORPHANAGE, du.dp, false);

	/*
	 * We bumped up the link count in the root directory to account
	 * for .. in the new directory, so now update the irec copy of the
	 * on-disk nlink so we don't fail the link count check later.
	 */
	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino),
				  XFS_INO_TO_AGINO(mp, mp->m_sb.sb_rootino));
	add_inode_ref(irec, 0);
	set_inode_disk_nlinks(irec, 0, get_inode_disk_nlinks(irec, 0) + 1);

	error = -libxfs_trans_commit(tp);
	if (error) {
		do_error(_("%s directory creation failed -- bmapf error %d\n"),
			ORPHANAGE, error);
	}
	libxfs_irele(du.ip);
out_pip:
	libxfs_irele(du.dp);
	libxfs_parent_finish(mp, du.ppargs);

	return(ino);
}

/* Don't let metadata inode contents leak to lost+found. */
static void
trunc_metadata_inode(
	struct xfs_inode	*ip)
{
	struct xfs_trans	*tp;
	struct xfs_mount	*mp = ip->i_mount;
	int			err;

	err = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_ichange, 0, 0, 0, &tp);
	if (err)
		do_error(
	_("space reservation failed (%d), filesystem may be out of space\n"),
					err);

	libxfs_trans_ijoin(tp, ip, 0);
	ip->i_diflags2 &= ~XFS_DIFLAG2_METADATA;

	switch (VFS_I(ip)->i_mode & S_IFMT) {
	case S_IFIFO:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFSOCK:
		ip->i_df.if_format = XFS_DINODE_FMT_DEV;
		break;
	case S_IFREG:
		switch (ip->i_df.if_format) {
		case XFS_DINODE_FMT_EXTENTS:
		case XFS_DINODE_FMT_BTREE:
			break;
		default:
			ip->i_df.if_format = XFS_DINODE_FMT_EXTENTS;
			ip->i_df.if_nextents = 0;
			break;
		}
		break;
	}

	libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);

	err = -libxfs_trans_commit(tp);
	if (err)
		do_error(
	_("truncation of metadata inode 0x%llx failed, err=%d\n"),
				(unsigned long long)ip->i_ino, err);
}

/*
 * Add a parent pointer back to the orphanage for any file we're moving into
 * the orphanage, being careful not to trip over any existing parent pointer.
 * You never know when the orphanage might get corrupted.
 */
static void
add_orphan_pptr(
	struct xfs_trans	*tp,
	struct xfs_inode	*orphanage_ip,
	const struct xfs_name	*xname,
	struct xfs_inode	*ip,
	struct xfs_parent_args	*ppargs)
{
	struct xfs_parent_rec	pptr = { };
	struct xfs_da_args	scratch;
	int			error;

	xfs_inode_to_parent_rec(&pptr, orphanage_ip);
	error = -libxfs_parent_lookup(tp, ip, xname, &pptr, &scratch);
	if (!error)
		return;
	if (error != ENOATTR)
		do_log(
 _("cannot look up parent pointer for '%.*s', err %d\n"),
				xname->len, xname->name, error);

	if (!xfs_inode_has_attr_fork(ip)) {
		error = -libxfs_bmap_add_attrfork(tp, ip,
				sizeof(struct xfs_attr_sf_hdr), true);
		if (error)
			do_error(_("can't add attr fork to inode 0x%llx\n"),
					(unsigned long long)ip->i_ino);
	}

	error = -libxfs_parent_addname(tp, ppargs, orphanage_ip, xname, ip);
	if (error)
		do_error(
 _("can't add parent pointer for '%.*s', error %d\n"),
				xname->len, xname->name, error);
}

/*
 * move a file to the orphange.
 */
static void
mv_orphanage(
	xfs_mount_t		*mp,
	xfs_ino_t		ino,		/* inode # to be moved */
	int			isa_dir)	/* 1 if inode is a directory */
{
	xfs_inode_t		*orphanage_ip;
	xfs_ino_t		entry_ino_num;
	xfs_inode_t		*ino_p;
	xfs_trans_t		*tp;
	int			err;
	unsigned char		fname[MAXPATHLEN + 1];
	int			nres;
	int			incr;
	ino_tree_node_t		*irec;
	int			ino_offset = 0;
	struct xfs_name		xname;
	struct xfs_parent_args	*ppargs;

	err = -libxfs_parent_start(mp, &ppargs);
	if (err)
		do_error(
 _("%d - couldn't allocate parent pointer for lost inode\n"),
			err);

	xname.name = fname;
	xname.len = snprintf((char *)fname, sizeof(fname), "%llu",
				(unsigned long long)ino);

	err = -libxfs_iget(mp, NULL, orphanage_ino, 0, &orphanage_ip);
	if (err)
		do_error(_("%d - couldn't iget orphanage inode\n"), err);
	/*
	 * Make sure the filename is unique in the lost+found
	 */
	incr = 0;
	while (libxfs_dir_lookup(NULL, orphanage_ip, &xname, &entry_ino_num,
								NULL) == 0)
		xname.len = snprintf((char *)fname, sizeof(fname), "%llu.%d",
					(unsigned long long)ino, ++incr);

	/* Orphans may not have a proper parent, so use custom ops here */
	err = -libxfs_iget(mp, NULL, ino, 0, &ino_p);
	if (err)
		do_error(_("%d - couldn't iget disconnected inode\n"), err);

	if (xfs_is_metadir_inode(ino_p))
		trunc_metadata_inode(ino_p);

	xname.type = libxfs_mode_to_ftype(VFS_I(ino_p)->i_mode);

	if (isa_dir)  {
		irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, orphanage_ino),
				XFS_INO_TO_AGINO(mp, orphanage_ino));
		if (irec)
			ino_offset = XFS_INO_TO_AGINO(mp, orphanage_ino) -
					irec->ino_startnum;
		nres = XFS_DIRENTER_SPACE_RES(mp, fnamelen) +
		       XFS_DIRENTER_SPACE_RES(mp, 2);
		err = -libxfs_dir_lookup(NULL, ino_p, &xfs_name_dotdot,
					&entry_ino_num, NULL);
		if (err) {
			ASSERT(err == ENOENT);

			err = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_rename,
						  nres, 0, 0, &tp);
			if (err)
				res_failed(err);

			libxfs_trans_ijoin(tp, orphanage_ip, 0);
			libxfs_trans_ijoin(tp, ino_p, 0);

			err = -libxfs_dir_createname(tp, orphanage_ip, &xname,
						ino, nres);
			if (err)
				do_error(
	_("name create failed in %s (%d)\n"), ORPHANAGE, err);

			if (ppargs)
				add_orphan_pptr(tp, orphanage_ip, &xname,
						ino_p, ppargs);

			if (irec)
				add_inode_ref(irec, ino_offset);
			else
				libxfs_bumplink(tp, orphanage_ip);
			libxfs_trans_log_inode(tp, orphanage_ip, XFS_ILOG_CORE);

			err = -libxfs_dir_createname(tp, ino_p, &xfs_name_dotdot,
					orphanage_ino, nres);
			if (err)
				do_error(
	_("creation of .. entry failed (%d)\n"), err);

			libxfs_bumplink(tp, ino_p);
			libxfs_trans_log_inode(tp, ino_p, XFS_ILOG_CORE);
			err = -libxfs_trans_commit(tp);
			if (err)
				do_error(
	_("creation of .. entry failed (%d)\n"), err);
		} else  {
			err = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_rename,
						  nres, 0, 0, &tp);
			if (err)
				res_failed(err);

			libxfs_trans_ijoin(tp, orphanage_ip, 0);
			libxfs_trans_ijoin(tp, ino_p, 0);


			err = -libxfs_dir_createname(tp, orphanage_ip, &xname,
						ino, nres);
			if (err)
				do_error(
	_("name create failed in %s (%d)\n"), ORPHANAGE, err);

			if (ppargs)
				add_orphan_pptr(tp, orphanage_ip, &xname,
						ino_p, ppargs);

			if (irec)
				add_inode_ref(irec, ino_offset);
			else
				libxfs_bumplink(tp, orphanage_ip);
			libxfs_trans_log_inode(tp, orphanage_ip, XFS_ILOG_CORE);

			/*
			 * don't replace .. value if it already points
			 * to us.  that'll pop a libxfs/kernel ASSERT.
			 */
			if (entry_ino_num != orphanage_ino)  {
				err = -libxfs_dir_replace(tp, ino_p,
						&xfs_name_dotdot, orphanage_ino,
						nres);
				if (err)
					do_error(
	_("name replace op failed (%d)\n"), err);
			}

			err = -libxfs_trans_commit(tp);
			if (err)
				do_error(
	_("orphanage name replace op failed (%d)\n"), err);
		}

	} else  {
		/*
		 * use the remove log reservation as that's
		 * more accurate.  we're only creating the
		 * links, we're not doing the inode allocation
		 * also accounted for in the create
		 */
		nres = XFS_DIRENTER_SPACE_RES(mp, xname.len);
		err = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove,
					  nres, 0, 0, &tp);
		if (err)
			res_failed(err);

		libxfs_trans_ijoin(tp, orphanage_ip, 0);
		libxfs_trans_ijoin(tp, ino_p, 0);

		err = -libxfs_dir_createname(tp, orphanage_ip, &xname, ino,
						nres);
		if (err)
			do_error(
	_("name create failed in %s (%d)\n"), ORPHANAGE, err);
		ASSERT(err == 0);

		if (ppargs)
			add_orphan_pptr(tp, orphanage_ip, &xname, ino_p,
					ppargs);

		set_nlink(VFS_I(ino_p), 1);
		libxfs_trans_log_inode(tp, ino_p, XFS_ILOG_CORE);
		err = -libxfs_trans_commit(tp);
		if (err)
			do_error(
	_("orphanage name create failed (%d)\n"), err);
	}

	if (xfs_has_parent(mp))
		add_parent_ptr(ino_p->i_ino, xname.name, orphanage_ip, false);

	libxfs_irele(ino_p);
	libxfs_irele(orphanage_ip);
	libxfs_parent_finish(mp, ppargs);
}

static int
entry_junked(
	const char 	*msg,
	const char	*iname,
	xfs_ino_t	ino1,
	xfs_ino_t	ino2,
	xfs_ino_t	ino3)
{
	if(ino3 != NULLFSINO)
		do_warn(msg, iname, ino1, ino2, ino3);
	else
		do_warn(msg, iname, ino1, ino2);
	if (!no_modify)
		do_warn(_("junking entry\n"));
	else
		do_warn(_("would junk entry\n"));
	return !no_modify;
}

/* Find and invalidate all the directory's buffers. */
static int
dir_binval(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	int			whichfork)
{
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	rec;
	struct xfs_ifork	*ifp;
	struct xfs_da_geometry	*geo;
	struct xfs_buf		*bp;
	xfs_dablk_t		dabno;
	int			error = 0;

	if (ip->i_df.if_format != XFS_DINODE_FMT_EXTENTS &&
	    ip->i_df.if_format != XFS_DINODE_FMT_BTREE)
		return 0;

	geo = tp->t_mountp->m_dir_geo;
	ifp = xfs_ifork_ptr(ip, XFS_DATA_FORK);
	for_each_xfs_iext(ifp, &icur, &rec) {
		for (dabno = roundup(rec.br_startoff, geo->fsbcount);
		     dabno < rec.br_startoff + rec.br_blockcount;
		     dabno += geo->fsbcount) {
			bp = NULL;
			error = -libxfs_da_get_buf(tp, ip, dabno, &bp,
					whichfork);
			if (error)
				return error;
			if (!bp)
				continue;
			libxfs_trans_binval(tp, bp);
			libxfs_trans_brelse(tp, bp);
		}
	}

	return error;
}

/*
 * Unexpected failure during the rebuild will leave the entries in
 * lost+found on the next run
 */

static void
longform_dir2_rebuild(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_inode	*ip,
	struct ino_tree_node	*irec,
	int			ino_offset,
	struct dir_hash_tab	*hashtab)
{
	int			error;
	int			nres;
	struct xfs_trans	*tp;
	xfs_fileoff_t		lastblock;
	struct xfs_inode	pip;
	struct dir_hash_ent	*p;
	int			done = 0;

	/*
	 * trash directory completely and rebuild from scratch using the
	 * name/inode pairs in the hash table
	 */

	do_warn(_("rebuilding directory inode %" PRIu64 "\n"), ino);

	/*
	 * first attempt to locate the parent inode, if it can't be
	 * found, set it to the root inode and it'll be moved to the
	 * orphanage later (the inode number here needs to be valid
	 * for the libxfs_dir_init() call).
	 */
	pip.i_ino = get_inode_parent(irec, ino_offset);
	if (pip.i_ino == NULLFSINO ||
	    libxfs_dir_ino_validate(mp, pip.i_ino))
		pip.i_ino = mp->m_sb.sb_rootino;

	nres = libxfs_remove_space_res(mp, 0);
	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove, nres, 0, 0, &tp);
	if (error)
		res_failed(error);
	libxfs_trans_ijoin(tp, ip, 0);

	error = dir_binval(tp, ip, XFS_DATA_FORK);
	if (error)
		do_error(_("error %d invalidating directory %llu blocks\n"),
				error, (unsigned long long)ip->i_ino);

	if ((error = -libxfs_bmap_last_offset(ip, &lastblock, XFS_DATA_FORK)))
		do_error(_("xfs_bmap_last_offset failed -- error - %d\n"),
			error);

	/* free all data, leaf, node and freespace blocks */
	while (!done) {
	       error = -libxfs_bunmapi(tp, ip, 0, lastblock, XFS_BMAPI_METADATA,
			               0, &done);
	       if (error) {
		       do_warn(_("xfs_bunmapi failed -- error - %d\n"), error);
		       goto out_bmap_cancel;
	       }
	       error = -libxfs_defer_finish(&tp);
	       if (error) {
		       do_warn(("defer_finish failed -- error - %d\n"), error);
		       goto out_bmap_cancel;
	       }
	       /*
		* Close out trans and start the next one in the chain.
		*/
	       error = -libxfs_trans_roll_inode(&tp, ip);
	       if (error)
			goto out_bmap_cancel;
        }

	error = -libxfs_dir_init(tp, ip, &pip);
	if (error) {
		do_warn(_("xfs_dir_init failed -- error - %d\n"), error);
		goto out_bmap_cancel;
	}

	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(
	_("dir init failed (%d)\n"), error);

	if (ino == mp->m_sb.sb_rootino)
		need_root_dotdot = 0;
	else if (ino == mp->m_sb.sb_metadirino)
		need_metadir_dotdot = 0;

	/* go through the hash list and re-add the inodes */

	for (p = hashtab->first; p; p = p->nextbyorder) {
		if (p->junkit)
			continue;
		if (p->name.name[0] == '/' || (p->name.name[0] == '.' &&
				(p->name.len == 1 || (p->name.len == 2 &&
						p->name.name[1] == '.'))))
			continue;

		nres = libxfs_create_space_res(mp, p->name.len);
		error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_create,
					    nres, 0, 0, &tp);
		if (error)
			res_failed(error);

		libxfs_trans_ijoin(tp, ip, 0);

		error = -libxfs_dir_createname(tp, ip, &p->name, p->inum,
						nres);
		if (error) {
			do_warn(
_("name create failed in ino %" PRIu64 " (%d)\n"), ino, error);
			goto out_bmap_cancel;
		}

		error = -libxfs_trans_commit(tp);
		if (error)
			do_error(
_("name create failed (%d) during rebuild\n"), error);
	}

	/*
	 * If we added too few entries to retain longform, add the extra
	 * ref for . as this is now a shortform directory.
	 */
	if (ip->i_df.if_format == XFS_DINODE_FMT_LOCAL)
		add_inode_ref(irec, ino_offset);

	return;

out_bmap_cancel:
	libxfs_trans_cancel(tp);
	return;
}


/*
 * Kill a block in a version 2 inode.
 * Makes its own transaction.
 */
static void
dir2_kill_block(
	xfs_mount_t	*mp,
	xfs_inode_t	*ip,
	xfs_dablk_t	da_bno,
	struct xfs_buf	*bp)
{
	xfs_da_args_t	args;
	int		error;
	int		nres;
	xfs_trans_t	*tp;

	nres = libxfs_remove_space_res(mp, 0);
	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove, nres, 0, 0, &tp);
	if (error)
		res_failed(error);
	libxfs_trans_ijoin(tp, ip, 0);
	libxfs_trans_bjoin(tp, bp);
	libxfs_trans_bhold(tp, bp);
	memset(&args, 0, sizeof(args));
	args.dp = ip;
	args.trans = tp;
	args.whichfork = XFS_DATA_FORK;
	args.geo = mp->m_dir_geo;
	args.owner = ip->i_ino;
	if (da_bno >= mp->m_dir_geo->leafblk && da_bno < mp->m_dir_geo->freeblk)
		error = -libxfs_da_shrink_inode(&args, da_bno, bp);
	else
		error = -libxfs_dir2_shrink_inode(&args,
				xfs_dir2_da_to_db(mp->m_dir_geo, da_bno), bp);
	if (error)
		do_error(_("shrink_inode failed inode %" PRIu64 " block %u\n"),
			ip->i_ino, da_bno);
	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(
_("directory shrink failed (%d)\n"), error);
}

static inline void
check_longform_ftype(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip,
	xfs_dir2_data_entry_t	*dep,
	ino_tree_node_t		*irec,
	int			ino_offset,
	struct dir_hash_tab	*hashtab,
	xfs_dir2_dataptr_t	addr,
	struct xfs_da_args	*da,
	struct xfs_buf		*bp)
{
	xfs_ino_t		inum = be64_to_cpu(dep->inumber);
	uint8_t			dir_ftype;
	uint8_t			ino_ftype;

	if (!xfs_has_ftype(mp))
		return;

	dir_ftype = libxfs_dir2_data_get_ftype(mp, dep);
	ino_ftype = get_inode_ftype(irec, ino_offset);

	if (dir_ftype == ino_ftype)
		return;

	if (no_modify) {
		do_warn(
_("would fix ftype mismatch (%d/%d) in directory/child inode %" PRIu64 "/%" PRIu64 "\n"),
			dir_ftype, ino_ftype,
			ip->i_ino, inum);
		return;
	}

	do_warn(
_("fixing ftype mismatch (%d/%d) in directory/child inode %" PRIu64 "/%" PRIu64 "\n"),
		dir_ftype, ino_ftype,
		ip->i_ino, inum);
	libxfs_dir2_data_put_ftype(mp, dep, ino_ftype);
	libxfs_dir2_data_log_entry(da, bp, dep);
	dir_hash_update_ftype(hashtab, addr, ino_ftype);
}

/*
 * process a data block, also checks for .. entry
 * and corrects it to match what we think .. should be
 */
static void
longform_dir2_entry_check_data(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip,
	int			*num_illegal,
	int			*need_dot,
	struct ino_tree_node	*current_irec,
	int			current_ino_offset,
	struct xfs_buf		*bp,
	struct dir_hash_tab	*hashtab,
	freetab_t		**freetabp,
	xfs_dablk_t		da_bno,
	bool			isblock)
{
	xfs_dir2_dataptr_t	addr;
	xfs_dir2_leaf_entry_t	*blp;
	xfs_dir2_block_tail_t	*btp;
	struct xfs_dir2_data_hdr *d;
	xfs_dir2_db_t		db;
	xfs_dir2_data_entry_t	*dep;
	xfs_dir2_data_unused_t	*dup;
	struct xfs_dir2_data_free *bf;
	char			*endptr;
	int			error;
	char			fname[MAXNAMELEN + 1];
	freetab_t		*freetab;
	int			i;
	int			ino_offset;
	xfs_ino_t		inum;
	xfs_ino_t		dup_inum;
	ino_tree_node_t		*irec;
	int			junkit;
	int			lastfree;
	int			len;
	int			nbad;
	int			needlog;
	int			needscan;
	xfs_ino_t		parent;
	char			*ptr;
	xfs_trans_t		*tp;
	int			wantmagic;
	struct xfs_da_args	da = {
		.dp = ip,
		.geo = mp->m_dir_geo,
		.owner = ip->i_ino,
	};


	d = bp->b_addr;
	ptr = (char *)d + mp->m_dir_geo->data_entry_offset;
	nbad = 0;
	needscan = needlog = 0;
	junkit = 0;
	freetab = *freetabp;
	if (isblock) {
		btp = xfs_dir2_block_tail_p(mp->m_dir_geo, d);
		blp = xfs_dir2_block_leaf_p(btp);
		endptr = (char *)blp;
		if (endptr > (char *)btp)
			endptr = (char *)btp;
		if (xfs_has_crc(mp))
			wantmagic = XFS_DIR3_BLOCK_MAGIC;
		else
			wantmagic = XFS_DIR2_BLOCK_MAGIC;
	} else {
		endptr = (char *)d + mp->m_dir_geo->blksize;
		if (xfs_has_crc(mp))
			wantmagic = XFS_DIR3_DATA_MAGIC;
		else
			wantmagic = XFS_DIR2_DATA_MAGIC;
	}
	db = xfs_dir2_da_to_db(mp->m_dir_geo, da_bno);

	/* check for data block beyond expected end */
	if (freetab->naents <= db) {
		struct freetab_ent e;

		*freetabp = freetab = realloc(freetab, FREETAB_SIZE(db + 1));
		if (!freetab) {
			do_error(_("realloc failed in %s (%zu bytes)\n"),
				__func__, FREETAB_SIZE(db + 1));
		}
		e.v = NULLDATAOFF;
		e.s = 0;
		for (i = freetab->naents; i < db; i++)
			freetab->ents[i] = e;
		freetab->naents = db + 1;
	}

	/* check the data block */
	while (ptr < endptr) {

		/* check for freespace */
		dup = (xfs_dir2_data_unused_t *)ptr;
		if (XFS_DIR2_DATA_FREE_TAG == be16_to_cpu(dup->freetag)) {

			/* check for invalid freespace length */
			if (ptr + be16_to_cpu(dup->length) > endptr ||
					be16_to_cpu(dup->length) == 0 ||
					(be16_to_cpu(dup->length) &
						(XFS_DIR2_DATA_ALIGN - 1)))
				break;

			/* check for invalid tag */
			if (be16_to_cpu(*xfs_dir2_data_unused_tag_p(dup)) !=
						(char *)dup - (char *)d)
				break;

			/* check for block with no data entries */
			if ((ptr == (char *)d + mp->m_dir_geo->data_entry_offset) &&
			    (ptr + be16_to_cpu(dup->length) >= endptr)) {
				junkit = 1;
				*num_illegal += 1;
				break;
			}

			/* continue at the end of the freespace */
			ptr += be16_to_cpu(dup->length);
			if (ptr >= endptr)
				break;
		}

		/* validate data entry size */
		dep = (xfs_dir2_data_entry_t *)ptr;
		if (ptr + libxfs_dir2_data_entsize(mp, dep->namelen) > endptr)
			break;
		if (be16_to_cpu(*libxfs_dir2_data_entry_tag_p(mp, dep)) !=
						(char *)dep - (char *)d)
			break;
		ptr += libxfs_dir2_data_entsize(mp, dep->namelen);
	}

	/* did we find an empty or corrupt block? */
	if (ptr != endptr) {
		if (junkit) {
			do_warn(
	_("empty data block %u in directory inode %" PRIu64 ": "),
				da_bno, ip->i_ino);
		} else {
			do_warn(_
	("corrupt block %u in directory inode %" PRIu64 ": "),
				da_bno, ip->i_ino);
		}
		if (!no_modify) {
			do_warn(_("junking block\n"));
			dir2_kill_block(mp, ip, da_bno, bp);
		} else {
			do_warn(_("would junk block\n"));
		}
		freetab->ents[db].v = NULLDATAOFF;
		return;
	}

	/* update number of data blocks processed */
	if (freetab->nents < db + 1)
		freetab->nents = db + 1;

	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove, 0, 0, 0, &tp);
	if (error)
		res_failed(error);
	da.trans = tp;
	libxfs_trans_ijoin(tp, ip, 0);
	libxfs_trans_bjoin(tp, bp);
	libxfs_trans_bhold(tp, bp);
	if (be32_to_cpu(d->magic) != wantmagic) {
		do_warn(
	_("bad directory block magic # %#x for directory inode %" PRIu64 " block %d: "),
			be32_to_cpu(d->magic), ip->i_ino, da_bno);
		if (!no_modify) {
			do_warn(_("fixing magic # to %#x\n"), wantmagic);
			d->magic = cpu_to_be32(wantmagic);
			needlog = 1;
		} else
			do_warn(_("would fix magic # to %#x\n"), wantmagic);
	}
	lastfree = 0;
	ptr = (char *)d + mp->m_dir_geo->data_entry_offset;
	/*
	 * look at each entry.  reference inode pointed to by each
	 * entry in the incore inode tree.
	 * if not a directory, set reached flag, increment link count
	 * if a directory and reached, mark entry as to be deleted.
	 * if a directory, check to see if recorded parent
	 *	matches current inode #,
	 *	if so, then set reached flag, increment link count
	 *		of current and child dir inodes, push the child
	 *		directory inode onto the directory stack.
	 *	if current inode != parent, then mark entry to be deleted.
	 */
	while (ptr < endptr) {
		dup = (xfs_dir2_data_unused_t *)ptr;
		if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
			if (lastfree) {
				do_warn(
	_("directory inode %" PRIu64 " block %u has consecutive free entries: "),
					ip->i_ino, da_bno);
				if (!no_modify) {

					do_warn(_("joining together\n"));
					len = be16_to_cpu(dup->length);
					libxfs_dir2_data_use_free(&da, bp, dup,
						ptr - (char *)d, len, &needlog,
						&needscan);
					libxfs_dir2_data_make_free(&da, bp,
						ptr - (char *)d, len, &needlog,
						&needscan);
				} else
					do_warn(_("would join together\n"));
			}
			ptr += be16_to_cpu(dup->length);
			lastfree = 1;
			continue;
		}
		addr = xfs_dir2_db_off_to_dataptr(mp->m_dir_geo, db,
						  ptr - (char *)d);
		dep = (xfs_dir2_data_entry_t *)ptr;
		ptr += libxfs_dir2_data_entsize(mp, dep->namelen);
		inum = be64_to_cpu(dep->inumber);
		lastfree = 0;
		/*
		 * skip bogus entries (leading '/').  they'll be deleted
		 * later.  must still log it, else we leak references to
		 * buffers.
		 */
		if (dep->name[0] == '/')  {
			nbad++;
			if (!no_modify)
				libxfs_dir2_data_log_entry(&da, bp, dep);
			continue;
		}

		memmove(fname, dep->name, dep->namelen);
		fname[dep->namelen] = '\0';
		ASSERT(inum != NULLFSINO);

		irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, inum),
					XFS_INO_TO_AGINO(mp, inum));
		if (irec == NULL)  {
			nbad++;
			if (entry_junked(
	_("entry \"%s\" in directory inode %" PRIu64 " points to non-existent inode %" PRIu64 ", "),
					fname, ip->i_ino, inum, NULLFSINO)) {
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
			}
			continue;
		}
		ino_offset = XFS_INO_TO_AGINO(mp, inum) - irec->ino_startnum;

		/*
		 * if it's a free inode, blow out the entry.
		 * by now, any inode that we think is free
		 * really is free.
		 */
		if (is_inode_free(irec, ino_offset))  {
			nbad++;
			if (entry_junked(
	_("entry \"%s\" in directory inode %" PRIu64 " points to free inode %" PRIu64 ", "),
					fname, ip->i_ino, inum, NULLFSINO)) {
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
			}
			continue;
		}

		/*
		 * Regular directories cannot point to metadata files.  If
		 * we find such a thing, blow out the entry.
		 */
		if (!xfs_is_metadir_inode(ip) &&
		    inode_is_meta(irec, ino_offset)) {
			nbad++;
			if (entry_junked(
	_("entry \"%s\" in regular dir %" PRIu64" points to a metadata inode %" PRIu64 ", "),
					fname, ip->i_ino, inum, NULLFSINO)) {
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
			}
			continue;
		}

		/*
		 * Metadata directories cannot point to regular files.  If
		 * we find such a thing, blow out the entry.
		 */
		if (xfs_is_metadir_inode(ip) &&
		    !inode_is_meta(irec, ino_offset)) {
			nbad++;
			if (entry_junked(
	_("entry \"%s\" in metadata dir %" PRIu64" points to a regular inode %" PRIu64 ", "),
					fname, ip->i_ino, inum, NULLFSINO)) {
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
			}
			continue;
		}

		/*
		 * check if this inode is lost+found dir in the root
		 */
		if (inum == mp->m_sb.sb_rootino && strcmp(fname, ORPHANAGE) == 0) {
			/*
			 * if it's not a directory, trash it
			 */
			if (!inode_isadir(irec, ino_offset)) {
				nbad++;
				if (entry_junked(
	_("%s (ino %" PRIu64 ") in root (%" PRIu64 ") is not a directory, "),
						ORPHANAGE, inum, ip->i_ino, NULLFSINO)) {
					dep->name[0] = '/';
					libxfs_dir2_data_log_entry(&da, bp, dep);
				}
				continue;
			}
			/*
			 * if this is a dup, it will be picked up below,
			 * otherwise, mark it as the orphanage for later.
			 */
			if (!orphanage_ino)
				orphanage_ino = inum;
		}

		/*
		 * check for duplicate names in directory.
		 */
		dup_inum = dir_hash_add(mp, hashtab, addr, inum, dep->namelen,
				dep->name, libxfs_dir2_data_get_ftype(mp, dep));
		if (dup_inum != NULLFSINO) {
			nbad++;
			if (entry_junked(
	_("entry \"%s\" (ino %" PRIu64 ") in dir %" PRIu64 " already points to ino %" PRIu64 ", "),
					fname, inum, ip->i_ino, dup_inum)) {
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
			}
			if (inum == orphanage_ino)
				orphanage_ino = 0;
			continue;
		}

		/*
		 * if just scanning to rebuild a directory due to a ".."
		 * update, just continue
		 */
		if (dotdot_update)
			continue;

		/*
		 * skip the '..' entry since it's checked when the
		 * directory is reached by something else.  if it never
		 * gets reached, it'll be moved to the orphanage and we'll
		 * take care of it then. If it doesn't exist at all, the
		 * directory needs to be rebuilt first before being added
		 * to the orphanage.
		 */
		if (dep->namelen == 2 && dep->name[0] == '.' &&
				dep->name[1] == '.') {
			if (da_bno != 0) {
				/* ".." should be in the first block */
				nbad++;
				if (entry_junked(
	_("entry \"%s\" (ino %" PRIu64 ") in dir %" PRIu64 " is not in the the first block, "), fname,
						inum, ip->i_ino, NULLFSINO)) {
					dir_hash_junkit(hashtab, addr);
					dep->name[0] = '/';
					libxfs_dir2_data_log_entry(&da, bp, dep);
				}
			}

			if (!nbad)
				check_longform_ftype(mp, ip, dep, irec,
						ino_offset, hashtab, addr, &da,
						bp);
			continue;
		}
		ASSERT(no_modify || libxfs_verify_dir_ino(mp, inum));
		/*
		 * special case the . entry.  we know there's only one
		 * '.' and only '.' points to itself because bogus entries
		 * got trashed in phase 3 if there were > 1.
		 * bump up link count for '.' but don't set reached
		 * until we're actually reached by another directory
		 * '..' is already accounted for or will be taken care
		 * of when directory is moved to orphanage.
		 */
		if (ip->i_ino == inum)  {
			ASSERT(no_modify ||
			       (dep->name[0] == '.' && dep->namelen == 1));
			add_inode_ref(current_irec, current_ino_offset);
			if (da_bno != 0 ||
			    dep != (void *)d + mp->m_dir_geo->data_entry_offset) {
				/* "." should be the first entry */
				nbad++;
				if (entry_junked(
	_("entry \"%s\" in dir %" PRIu64 " is not the first entry, "),
						fname, inum, ip->i_ino, NULLFSINO)) {
					dir_hash_junkit(hashtab, addr);
					dep->name[0] = '/';
					libxfs_dir2_data_log_entry(&da, bp, dep);
				}
			}

			if (!nbad)
				check_longform_ftype(mp, ip, dep, irec,
						ino_offset, hashtab, addr, &da,
						bp);
			*need_dot = 0;
			continue;
		}
		/*
		 * skip entries with bogus inumbers if we're in no modify mode
		 */
		if (no_modify && !libxfs_verify_dir_ino(mp, inum))
			continue;

		/* validate ftype field if supported */
		check_longform_ftype(mp, ip, dep, irec, ino_offset, hashtab,
				addr, &da, bp);

		/*
		 * check easy case first, regular inode, just bump
		 * the link count and continue
		 */
		if (!inode_isadir(irec, ino_offset))  {
			add_inode_reached(irec, ino_offset);
			continue;
		}
		parent = get_inode_parent(irec, ino_offset);
		if (parent == 0) {
			if (no_modify)
				do_warn(
 _("unknown parent for inode %" PRIu64 "\n"),
						inum);
			else
				ASSERT(parent != 0);
		}
		junkit = 0;
		/*
		 * bump up the link counts in parent and child
		 * directory but if the link doesn't agree with
		 * the .. in the child, blow out the entry.
		 * if the directory has already been reached,
		 * blow away the entry also.
		 */
		if (is_inode_reached(irec, ino_offset))  {
			junkit = 1;
			do_warn(
_("entry \"%s\" in dir %" PRIu64" points to an already connected directory inode %" PRIu64 "\n"),
				fname, ip->i_ino, inum);
		} else if (parent == ip->i_ino)  {
			add_inode_reached(irec, ino_offset);
			add_inode_ref(current_irec, current_ino_offset);
		} else if (parent == NULLFSINO) {
			/* ".." was missing, but this entry refers to it,
			   so, set it as the parent and mark for rebuild */
			do_warn(
	_("entry \"%s\" in dir ino %" PRIu64 " doesn't have a .. entry, will set it in ino %" PRIu64 ".\n"),
				fname, ip->i_ino, inum);
			set_inode_parent(irec, ino_offset, ip->i_ino);
			add_inode_reached(irec, ino_offset);
			add_inode_ref(current_irec, current_ino_offset);
			add_dotdot_update(XFS_INO_TO_AGNO(mp, inum), irec,
								ino_offset);
		} else  {
			junkit = 1;
			do_warn(
_("entry \"%s\" in dir inode %" PRIu64 " inconsistent with .. value (%" PRIu64 ") in ino %" PRIu64 "\n"),
				fname, ip->i_ino, parent, inum);
		}
		if (junkit)  {
			if (inum == orphanage_ino)
				orphanage_ino = 0;
			nbad++;
			if (!no_modify)  {
				dir_hash_junkit(hashtab, addr);
				dep->name[0] = '/';
				libxfs_dir2_data_log_entry(&da, bp, dep);
				do_warn(
					_("\twill clear entry \"%s\"\n"),
						fname);
			} else  {
				do_warn(_("\twould clear entry \"%s\"\n"),
					fname);
			}
		}
	}
	*num_illegal += nbad;
	if (needscan)
		libxfs_dir2_data_freescan(mp, d, &i);
	if (needlog)
		libxfs_dir2_data_log_header(&da, bp);
	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(
_("directory block fixing failed (%d)\n"), error);

	/* record the largest free space in the freetab for later checking */
	bf = libxfs_dir2_data_bestfree_p(mp, d);
	freetab->ents[db].v = be16_to_cpu(bf[0].length);
	freetab->ents[db].s = 0;
}

/* check v5 metadata */
static int
__check_dir3_header(
	struct xfs_mount	*mp,
	struct xfs_buf		*bp,
	xfs_ino_t		ino,
	__be64			owner,
	__be64			blkno,
	uuid_t			*uuid)
{

	/* verify owner */
	if (be64_to_cpu(owner) != ino) {
		do_warn(
_("expected owner inode %" PRIu64 ", got %llu, directory block %" PRIu64 "\n"),
			ino, (unsigned long long)be64_to_cpu(owner), xfs_buf_daddr(bp));
		return 1;
	}
	/* verify block number */
	if (be64_to_cpu(blkno) != xfs_buf_daddr(bp)) {
		do_warn(
_("expected block %" PRIu64 ", got %llu, directory inode %" PRIu64 "\n"),
			xfs_buf_daddr(bp), (unsigned long long)be64_to_cpu(blkno), ino);
		return 1;
	}
	/* verify uuid */
	if (platform_uuid_compare(uuid, &mp->m_sb.sb_meta_uuid) != 0) {
		do_warn(
_("wrong FS UUID, directory inode %" PRIu64 " block %" PRIu64 "\n"),
			ino, xfs_buf_daddr(bp));
		return 1;
	}

	return 0;
}

static int
check_da3_header(
	struct xfs_mount	*mp,
	struct xfs_buf		*bp,
	xfs_ino_t		ino)
{
	struct xfs_da3_blkinfo	*info = bp->b_addr;

	return __check_dir3_header(mp, bp, ino, info->owner, info->blkno,
				   &info->uuid);
}

static int
check_dir3_header(
	struct xfs_mount	*mp,
	struct xfs_buf		*bp,
	xfs_ino_t		ino)
{
	struct xfs_dir3_blk_hdr	*info = bp->b_addr;

	return __check_dir3_header(mp, bp, ino, info->owner, info->blkno,
				   &info->uuid);
}

/*
 * Check contents of leaf-form block.
 */
static int
longform_dir2_check_leaf(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip,
	struct dir_hash_tab	*hashtab,
	struct freetab		*freetab)
{
	int			badtail;
	__be16			*bestsp;
	struct xfs_buf		*bp;
	xfs_dablk_t		da_bno;
	int			i;
	xfs_dir2_leaf_t		*leaf;
	xfs_dir2_leaf_tail_t	*ltp;
	int			seeval;
	struct xfs_dir2_leaf_entry *ents;
	struct xfs_dir3_icleaf_hdr leafhdr;
	int			error;
	int			fixit = 0;

	da_bno = mp->m_dir_geo->leafblk;
	error = dir_read_buf(ip, da_bno, &bp, &xfs_dir3_leaf1_buf_ops, &fixit);
	if (error == EFSBADCRC || error == EFSCORRUPTED || fixit) {
		do_warn(
	_("leaf block %u for directory inode %" PRIu64 " bad CRC\n"),
			da_bno, ip->i_ino);
		return 1;
	} else if (error) {
		do_error(
	_("can't read block %u for directory inode %" PRIu64 ", error %d\n"),
			da_bno, ip->i_ino, error);
		/* NOTREACHED */
	}

	leaf = bp->b_addr;
	libxfs_dir2_leaf_hdr_from_disk(mp, &leafhdr, leaf);
	ents = leafhdr.ents;
	ltp = xfs_dir2_leaf_tail_p(mp->m_dir_geo, leaf);
	bestsp = xfs_dir2_leaf_bests_p(ltp);
	if (!(leafhdr.magic == XFS_DIR2_LEAF1_MAGIC ||
	      leafhdr.magic == XFS_DIR3_LEAF1_MAGIC) ||
				leafhdr.forw || leafhdr.back ||
				leafhdr.count < leafhdr.stale ||
				leafhdr.count > mp->m_dir_geo->leaf_max_ents ||
				(char *)&ents[leafhdr.count] > (char *)bestsp) {
		do_warn(
	_("leaf block %u for directory inode %" PRIu64 " bad header\n"),
			da_bno, ip->i_ino);
		libxfs_buf_relse(bp);
		return 1;
	}

	if (leafhdr.magic == XFS_DIR3_LEAF1_MAGIC) {
		error = check_da3_header(mp, bp, ip->i_ino);
		if (error) {
			libxfs_buf_relse(bp);
			return error;
		}
	}

	seeval = dir_hash_see_all(hashtab, ents, leafhdr.count, leafhdr.stale);
	if (dir_hash_check(hashtab, ip, seeval)) {
		libxfs_buf_relse(bp);
		return 1;
	}
	badtail = freetab->nents != be32_to_cpu(ltp->bestcount);
	for (i = 0; !badtail && i < be32_to_cpu(ltp->bestcount); i++) {
		freetab->ents[i].s = 1;
		badtail = freetab->ents[i].v != be16_to_cpu(bestsp[i]);
	}
	if (badtail) {
		do_warn(
	_("leaf block %u for directory inode %" PRIu64 " bad tail\n"),
			da_bno, ip->i_ino);
		libxfs_buf_relse(bp);
		return 1;
	}
	libxfs_buf_relse(bp);
	return fixit;
}

/*
 * Check contents of the node blocks (leaves)
 * Looks for matching hash values for the data entries.
 */
static int
longform_dir2_check_node(
	struct xfs_mount	*mp,
	struct xfs_inode	*ip,
	struct dir_hash_tab	*hashtab,
	struct freetab		*freetab)
{
	struct xfs_buf		*bp;
	xfs_dablk_t		da_bno;
	xfs_dir2_db_t		fdb;
	xfs_dir2_free_t		*free;
	int			i;
	xfs_dir2_leaf_t		*leaf;
	xfs_fileoff_t		next_da_bno;
	int			seeval = 0;
	int			used;
	struct xfs_dir2_leaf_entry *ents;
	struct xfs_dir3_icleaf_hdr leafhdr;
	struct xfs_dir3_icfree_hdr freehdr;
	__be16			*bests;
	int			error;
	int			fixit = 0;

	for (da_bno = mp->m_dir_geo->leafblk, next_da_bno = 0;
			next_da_bno != NULLFILEOFF && da_bno < mp->m_dir_geo->freeblk;
			da_bno = (xfs_dablk_t)next_da_bno) {
		next_da_bno = da_bno + mp->m_dir_geo->fsbcount - 1;
		if (bmap_next_offset(ip, &next_da_bno))
			break;

		if (next_da_bno != NULLFILEOFF &&
		    !libxfs_verify_dablk(mp, next_da_bno)) {
			do_warn(_("invalid dir leaf block 0x%llx\n"),
					(unsigned long long)next_da_bno);
			return 1;
		}

		/*
		 * we need to use the da3 node verifier here as it handles the
		 * fact that reading the leaf hash tree blocks can return either
		 * leaf or node blocks and calls the correct verifier. If we get
		 * a node block, then we'll skip it below based on a magic
		 * number check.
		 */
		error = dir_read_buf(ip, da_bno, &bp, &xfs_da3_node_buf_ops,
				&fixit);
		if (error) {
			do_warn(
	_("can't read leaf block %u for directory inode %" PRIu64 ", error %d\n"),
				da_bno, ip->i_ino, error);
			return 1;
		}
		leaf = bp->b_addr;
		libxfs_dir2_leaf_hdr_from_disk(mp, &leafhdr, leaf);
		ents = leafhdr.ents;
		if (!(leafhdr.magic == XFS_DIR2_LEAFN_MAGIC ||
		      leafhdr.magic == XFS_DIR3_LEAFN_MAGIC ||
		      leafhdr.magic == XFS_DA_NODE_MAGIC ||
		      leafhdr.magic == XFS_DA3_NODE_MAGIC)) {
			do_warn(
	_("unknown magic number %#x for block %u in directory inode %" PRIu64 "\n"),
				leafhdr.magic, da_bno, ip->i_ino);
			libxfs_buf_relse(bp);
			return 1;
		}

		/* check v5 metadata */
		if (leafhdr.magic == XFS_DIR3_LEAFN_MAGIC ||
		    leafhdr.magic == XFS_DA3_NODE_MAGIC) {
			error = check_da3_header(mp, bp, ip->i_ino);
			if (error) {
				libxfs_buf_relse(bp);
				return error;
			}
		}

		/* ignore nodes */
		if (leafhdr.magic == XFS_DA_NODE_MAGIC ||
		    leafhdr.magic == XFS_DA3_NODE_MAGIC) {
			libxfs_buf_relse(bp);
			continue;
		}

		/*
		 * If there's a validator error, we need to ensure that we got
		 * the right ops on the buffer for when we write it back out.
		 */
		bp->b_ops = &xfs_dir3_leafn_buf_ops;
		if (leafhdr.count > mp->m_dir_geo->leaf_max_ents ||
		    leafhdr.count < leafhdr.stale) {
			do_warn(
	_("leaf block %u for directory inode %" PRIu64 " bad header\n"),
				da_bno, ip->i_ino);
			libxfs_buf_relse(bp);
			return 1;
		}
		seeval = dir_hash_see_all(hashtab, ents,
					leafhdr.count, leafhdr.stale);
		libxfs_buf_relse(bp);
		if (seeval != DIR_HASH_CK_OK)
			return 1;
	}
	if (dir_hash_check(hashtab, ip, seeval))
		return 1;

	for (da_bno = mp->m_dir_geo->freeblk, next_da_bno = 0;
	     next_da_bno != NULLFILEOFF;
	     da_bno = (xfs_dablk_t)next_da_bno) {
		next_da_bno = da_bno + mp->m_dir_geo->fsbcount - 1;
		if (bmap_next_offset(ip, &next_da_bno))
			break;

		if (next_da_bno != NULLFILEOFF &&
		    !libxfs_verify_dablk(mp, next_da_bno)) {
			do_warn(_("invalid dir free block 0x%llx\n"),
					(unsigned long long)next_da_bno);
			return 1;
		}

		error = dir_read_buf(ip, da_bno, &bp, &xfs_dir3_free_buf_ops,
				&fixit);
		if (error) {
			do_warn(
	_("can't read freespace block %u for directory inode %" PRIu64 ", error %d\n"),
				da_bno, ip->i_ino, error);
			return 1;
		}
		free = bp->b_addr;
		libxfs_dir2_free_hdr_from_disk(mp, &freehdr, free);
		bests = freehdr.bests;
		fdb = xfs_dir2_da_to_db(mp->m_dir_geo, da_bno);
		if (!(freehdr.magic == XFS_DIR2_FREE_MAGIC ||
		      freehdr.magic == XFS_DIR3_FREE_MAGIC) ||
		    freehdr.firstdb !=
			(fdb - xfs_dir2_byte_to_db(mp->m_dir_geo, XFS_DIR2_FREE_OFFSET)) *
			mp->m_dir_geo->free_max_bests ||
		    freehdr.nvalid < freehdr.nused) {
			do_warn(
	_("free block %u for directory inode %" PRIu64 " bad header\n"),
				da_bno, ip->i_ino);
			libxfs_buf_relse(bp);
			return 1;
		}

		if (freehdr.magic == XFS_DIR3_FREE_MAGIC) {
			error = check_dir3_header(mp, bp, ip->i_ino);
			if (error) {
				libxfs_buf_relse(bp);
				return error;
			}
		}
		for (i = used = 0; i < freehdr.nvalid; i++) {
			if (i + freehdr.firstdb >= freetab->nents ||
					freetab->ents[i + freehdr.firstdb].v !=
						be16_to_cpu(bests[i])) {
				do_warn(
	_("free block %u entry %i for directory ino %" PRIu64 " bad\n"),
					da_bno, i, ip->i_ino);
				libxfs_buf_relse(bp);
				return 1;
			}
			used += be16_to_cpu(bests[i]) != NULLDATAOFF;
			freetab->ents[i + freehdr.firstdb].s = 1;
		}
		if (used != freehdr.nused) {
			do_warn(
	_("free block %u for directory inode %" PRIu64 " bad nused\n"),
				da_bno, ip->i_ino);
			libxfs_buf_relse(bp);
			return 1;
		}
		libxfs_buf_relse(bp);
	}
	for (i = 0; i < freetab->nents; i++) {
		if ((freetab->ents[i].s == 0) &&
		    (freetab->ents[i].v != NULLDATAOFF)) {
			do_warn(
	_("missing freetab entry %u for directory inode %" PRIu64 "\n"),
				i, ip->i_ino);
			return 1;
		}
	}
	return fixit;
}

/*
 * If a directory is corrupt, we need to read in as many entries as possible,
 * destroy the entry and create a new one with recovered name/inode pairs.
 * (ie. get libxfs to do all the grunt work)
 */
static void
longform_dir2_entry_check(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_inode	*ip,
	int			*num_illegal,
	int			*need_dot,
	struct ino_tree_node	*irec,
	int			ino_offset,
	struct dir_hash_tab	*hashtab)
{
	struct xfs_buf		*bp = NULL;
	xfs_dablk_t		da_bno;
	freetab_t		*freetab;
	int			i;
	enum xfs_dir2_fmt	fmt;
	xfs_fileoff_t		next_da_bno;
	int			seeval;
	int			fixit = 0;
	struct xfs_da_args	args;
	int			error;

	*need_dot = 1;
	freetab = malloc(FREETAB_SIZE(ip->i_disk_size / mp->m_dir_geo->blksize));
	if (!freetab) {
		do_error(_("malloc failed in %s (%" PRId64 " bytes)\n"),
			__func__,
			FREETAB_SIZE(ip->i_disk_size / mp->m_dir_geo->blksize));
		exit(1);
	}
	freetab->naents = ip->i_disk_size / mp->m_dir_geo->blksize;
	freetab->nents = 0;
	for (i = 0; i < freetab->naents; i++) {
		freetab->ents[i].v = NULLDATAOFF;
		freetab->ents[i].s = 0;
	}

	/* is this a block, leaf, or node directory? */
	args.dp = ip;
	args.geo = mp->m_dir_geo;
	args.owner = ip->i_ino;
	fmt = libxfs_dir2_format(&args, &error);

	/* check directory "data" blocks (ie. name/inode pairs) */
	for (da_bno = 0, next_da_bno = 0;
	     next_da_bno != NULLFILEOFF && da_bno < mp->m_dir_geo->leafblk;
	     da_bno = (xfs_dablk_t)next_da_bno) {
		const struct xfs_buf_ops *ops;
		int			 error;

		next_da_bno = da_bno + mp->m_dir_geo->fsbcount - 1;
		if (bmap_next_offset(ip, &next_da_bno)) {
			/*
			 * if this is the first block, there isn't anything we
			 * can recover so we just trash it.
			 */
			 if (da_bno == 0) {
				fixit++;
				goto out_fix;
			}
			break;
		}

		if (next_da_bno != NULLFILEOFF &&
		    !libxfs_verify_dablk(mp, next_da_bno)) {
			do_warn(_("invalid dir data block 0x%llx\n"),
					(unsigned long long)next_da_bno);
			fixit++;
			goto out_fix;
		}

		if (fmt == XFS_DIR2_FMT_BLOCK)
			ops = &xfs_dir3_block_buf_ops;
		else
			ops = &xfs_dir3_data_buf_ops;

		error = dir_read_buf(ip, da_bno, &bp, ops, &fixit);
		if (error) {
			do_warn(
	_("can't read data block %u for directory inode %" PRIu64 " error %d\n"),
				da_bno, ino, error);
			*num_illegal += 1;

			/*
			 * we try to read all "data" blocks, but if we are in
			 * block form and we fail, there isn't anything else to
			 * read, and nothing we can do but trash it.
			 */
			if (fmt == XFS_DIR2_FMT_BLOCK) {
				fixit++;
				goto out_fix;
			}
			continue;
		}

		/* salvage any dirents that look ok */
		longform_dir2_entry_check_data(mp, ip, num_illegal, need_dot,
				irec, ino_offset, bp, hashtab,
				&freetab, da_bno, fmt == XFS_DIR2_FMT_BLOCK);

		/* check v5 metadata */
		if (xfs_has_crc(mp)) {
			error = check_dir3_header(mp, bp, ino);
			if (error) {
				fixit++;
				if (fmt == XFS_DIR2_FMT_BLOCK)
					goto out_fix;

				libxfs_buf_relse(bp);
				bp = NULL;
				continue;
			}
		}

		if (fmt == XFS_DIR2_FMT_BLOCK)
			break;

		libxfs_buf_relse(bp);
		bp = NULL;
	}
	fixit |= (*num_illegal != 0) || dir2_is_badino(ino) || *need_dot;

	if (!dotdot_update) {
		/* check btree and freespace */
		if (fmt == XFS_DIR2_FMT_BLOCK) {
			struct xfs_dir2_data_hdr *block;
			xfs_dir2_block_tail_t	*btp;
			xfs_dir2_leaf_entry_t	*blp;

			block = bp->b_addr;
			btp = xfs_dir2_block_tail_p(mp->m_dir_geo, block);
			blp = xfs_dir2_block_leaf_p(btp);
			seeval = dir_hash_see_all(hashtab, blp,
						be32_to_cpu(btp->count),
						be32_to_cpu(btp->stale));
			if (dir_hash_check(hashtab, ip, seeval))
				fixit |= 1;
		} else if (fmt == XFS_DIR2_FMT_LEAF) {
			fixit |= longform_dir2_check_leaf(mp, ip, hashtab,
								freetab);
		} else {
			fixit |= longform_dir2_check_node(mp, ip, hashtab,
								freetab);
		}
	}
out_fix:
	if (bp)
		libxfs_buf_relse(bp);

	if (!no_modify && (fixit || dotdot_update)) {
		longform_dir2_rebuild(mp, ino, ip, irec, ino_offset, hashtab);
		*num_illegal = 0;
		*need_dot = 0;
	} else {
		if (fixit || dotdot_update)
			do_warn(
	_("would rebuild directory inode %" PRIu64 "\n"), ino);
	}

	free(freetab);
}

/*
 * shortform directory v2 processing routines -- entry verification and
 * bad entry deletion (pruning).
 */
static struct xfs_dir2_sf_entry *
shortform_dir2_junk(
	struct xfs_mount	*mp,
	struct xfs_dir2_sf_hdr	*sfp,
	struct xfs_dir2_sf_entry *sfep,
	xfs_ino_t		lino,
	int			*max_size,
	int			*index,
	int			*bytes_deleted,
	int			*ino_dirty)
{
	struct xfs_dir2_sf_entry *next_sfep;
	int			next_len;
	int			next_elen;

	if (lino == orphanage_ino)
		orphanage_ino = 0;

	next_elen = libxfs_dir2_sf_entsize(mp, sfp, sfep->namelen);
	next_sfep = libxfs_dir2_sf_nextentry(mp, sfp, sfep);

	/*
	 * if we are just checking, simply return the pointer to the next entry
	 * here so that the checking loop can continue.
	 */
	if (no_modify) {
		do_warn(_("would junk entry\n"));
		return next_sfep;
	}

	/*
	 * now move all the remaining entries down over the junked entry and
	 * clear the newly unused bytes at the tail of the directory region.
	 */
	next_len = *max_size - ((intptr_t)next_sfep - (intptr_t)sfp);
	*max_size -= next_elen;
	*bytes_deleted += next_elen;

	memmove(sfep, next_sfep, next_len);
	memset((void *)((intptr_t)sfep + next_len), 0, next_elen);
	sfp->count -= 1;
	*ino_dirty = 1;

	/*
	 * WARNING:  drop the index i by one so it matches the decremented count
	 * for accurate comparisons in the loop test
	 */
	(*index)--;

	do_warn(_("junking entry\n"));
	return sfep;
}

static void
shortform_dir2_entry_check(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_inode	*ip,
	int			*ino_dirty,
	struct ino_tree_node	*current_irec,
	int			current_ino_offset,
	struct dir_hash_tab	*hashtab)
{
	xfs_ino_t		lino;
	xfs_ino_t		parent;
	xfs_ino_t		dup_inum;
	struct xfs_dir2_sf_hdr	*sfp = ip->i_df.if_data;
	struct xfs_dir2_sf_entry *sfep;
	struct xfs_dir2_sf_entry *next_sfep;
	struct xfs_ifork	*ifp;
	struct ino_tree_node	*irec;
	xfs_dir2_dataptr_t	diroffset;
	int			max_size;
	int			ino_offset;
	int			i;
	int			bad_sfnamelen;
	int			namelen;
	int			bytes_deleted;
	char			fname[MAXNAMELEN + 1];
	int			i8;

	ifp = &ip->i_df;
	*ino_dirty = 0;
	bytes_deleted = 0;

	max_size = ifp->if_bytes;
	ASSERT(ip->i_disk_size <= ifp->if_bytes);

	/*
	 * if just rebuild a directory due to a "..", update and return
	 */
	if (dotdot_update) {
		parent = get_inode_parent(current_irec, current_ino_offset);
		if (no_modify) {
			do_warn(
	_("would set .. in sf dir inode %" PRIu64 " to %" PRIu64 "\n"),
				ino, parent);
		} else {
			do_warn(
	_("setting .. in sf dir inode %" PRIu64 " to %" PRIu64 "\n"),
				ino, parent);
			libxfs_dir2_sf_put_parent_ino(sfp, parent);
			*ino_dirty = 1;
		}
		return;
	}

	/*
	 * no '.' entry in shortform dirs, just bump up ref count by 1
	 * '..' was already (or will be) accounted for and checked when
	 * the directory is reached or will be taken care of when the
	 * directory is moved to orphanage.
	 */
	add_inode_ref(current_irec, current_ino_offset);

	/*
	 * Initialise i8 counter -- the parent inode number counts as well.
	 */
	i8 = libxfs_dir2_sf_get_parent_ino(sfp) > XFS_DIR2_MAX_SHORT_INUM;

	/*
	 * now run through entries, stop at first bad entry, don't need
	 * to skip over '..' since that's encoded in its own field and
	 * no need to worry about '.' since it doesn't exist.
	 */
	sfep = next_sfep = xfs_dir2_sf_firstentry(sfp);

	for (i = 0; i < sfp->count && max_size >
					(intptr_t)next_sfep - (intptr_t)sfp;
			sfep = next_sfep, i++)  {
		bad_sfnamelen = 0;

		lino = libxfs_dir2_sf_get_ino(mp, sfp, sfep);

		namelen = sfep->namelen;

		ASSERT(no_modify || namelen > 0);

		if (no_modify && namelen == 0)  {
			/*
			 * if we're really lucky, this is
			 * the last entry in which case we
			 * can use the dir size to set the
			 * namelen value.  otherwise, forget
			 * it because we're not going to be
			 * able to find the next entry.
			 */
			bad_sfnamelen = 1;

			if (i == sfp->count - 1)  {
				namelen = ip->i_disk_size -
					((intptr_t) &sfep->name[0] -
					 (intptr_t) sfp);
			} else  {
				/*
				 * don't process the rest of the directory,
				 * break out of processing loop
				 */
				break;
			}
		} else if (no_modify && (intptr_t) sfep - (intptr_t) sfp +
				+ libxfs_dir2_sf_entsize(mp, sfp, sfep->namelen)
				> ip->i_disk_size)  {
			bad_sfnamelen = 1;

			if (i == sfp->count - 1)  {
				namelen = ip->i_disk_size -
					((intptr_t) &sfep->name[0] -
					 (intptr_t) sfp);
			} else  {
				/*
				 * don't process the rest of the directory,
				 * break out of processing loop
				 */
				break;
			}
		}

		memmove(fname, sfep->name, sfep->namelen);
		fname[sfep->namelen] = '\0';

		ASSERT(no_modify || (lino != NULLFSINO && lino != 0));
		ASSERT(no_modify || libxfs_verify_dir_ino(mp, lino));

		/*
		 * Also skip entries with bogus inode numbers if we're
		 * in no modify mode.
		 */

		if (no_modify && !libxfs_verify_dir_ino(mp, lino))  {
			next_sfep = libxfs_dir2_sf_nextentry(mp, sfp, sfep);
			continue;
		}

		irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, lino),
					XFS_INO_TO_AGINO(mp, lino));

		if (irec == NULL)  {
			do_warn(
	_("entry \"%s\" in shortform directory %" PRIu64 " references non-existent inode %" PRIu64 ", "),
				fname, ino, lino);
			next_sfep = shortform_dir2_junk(mp, sfp, sfep, lino,
						&max_size, &i, &bytes_deleted,
						ino_dirty);
			continue;
		}

		ino_offset = XFS_INO_TO_AGINO(mp, lino) - irec->ino_startnum;

		/*
		 * if it's a free inode, blow out the entry.
		 * by now, any inode that we think is free
		 * really is free.
		 */
		if (is_inode_free(irec, ino_offset))  {
			do_warn(
	_("entry \"%s\" in shortform directory inode %" PRIu64 " points to free inode %" PRIu64 ", "),
				fname, ino, lino);
			next_sfep = shortform_dir2_junk(mp, sfp, sfep, lino,
						&max_size, &i, &bytes_deleted,
						ino_dirty);
			continue;
		}

		/*
		 * Regular directories cannot point to metadata files.  If
		 * we find such a thing, blow out the entry.
		 */
		if (!xfs_is_metadir_inode(ip) &&
		    inode_is_meta(irec, ino_offset)) {
			do_warn(
	_("entry \"%s\" in regular dir %" PRIu64" points to a metadata inode %" PRIu64 ", "),
					fname, ip->i_ino, lino);
			next_sfep = shortform_dir2_junk(mp, sfp, sfep, lino,
						&max_size, &i, &bytes_deleted,
						ino_dirty);
			continue;
		}

		/*
		 * Metadata directories cannot point to regular files.  If
		 * we find such a thing, blow out the entry.
		 */
		if (xfs_is_metadir_inode(ip) &&
		    !inode_is_meta(irec, ino_offset)) {
			do_warn(
	_("entry \"%s\" in metadata dir %" PRIu64" points to a regular inode %" PRIu64 ", "),
					fname, ip->i_ino, lino);
			next_sfep = shortform_dir2_junk(mp, sfp, sfep, lino,
						&max_size, &i, &bytes_deleted,
						ino_dirty);
			continue;
		}

		/*
		 * check if this inode is lost+found dir in the root
		 */
		if (ino == mp->m_sb.sb_rootino && strcmp(fname, ORPHANAGE) == 0) {
			/*
			 * if it's not a directory, trash it
			 */
			if (!inode_isadir(irec, ino_offset)) {
				do_warn(
	_("%s (ino %" PRIu64 ") in root (%" PRIu64 ") is not a directory, "),
					ORPHANAGE, lino, ino);
				next_sfep = shortform_dir2_junk(mp, sfp, sfep,
						lino, &max_size, &i,
						&bytes_deleted, ino_dirty);
				continue;
			}
			/*
			 * if this is a dup, it will be picked up below,
			 * otherwise, mark it as the orphanage for later.
			 */
			if (!orphanage_ino)
				orphanage_ino = lino;
		}
		/*
		 * check for duplicate names in directory.
		 */
		diroffset = xfs_dir2_byte_to_dataptr(
				xfs_dir2_sf_get_offset(sfep));
		dup_inum = dir_hash_add(mp, hashtab, diroffset,
				lino, sfep->namelen, sfep->name,
				libxfs_dir2_sf_get_ftype(mp, sfep));
		if (dup_inum != NULLFSINO) {
			do_warn(
_("entry \"%s\" (ino %" PRIu64 ") in dir %" PRIu64 " already points to ino %" PRIu64 ", "),
				fname, lino, ino, dup_inum);
			next_sfep = shortform_dir2_junk(mp, sfp, sfep, lino,
						&max_size, &i, &bytes_deleted,
						ino_dirty);
			continue;
		}

		if (!inode_isadir(irec, ino_offset))  {
			/*
			 * check easy case first, regular inode, just bump
			 * the link count
			 */
			add_inode_reached(irec, ino_offset);
		} else  {
			parent = get_inode_parent(irec, ino_offset);

			/*
			 * bump up the link counts in parent and child.
			 * directory but if the link doesn't agree with
			 * the .. in the child, blow out the entry
			 */
			if (is_inode_reached(irec, ino_offset))  {
				do_warn(
	_("entry \"%s\" in directory inode %" PRIu64
	  " references already connected inode %" PRIu64 ", "),
					fname, ino, lino);
				next_sfep = shortform_dir2_junk(mp, sfp, sfep,
						lino, &max_size, &i,
						&bytes_deleted, ino_dirty);
				dir_hash_junkit(hashtab, diroffset);
				continue;
			} else if (parent == ino)  {
				add_inode_reached(irec, ino_offset);
				add_inode_ref(current_irec, current_ino_offset);
			} else if (parent == NULLFSINO) {
				/* ".." was missing, but this entry refers to it,
				so, set it as the parent and mark for rebuild */
				do_warn(
	_("entry \"%s\" in dir ino %" PRIu64 " doesn't have a .. entry, will set it in ino %" PRIu64 ".\n"),
					fname, ino, lino);
				set_inode_parent(irec, ino_offset, ino);
				add_inode_reached(irec, ino_offset);
				add_inode_ref(current_irec, current_ino_offset);
				add_dotdot_update(XFS_INO_TO_AGNO(mp, lino),
							irec, ino_offset);
			} else  {
				do_warn(
	_("entry \"%s\" in directory inode %" PRIu64
	  " not consistent with .. value (%" PRIu64
	  ") in inode %" PRIu64 ", "),
					fname, ino, parent, lino);
				next_sfep = shortform_dir2_junk(mp, sfp, sfep,
						lino, &max_size, &i,
						&bytes_deleted, ino_dirty);
				dir_hash_junkit(hashtab, diroffset);
				continue;
			}
		}

		/* validate ftype field if supported */
		if (xfs_has_ftype(mp)) {
			uint8_t dir_ftype;
			uint8_t ino_ftype;

			dir_ftype = libxfs_dir2_sf_get_ftype(mp, sfep);
			ino_ftype = get_inode_ftype(irec, ino_offset);

			if (dir_ftype != ino_ftype) {
				if (no_modify) {
					do_warn(
	_("would fix ftype mismatch (%d/%d) in directory/child inode %" PRIu64 "/%" PRIu64 "\n"),
						dir_ftype, ino_ftype,
						ino, lino);
				} else {
					do_warn(
	_("fixing ftype mismatch (%d/%d) in directory/child inode %" PRIu64 "/%" PRIu64 "\n"),
						dir_ftype, ino_ftype,
						ino, lino);
					libxfs_dir2_sf_put_ftype(mp, sfep,
								ino_ftype);
					dir_hash_update_ftype(hashtab,
			(xfs_dir2_dataptr_t)(sfep - xfs_dir2_sf_firstentry(sfp)),
							      ino_ftype);
					*ino_dirty = 1;
				}
			}
		}

		if (lino > XFS_DIR2_MAX_SHORT_INUM)
			i8++;

		/*
		 * go onto next entry - we have to take entries with bad namelen
		 * into account in no modify mode since we calculate size based
		 * on next_sfep.
		 */
		ASSERT(no_modify || bad_sfnamelen == 0);
		next_sfep = (struct xfs_dir2_sf_entry *)((intptr_t)sfep +
			      (bad_sfnamelen
				? libxfs_dir2_sf_entsize(mp, sfp, namelen)
				: libxfs_dir2_sf_entsize(mp, sfp, sfep->namelen)));
	}

	if (sfp->i8count != i8) {
		if (no_modify) {
			do_warn(_("would fix i8count in inode %" PRIu64 "\n"),
				ino);
		} else {
			if (i8 == 0) {
				struct xfs_dir2_sf_entry *tmp_sfep;

				tmp_sfep = next_sfep;
				process_sf_dir2_fixi8(mp, sfp, &tmp_sfep);
				bytes_deleted +=
					(intptr_t)next_sfep -
					(intptr_t)tmp_sfep;
				next_sfep = tmp_sfep;
			} else
				sfp->i8count = i8;
			*ino_dirty = 1;
			do_warn(_("fixing i8count in inode %" PRIu64 "\n"),
				ino);
		}
	}

	/*
	 * sync up sizes if required
	 */
	if (*ino_dirty && bytes_deleted > 0)  {
		ASSERT(!no_modify);
		libxfs_idata_realloc(ip, -bytes_deleted, XFS_DATA_FORK);
		ip->i_disk_size -= bytes_deleted;
	}

	if (ip->i_disk_size != ip->i_df.if_bytes)  {
		ASSERT(ip->i_df.if_bytes == (xfs_fsize_t)
				((intptr_t) next_sfep - (intptr_t) sfp));
		ip->i_disk_size = (xfs_fsize_t)
				((intptr_t) next_sfep - (intptr_t) sfp);
		do_warn(
	_("setting size to %" PRId64 " bytes to reflect junked entries\n"),
			ip->i_disk_size);
		*ino_dirty = 1;
	}
}

static void
dir_hash_add_parent_ptrs(
	struct xfs_inode	*dp,
	struct dir_hash_tab	*hashtab)
{
	struct dir_hash_ent	*p;

	if (!xfs_has_parent(dp->i_mount))
		return;

	for (p = hashtab->first; p; p = p->nextbyorder) {
		if (p->junkit)
			continue;
		if (p->name.name[0] == '/')
			continue;
		if (p->name.name[0] == '.' &&
		    (p->name.len == 1 ||
		     (p->name.len == 2 && p->name.name[1] == '.')))
			continue;

		add_parent_ptr(p->inum, p->name.name, dp, dotdot_update);
	}
}

/*
 * If we have to create a .. for /, do it now *before* we delete the bogus
 * entries, otherwise the directory could transform into a shortform dir which
 * would probably cause the simulation to choke.  Even if the illegal entries
 * get shifted around, it's ok because the entries are structurally intact and
 * in in hash-value order so the simulation won't get confused if it has to
 * move them around.
 */
static void
fix_dotdot(
	struct xfs_mount	*mp,
	xfs_ino_t		ino,
	struct xfs_inode	*ip,
	xfs_ino_t		rootino,
	const char		*tag,
	int			*need_dotdot)
{
	struct xfs_trans	*tp;
	int			nres;
	int			error;

	if (ino != rootino || !*need_dotdot)
		return;

	if (no_modify) {
		do_warn(_("would recreate %s directory .. entry\n"), tag);
		return;
	}

	ASSERT(ip->i_df.if_format != XFS_DINODE_FMT_LOCAL);

	do_warn(_("recreating %s directory .. entry\n"), tag);

	nres = libxfs_mkdir_space_res(mp, 2);
	error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_mkdir, nres, 0, 0, &tp);
	if (error)
		res_failed(error);

	libxfs_trans_ijoin(tp, ip, 0);

	error = -libxfs_dir_createname(tp, ip, &xfs_name_dotdot, ip->i_ino,
			nres);
	if (error)
		do_error(
_("can't make \"..\" entry in %s inode %" PRIu64 ", createname error %d\n"),
			tag ,ino, error);

	libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	error = -libxfs_trans_commit(tp);
	if (error)
		do_error(
_("%s inode \"..\" entry recreation failed (%d)\n"), tag, error);

	*need_dotdot = 0;
}

/*
 * processes all reachable inodes in directories
 */
static void
process_dir_inode(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	struct ino_tree_node	*irec,
	int			ino_offset)
{
	xfs_ino_t		ino;
	struct xfs_inode	*ip;
	struct xfs_trans	*tp;
	struct dir_hash_tab	*hashtab;
	int			need_dot;
	int			dirty, num_illegal, error, nres;

	ino = XFS_AGINO_TO_INO(mp, agno, irec->ino_startnum + ino_offset);

	/*
	 * open up directory inode, check all entries,
	 * then call prune_dir_entries to remove all
	 * remaining illegal directory entries.
	 */

	ASSERT(!is_inode_refchecked(irec, ino_offset) || dotdot_update);

	error = -libxfs_iget(mp, NULL, ino, 0, &ip);
	if (error) {
		if (!no_modify)
			do_error(
	_("couldn't map inode %" PRIu64 ", err = %d\n"),
				ino, error);
		else  {
			do_warn(
	_("couldn't map inode %" PRIu64 ", err = %d\n"),
				ino, error);
			/*
			 * see below for what we're doing if this
			 * is root.  Why do we need to do this here?
			 * to ensure that the root doesn't show up
			 * as being disconnected in the no_modify case.
			 */
			if (mp->m_sb.sb_rootino == ino)  {
				add_inode_reached(irec, 0);
				add_inode_ref(irec, 0);
			}
		}

		add_inode_refchecked(irec, 0);
		return;
	}

	need_dot = dirty = num_illegal = 0;

	if (mp->m_sb.sb_rootino == ino || mp->m_sb.sb_metadirino == ino) {
		/*
		 * mark root inode reached and bump up
		 * link count for root inode to account
		 * for '..' entry since the root inode is
		 * never reached by a parent.  we know
		 * that root's '..' is always good --
		 * guaranteed by phase 3 and/or below.
		 */
		add_inode_reached(irec, ino_offset);
	}

	add_inode_refchecked(irec, ino_offset);

	hashtab = dir_hash_init(ip->i_disk_size);

	/*
	 * look for bogus entries
	 */
	switch (ip->i_df.if_format)  {
		case XFS_DINODE_FMT_EXTENTS:
		case XFS_DINODE_FMT_BTREE:
			/*
			 * also check for missing '.' in longform dirs.
			 * missing .. entries are added if required when
			 * the directory is connected to lost+found. but
			 * we need to create '.' entries here.
			 */
			longform_dir2_entry_check(mp, ino, ip,
						&num_illegal, &need_dot,
						irec, ino_offset,
						hashtab);
			break;

		case XFS_DINODE_FMT_LOCAL:
			/*
			 * using the remove reservation is overkill
			 * since at most we'll only need to log the
			 * inode but it's easier than wedging a
			 * new define in ourselves.
			 */
			nres = no_modify ? 0 : libxfs_remove_space_res(mp, 0);
			error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_remove,
						    nres, 0, 0, &tp);
			if (error)
				res_failed(error);

			libxfs_trans_ijoin(tp, ip, 0);

			shortform_dir2_entry_check(mp, ino, ip, &dirty,
						irec, ino_offset,
						hashtab);

			ASSERT(dirty == 0 || (dirty && !no_modify));
			if (dirty)  {
				libxfs_trans_log_inode(tp, ip,
					XFS_ILOG_CORE | XFS_ILOG_DDATA);
				error = -libxfs_trans_commit(tp);
				if (error)
					do_error(
_("error %d fixing shortform directory %llu\n"),
						error,
						(unsigned long long)ip->i_ino);
			} else  {
				libxfs_trans_cancel(tp);
			}
			break;

		default:
			break;
	}
	dir_hash_add_parent_ptrs(ip, hashtab);
	dir_hash_done(hashtab);

	fix_dotdot(mp, ino, ip, mp->m_sb.sb_rootino, "root", &need_root_dotdot);
	if (xfs_has_metadir(mp))
		fix_dotdot(mp, ino, ip, mp->m_sb.sb_metadirino, "metadata",
				&need_metadir_dotdot);

	/*
	 * if we need to create the '.' entry, do so only if
	 * the directory is a longform dir.  if it's been
	 * turned into a shortform dir, then the inode is ok
	 * since shortform dirs have no '.' entry and the inode
	 * has already been committed by prune_lf_dir_entry().
	 */
	if (need_dot)  {
		/*
		 * bump up our link count but don't
		 * bump up the inode link count.  chances
		 * are good that even though we lost '.'
		 * the inode link counts reflect '.' so
		 * leave the inode link count alone and if
		 * it turns out to be wrong, we'll catch
		 * that in phase 7.
		 */
		add_inode_ref(irec, ino_offset);

		if (no_modify)  {
			do_warn(
	_("would create missing \".\" entry in dir ino %" PRIu64 "\n"),
				ino);
		} else if (ip->i_df.if_format != XFS_DINODE_FMT_LOCAL)  {
			/*
			 * need to create . entry in longform dir.
			 */
			do_warn(
	_("creating missing \".\" entry in dir ino %" PRIu64 "\n"), ino);

			nres = libxfs_mkdir_space_res(mp, 1);
			error = -libxfs_trans_alloc(mp, &M_RES(mp)->tr_mkdir,
						    nres, 0, 0, &tp);
			if (error)
				res_failed(error);

			libxfs_trans_ijoin(tp, ip, 0);

			error = -libxfs_dir_createname(tp, ip, &xfs_name_dot,
					ip->i_ino, nres);
			if (error)
				do_error(
	_("can't make \".\" entry in dir ino %" PRIu64 ", createname error %d\n"),
					ino, error);

			libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
			error = -libxfs_trans_commit(tp);
			if (error)
				do_error(
	_("root inode \".\" entry recreation failed (%d)\n"), error);
		}
	}
	libxfs_irele(ip);
}

static void
mark_inode(
	struct xfs_mount	*mp,
	xfs_ino_t		ino)
{
	struct ino_tree_node	*irec =
		find_inode_rec(mp, XFS_INO_TO_AGNO(mp, ino),
				   XFS_INO_TO_AGINO(mp, ino));

	add_inode_reached(irec, XFS_INO_TO_AGINO(mp, ino) - irec->ino_startnum);
}

/*
 * mark realtime bitmap and summary inodes as reached.
 * quota inode will be marked here as well
 */
static void
mark_standalone_inodes(xfs_mount_t *mp)
{
	if (!xfs_has_rtgroups(mp)) {
		mark_inode(mp, mp->m_sb.sb_rbmino);
		mark_inode(mp, mp->m_sb.sb_rsumino);
	}

	if (!fs_quotas || xfs_has_metadir(mp))
		return;

	if (has_quota_inode(XFS_DQTYPE_USER))
		mark_inode(mp, get_quota_inode(XFS_DQTYPE_USER));
	if (has_quota_inode(XFS_DQTYPE_GROUP))
		mark_inode(mp, get_quota_inode(XFS_DQTYPE_GROUP));
	if (has_quota_inode(XFS_DQTYPE_PROJ))
		mark_inode(mp, get_quota_inode(XFS_DQTYPE_PROJ));
}

static void
check_for_orphaned_inodes(
	xfs_mount_t		*mp,
	xfs_agnumber_t		agno,
	ino_tree_node_t		*irec)
{
	int			i;
	xfs_ino_t		ino;

	for (i = 0; i < XFS_INODES_PER_CHUNK; i++)  {
		ASSERT(is_inode_confirmed(irec, i));
		if (is_inode_free(irec, i))
			continue;

		if (is_inode_reached(irec, i))
			continue;

		ASSERT(inode_isadir(irec, i) ||
			num_inode_references(irec, i) == 0);

		ino = XFS_AGINO_TO_INO(mp, agno, i + irec->ino_startnum);
		if (inode_isadir(irec, i))
			do_warn(_("disconnected dir inode %" PRIu64 ", "), ino);
		else
			do_warn(_("disconnected inode %" PRIu64 ", "), ino);
		if (!no_modify)  {
			if (!orphanage_ino)
				orphanage_ino = mk_orphanage(mp);
			do_warn(_("moving to %s\n"), ORPHANAGE);
			mv_orphanage(mp, ino, inode_isadir(irec, i));
		} else  {
			do_warn(_("would move to %s\n"), ORPHANAGE);
		}
		/*
		 * for read-only case, even though the inode isn't
		 * really reachable, set the flag (and bump our link
		 * count) anyway to fool phase 7
		 */
		add_inode_reached(irec, i);
	}
}

static void
do_dir_inode(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct ino_tree_node	*irec = arg;
	int			i;

	for (i = 0; i < XFS_INODES_PER_CHUNK; i++)  {
		if (inode_isadir(irec, i))
			process_dir_inode(wq->wq_ctx, agno, irec, i);
	}
}

static void
traverse_function(
	struct workqueue	*wq,
	xfs_agnumber_t		agno,
	void			*arg)
{
	struct ino_tree_node	*irec;
	prefetch_args_t		*pf_args = arg;
	struct workqueue	lwq;
	struct xfs_mount	*mp = wq->wq_ctx;

	wait_for_inode_prefetch(pf_args);

	if (verbose)
		do_log(_("        - agno = %d\n"), agno);

	/*
	 * The more AGs we have in flight at once, the fewer processing threads
	 * per AG. This means we don't overwhelm the machine with hundreds of
	 * threads when we start acting on lots of AGs at once. We just want
	 * enough that we can keep multiple CPUs busy across multiple AGs.
	 */
	workqueue_create_bound(&lwq, mp, ag_stride, 1000);

	for (irec = findfirst_inode_rec(agno); irec; irec = next_ino_rec(irec)) {
		if (irec->ino_isa_dir == 0)
			continue;

		if (pf_args) {
			sem_post(&pf_args->ra_count);
#ifdef XR_PF_TRACE
			{
			int	i;
			sem_getvalue(&pf_args->ra_count, &i);
			pftrace(
		"processing inode chunk %p in AG %d (sem count = %d)",
				irec, agno, i);
			}
#endif
		}

		queue_work(&lwq, do_dir_inode, agno, irec);
	}
	destroy_work_queue(&lwq);
	cleanup_inode_prefetch(pf_args);
}

static void
update_missing_dotdot_entries(
	xfs_mount_t		*mp)
{
	dotdot_update_t		*dir;

	/*
	 * these entries parents were updated, rebuild them again
	 * set dotdot_update flag so processing routines do not count links
	 */
	dotdot_update = 1;
	while (!list_empty(&dotdot_update_list)) {
		dir = list_entry(dotdot_update_list.prev, struct dotdot_update,
				 list);
		list_del(&dir->list);
		process_dir_inode(mp, dir->agno, dir->irec, dir->ino_offset);
		free(dir);
	}
}

static void
traverse_ags(
	struct xfs_mount	*mp)
{
	do_inode_prefetch(mp, ag_stride, traverse_function, false, true);
}

static void
reset_rt_sb_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg;

	if (no_modify) {
		if (need_rbmino)
			do_warn(_("would reinitialize realtime bitmap inode\n"));
		if (need_rsumino)
			do_warn(_("would reinitialize realtime summary inode\n"));
		return;
	}

	rtg = libxfs_rtgroup_grab(mp, 0);

	if (need_rbmino)  {
		do_warn(_("reinitializing realtime bitmap inode\n"));
		mk_rtino(rtg, XFS_RTGI_BITMAP);
		need_rbmino = 0;
	}

	if (need_rsumino)  {
		do_warn(_("reinitializing realtime summary inode\n"));
		mk_rtino(rtg, XFS_RTGI_SUMMARY);
		need_rsumino = 0;
	}

	do_log(
_("        - resetting contents of realtime bitmap and summary inodes\n"));

	fill_rtbitmap(rtg);
	fill_rtsummary(rtg);

	libxfs_rtgroup_rele(rtg);
}

static void
reset_rt_metadir_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_rtgroup	*rtg = NULL;
	xfs_filblks_t		metadata_blocks = 0;
	xfs_filblks_t		est_fdblocks = 0;
	int			error;

	/*
	 * Release the rtgroup inodes so that we can rebuild everything from
	 * observations.
	 */
	if (!no_modify)
		unload_rtgroup_inodes(mp);

	if (mp->m_sb.sb_rgcount > 0) {
		if (no_modify) {
			if (!mp->m_rtdirip)
				do_warn(_("would recreate realtime metadir\n"));
		} else {
			error = -libxfs_rtginode_mkdir_parent(mp);
			if (error)
				do_error(_("failed to create realtime metadir (%d)\n"),
					error);
		}

		if (mp->m_rtdirip) {
			mark_ino_inuse(mp, mp->m_rtdirip->i_ino, S_IFDIR,
					mp->m_metadirip->i_ino);
			mark_ino_metadata(mp, mp->m_rtdirip->i_ino);
		}
	}

	/*
	 * Estimate how much free space will be left after building btrees
	 * unless we already decided that we needed to pack all new blocks
	 * maximally.
	 */
	if (!need_packed_btrees) {
		while ((rtg = xfs_rtgroup_next(mp, rtg))) {
			metadata_blocks += estimate_rtrmapbt_blocks(rtg);
			metadata_blocks += estimate_rtrefcountbt_blocks(rtg);
		}

		if (mp->m_sb.sb_fdblocks > metadata_blocks)
			est_fdblocks = mp->m_sb.sb_fdblocks - metadata_blocks;
	}

	/*
	 * This isn't the whole story, but it keeps the message that we've had
	 * for years and which is expected in xfstests and more.
	 */
	if (!no_modify)
		do_log(
_("        - resetting contents of realtime bitmap and summary inodes\n"));

	if (mp->m_sb.sb_rgcount == 0)
		return;

	while ((rtg = xfs_rtgroup_next(mp, rtg))) {
		if (!xfs_has_zoned(mp)) {
			ensure_rtgroup_bitmap(rtg);
			ensure_rtgroup_summary(rtg);
		}
		ensure_rtgroup_rmapbt(rtg, est_fdblocks);
		ensure_rtgroup_refcountbt(rtg, est_fdblocks);
	}
}

static bool
ensure_quota_file(
	struct xfs_inode	*dp,
	xfs_dqtype_t		type)
{
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_inode	*ip;
	const char		*name = libxfs_dqinode_path(type);
	int			error;

	if (!has_quota_inode(type))
		return false;

	if (no_modify) {
		if (lost_quota_inode(type))
			do_warn(_("would reset %s quota inode\n"), name);
		return false;
	}

	if (!lost_quota_inode(type)) {
		/*
		 * The /quotas directory has been discarded, but we should
		 * be able to iget the quota files directly.
		 */
		error = -libxfs_metafile_iget(mp, get_quota_inode(type),
				xfs_dqinode_metafile_type(type), &ip);
		if (error) {
			do_warn(
_("Could not open %s quota inode, error %d\n"),
					name, error);
			lose_quota_inode(type);
		}
	}

	if (lost_quota_inode(type)) {
		/*
		 * The inode was bad or missing, state that we'll make a new
		 * one even though we always create a new one.
		 */
		do_warn(_("resetting %s quota inode\n"), name);
		error =  -libxfs_dqinode_metadir_create(dp, type, &ip);
		if (error) {
			do_warn(
_("Couldn't create %s quota inode, error %d\n"),
					name, error);
			goto bad;
		}
	} else {
		struct xfs_trans	*tp;

		/* Erase parent pointers before we create the new link */
		try_erase_parent_ptrs(ip);

		error = -libxfs_dqinode_metadir_link(dp, type, ip);
		if (error) {
			do_warn(
_("Couldn't link %s quota inode, error %d\n"),
					name, error);
			goto bad;
		}

		/*
		 * Reset the link count to 1 because quota files are never
		 * hardlinked, but the link above probably bumped it.
		 */
		error = -libxfs_trans_alloc_inode(ip, &M_RES(mp)->tr_ichange,
				0, 0, false, &tp);
		if (!error) {
			set_nlink(VFS_I(ip), 1);
			libxfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
			error = -libxfs_trans_commit(tp);
		}
		if (error)
			do_error(
_("Couldn't reset link count on %s quota inode, error %d\n"),
					name, error);
	}

	/* Mark the inode in use. */
	mark_ino_inuse(mp, ip->i_ino, S_IFREG, dp->i_ino);
	mark_ino_metadata(mp, ip->i_ino);
	libxfs_irele(ip);
	return true;
bad:
	/* Zeroes qflags */
	quotacheck_skip();
	return false;
}

static void
reset_quota_metadir_inodes(
	struct xfs_mount	*mp)
{
	struct xfs_inode	*dp = NULL;
	int			error;

	if (!has_quota_inode(XFS_DQTYPE_USER) &&
	    !has_quota_inode(XFS_DQTYPE_GROUP) &&
	    !has_quota_inode(XFS_DQTYPE_PROJ))
		return;

	error = -libxfs_dqinode_mkdir_parent(mp, &dp);
	if (error)
		do_error(_("failed to create quota metadir (%d)\n"),
				error);

	mark_ino_inuse(mp, dp->i_ino, S_IFDIR, mp->m_metadirip->i_ino);
	mark_ino_metadata(mp, dp->i_ino);

	ensure_quota_file(dp, XFS_DQTYPE_USER);
	ensure_quota_file(dp, XFS_DQTYPE_GROUP);
	ensure_quota_file(dp, XFS_DQTYPE_PROJ);
	libxfs_irele(dp);
}

static int
reserve_ag_blocks(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag = NULL;
	int			error = 0;
	int			err2;

	mp->m_finobt_nores = false;

	while ((pag = xfs_perag_next(mp, pag))) {
		err2 = -libxfs_ag_resv_init(pag, NULL);
		if (err2 && !error)
			error = err2;
	}

	return error;
}

static void
unreserve_ag_blocks(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag = NULL;

	while ((pag = xfs_perag_next(mp, pag)))
		libxfs_ag_resv_free(pag);
}

void
phase6(xfs_mount_t *mp)
{
	ino_tree_node_t		*irec;
	bool			reserve_perag;
	int			error;
	int			i;

	parent_ptr_init(mp);

	orphanage_ino = 0;

	do_log(_("Phase 6 - check inode connectivity...\n"));

	incore_ext_teardown(mp);

	add_ino_ex_data(mp);

	/*
	 * verify existence of root directory - if we have to
	 * make one, it's ok for the incore data structs not to
	 * know about it since everything about it (and the other
	 * inodes in its chunk if a new chunk was created) are ok
	 */
	if (need_root_inode)  {
		if (!no_modify)  {
			do_warn(_("reinitializing root directory\n"));
			mk_root_dir(mp);
			need_root_inode = 0;
			need_root_dotdot = 0;
		} else  {
			do_warn(_("would reinitialize root directory\n"));
		}
	}

	if (!no_modify && xfs_has_metadir(mp)) {
		/*
		 * In write mode, we always rebuild the metadata directory
		 * tree, even if the old one was correct.  However, we still
		 * want to log something if we couldn't find the old root.
		 */
		if (need_metadir_inode)
			do_warn(_("reinitializing metadata root directory\n"));
		mk_metadir(mp);
		need_metadir_inode = false;
		need_metadir_dotdot = 0;
	} else if (need_metadir_inode) {
		do_warn(_("would reinitialize metadata root directory\n"));
	}

	reserve_perag = xfs_has_realtime(mp) && !no_modify;
	if (reserve_perag) {
		error = reserve_ag_blocks(mp);
		if (error) {
			if (error != ENOSPC)
				do_warn(
	_("could not reserve per-AG space to rebuild realtime metadata"));
			reserve_perag = false;
		}
	}

	if (xfs_has_rtgroups(mp))
		reset_rt_metadir_inodes(mp);
	else
		reset_rt_sb_inodes(mp);

	if (xfs_has_metadir(mp) && xfs_has_quota(mp) && !no_modify)
		reset_quota_metadir_inodes(mp);

	if (reserve_perag)
		unreserve_ag_blocks(mp);

	mark_standalone_inodes(mp);

	do_log(_("        - traversing filesystem ...\n"));

	irec = find_inode_rec(mp, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino),
				XFS_INO_TO_AGINO(mp, mp->m_sb.sb_rootino));

	/*
	 * we always have a root inode, even if it's free...
	 * if the root is free, forget it, lost+found is already gone
	 */
	if (is_inode_free(irec, 0) || !inode_isadir(irec, 0))  {
		need_root_inode = 1;
	}

	/*
	 * then process all inodes by walking incore inode tree
	 */
	traverse_ags(mp);

	/*
	 * any directories that had updated ".." entries, rebuild them now
	 */
	update_missing_dotdot_entries(mp);

	do_log(_("        - traversal finished ...\n"));
	do_log(_("        - moving disconnected inodes to %s ...\n"),
		ORPHANAGE);

	/*
	 * move all disconnected inodes to the orphanage
	 */
	for (i = 0; i < glob_agcount; i++)  {
		irec = findfirst_inode_rec(i);
		while (irec != NULL)  {
			check_for_orphaned_inodes(mp, i, irec);
			irec = next_ino_rec(irec);
		}
	}

	/* Check and repair directory parent pointers, if enabled. */
	check_parent_ptrs(mp);
	parent_ptr_free(mp);
}
