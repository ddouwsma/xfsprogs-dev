// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "globals.h"

/* global variables for xfs_repair */

/* arguments and argument flag variables */

char	*fs_name;		/* name of filesystem */
int	verbose;		/* verbose flag, mostly for debugging */


/* for reading stuff in manually (bypassing libsim) */

char	*iobuf;			/* large buffer */
int	iobuf_size;
char	*smallbuf;		/* small (1-4 page) buffer */
int	smallbuf_size;
int	sbbuf_size;

/* direct I/O info */

int	minio_align;		/* min I/O size and alignment */
int	mem_align;		/* memory alignment */
int	max_iosize;		/* max I/O size */

/* file descriptors */

int	fs_fd;			/* filesystem fd */

/* command-line flags */

int	verbose;
int	no_modify;
int	dangerously;		/* live dangerously ... fix ro mount */
int	isa_file;
int	zap_log;
int	dumpcore;		/* abort, not exit on fatal errs */
int	force_geo;		/* can set geo on low confidence info */
int	assume_xfs;		/* assume we have an xfs fs */
char	*log_name;		/* Name of log device */
int	log_spec;		/* Log dev specified as option */
char	*rt_name;		/* Name of realtime device */
int	rt_spec;		/* Realtime dev specified as option */
int	convert_lazy_count;	/* Convert lazy-count mode on/off */
int	lazy_count;		/* What to set if to if converting */
bool	features_changed;	/* did we change superblock feature bits? */
bool	add_inobtcount;		/* add inode btree counts to AGI */
bool	add_bigtime;		/* add support for timestamps up to 2486 */
bool	add_nrext64;
bool	add_exchrange;		/* add file content exchange support */

/* misc status variables */

int	primary_sb_modified;
int	bad_ino_btree;
int	copied_sunit;
int	fs_is_dirty;

/* for hunting down the root inode */

int	need_root_inode;
int	need_root_dotdot;

bool	need_metadir_inode;
int	need_metadir_dotdot;

int	need_rbmino;
int	need_rsumino;

int	lost_quotas;

/* configuration vars -- fs geometry dependent */

int		inodes_per_block;
unsigned int	glob_agcount;
int		chunks_pblock;	/* # of 64-ino chunks per allocation */
int		max_symlink_blocks;

/* inode tree records have full or partial backptr fields ? */

int	full_ino_ex_data;	/*
				 * if 1, use ino_ex_data_t component
				 * of ino_un union, if 0, use
				 * parent_list_t component.  see
				 * incore.h for more details
				 */

#define ORPHANAGE	"lost+found"

/* superblock counters */

uint64_t	sb_icount;	/* allocated (made) inodes */
uint64_t	sb_ifree;	/* free inodes */
uint64_t	sb_fdblocks;	/* free data blocks */
uint64_t	sb_frextents;	/* free realtime extents */

/* superblock geometry info */

xfs_extlen_t	sb_inoalignmt;
uint32_t	sb_unit;
uint32_t	sb_width;

time_t		report_interval;
uint64_t	*prog_rpt_done;

int		ag_stride;
int		thread_count;

/* If nonzero, simulate failure after this phase. */
int		fail_after_phase;

/*
 * Do we think we're going to be so low on disk space that we need to pack
 * all rebuilt btree blocks completely full to avoid running out of space?
 */
bool		need_packed_btrees;

/* quota inode numbers */
enum quotino_state {
	QI_STATE_UNKNOWN,
	QI_STATE_HAVE,
	QI_STATE_LOST,
};

static xfs_ino_t quotinos[3] = { NULLFSINO, NULLFSINO, NULLFSINO };
static enum quotino_state quotino_state[3];

static inline unsigned int quotino_off(xfs_dqtype_t type)
{
	switch (type) {
	case XFS_DQTYPE_USER:
		return 0;
	case XFS_DQTYPE_GROUP:
		return 1;
	case XFS_DQTYPE_PROJ:
		return 2;
	}

	ASSERT(0);
	return -1;
}

void
set_quota_inode(
	xfs_dqtype_t	type,
	xfs_ino_t	ino)
{
	unsigned int	off = quotino_off(type);

	quotinos[off] = ino;
	quotino_state[off] = QI_STATE_HAVE;
}

void
lose_quota_inode(
	xfs_dqtype_t	type)
{
	unsigned int	off = quotino_off(type);

	quotinos[off] = NULLFSINO;
	quotino_state[off] = QI_STATE_LOST;
}

void
clear_quota_inode(
	xfs_dqtype_t	type)
{
	unsigned int	off = quotino_off(type);

	quotinos[off] = NULLFSINO;
	quotino_state[off] = QI_STATE_UNKNOWN;
}

xfs_ino_t
get_quota_inode(
	xfs_dqtype_t	type)
{
	unsigned int	off = quotino_off(type);

	return quotinos[off];
}

bool
is_quota_inode(
	xfs_dqtype_t	type,
	xfs_ino_t	ino)
{
	unsigned int	off = quotino_off(type);

	return quotinos[off] == ino;
}

bool
is_any_quota_inode(
	xfs_ino_t		ino)
{
	unsigned int		i;

	for(i = 0; i < ARRAY_SIZE(quotinos); i++)
		if (quotinos[i] == ino)
			return true;
	return false;
}

bool
lost_quota_inode(
	xfs_dqtype_t	type)
{
	unsigned int	off = quotino_off(type);

	return quotino_state[off] == QI_STATE_LOST;
}

bool
has_quota_inode(
	xfs_dqtype_t	type)
{
	unsigned int	off = quotino_off(type);

	return quotino_state[off] == QI_STATE_HAVE;
}
