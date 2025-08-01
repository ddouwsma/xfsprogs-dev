// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "libfrog/util.h"
#include "libxfs.h"
#include <ctype.h>
#include <linux/blkzoned.h>
#include "libxfs/xfs_zones.h"
#include "xfs_multidisk.h"
#include "libxcmd.h"
#include "libfrog/fsgeom.h"
#include "libfrog/convert.h"
#include "libfrog/crc32cselftest.h"
#include "libfrog/dahashselftest.h"
#include "libfrog/fsproperties.h"
#include "proto.h"
#include <ini.h>

#define TERABYTES(count, blog)	((uint64_t)(count) << (40 - (blog)))
#define GIGABYTES(count, blog)	((uint64_t)(count) << (30 - (blog)))
#define MEGABYTES(count, blog)	((uint64_t)(count) << (20 - (blog)))

/*
 * Realistically, the log should never be smaller than 64MB.  Studies by the
 * kernel maintainer in early 2022 have shown a dramatic reduction in long tail
 * latency of the xlog grant head waitqueue when running a heavy metadata
 * update workload when the log size is at least 64MB.
 */
#define XFS_MIN_REALISTIC_LOG_BLOCKS(blog)	(MEGABYTES(64, (blog)))

/*
 * Use this macro before we have superblock and mount structure to
 * convert from basic blocks to filesystem blocks.
 */
#define	DTOBT(d, bl)	((xfs_rfsblock_t)((d) >> ((bl) - BBSHIFT)))

/*
 * amount (in bytes) we zero at the beginning and end of the device to
 * remove traces of other filesystems, raid superblocks, etc.
 */
#define WHACK_SIZE (128 * 1024)

/*
 * XXX: The configured block and sector sizes are defined as global variables so
 * that they don't need to be passed to getnum/cvtnum().
 */
static unsigned int		blocksize;
static unsigned int		sectorsize;

/*
 * Enums for each CLI parameter type are declared first so we can calculate the
 * maximum array size needed to hold them automatically.
 */
enum {
	B_SIZE = 0,
	B_MAX_OPTS,
};

enum {
	C_OPTFILE = 0,
	C_MAX_OPTS,
};

enum {
	D_AGCOUNT = 0,
	D_FILE,
	D_NAME,
	D_SIZE,
	D_SUNIT,
	D_SWIDTH,
	D_AGSIZE,
	D_SU,
	D_SW,
	D_SECTSIZE,
	D_NOALIGN,
	D_RTINHERIT,
	D_PROJINHERIT,
	D_EXTSZINHERIT,
	D_COWEXTSIZE,
	D_DAXINHERIT,
	D_CONCURRENCY,
	D_MAX_OPTS,
};

enum {
	I_ALIGN = 0,
	I_MAXPCT,
	I_PERBLOCK,
	I_SIZE,
	I_ATTR,
	I_PROJID32BIT,
	I_SPINODES,
	I_NREXT64,
	I_EXCHANGE,
	I_MAX_OPTS,
};

enum {
	L_AGNUM = 0,
	L_INTERNAL,
	L_SIZE,
	L_VERSION,
	L_SUNIT,
	L_SU,
	L_DEV,
	L_SECTSIZE,
	L_FILE,
	L_NAME,
	L_LAZYSBCNTR,
	L_CONCURRENCY,
	L_MAX_OPTS,
};

enum {
	N_SIZE = 0,
	N_VERSION,
	N_FTYPE,
	N_PARENT,
	N_MAX_OPTS,
};

enum {
	P_FILE = 0,
	P_SLASHES,
	P_MAX_OPTS,
};

enum {
	R_EXTSIZE = 0,
	R_SIZE,
	R_DEV,
	R_FILE,
	R_NAME,
	R_NOALIGN,
	R_RGCOUNT,
	R_RGSIZE,
	R_CONCURRENCY,
	R_ZONED,
	R_START,
	R_RESERVED,
	R_MAX_OPTS,
};

enum {
	S_SIZE = 0,
	S_SECTSIZE,
	S_MAX_OPTS,
};

enum {
	M_CRC = 0,
	M_FINOBT,
	M_UUID,
	M_RMAPBT,
	M_REFLINK,
	M_INOBTCNT,
	M_BIGTIME,
	M_AUTOFSCK,
	M_METADIR,
	M_UQUOTA,
	M_GQUOTA,
	M_PQUOTA,
	M_UQNOENFORCE,
	M_GQNOENFORCE,
	M_PQNOENFORCE,
	M_MAX_OPTS,
};

/*
 * Just define the max options array size manually to the largest
 * enum right now, leaving room for a NULL terminator at the end
 */
#define MAX_SUBOPTS	(D_MAX_OPTS + 1)

#define SUBOPT_NEEDS_VAL	(-1LL)
#define MAX_CONFLICTS	8
#define LAST_CONFLICT	(-1)

/*
 * Table for parsing mkfs parameters.
 *
 * Description of the structure members follows:
 *
 * name MANDATORY
 *   Name is a single char, e.g., for '-d file', name is 'd'.
 *
 * ini_section MANDATORY
 *   This field is required to connect each opt_params (that is to say, each
 *   option class) to a section in the config file. The only option class this
 *   is not required for is the config file specification class itself.
 *   The section name is a string, not longer than MAX_INI_NAME_LEN.
 *
 * subopts MANDATORY
 *   Subopts is a list of strings naming suboptions. In the example above,
 *   it would contain "file". The last entry of this list has to be NULL.
 *
 * subopt_params MANDATORY
 *   This is a list of structs tied with subopts. For each entry in subopts,
 *   a corresponding entry has to be defined:
 *
 * subopt_params struct:
 *   index MANDATORY
 *     This number, starting from zero, denotes which item in subopt_params
 *     it is. The index has to be the same as is the order in subopts list,
 *     so we can access the right item both in subopt_param and subopts.
 *
 *   seen INTERNAL
 *     Do not set this flag when definning a subopt. It is used to remeber that
 *     this subopt was already seen, for example for conflicts detection.
 *
 *   str_seen INTERNAL
 *     Do not set. It is used internally for respecification, when some options
 *     has to be parsed twice - at first as a string, then later as a number.
 *
 *   convert OPTIONAL
 *     A flag signalling whether the user-given value can use suffixes.
 *     If you want to allow the use of user-friendly values like 13k, 42G,
 *     set it to true.
 *
 *   is_power_2 OPTIONAL
 *     An optional flag for subopts where the given value has to be a power
 *     of two.
 *
 *   conflicts MANDATORY
 *     If your subopt is in a conflict with some other option, specify it.
 *     Accepts the .index values of the conflicting subopts and the last
 *     member of this list has to be LAST_CONFLICT.
 *
 *   minval, maxval OPTIONAL
 *     These options are used for automatic range check and they have to be
 *     always used together in pair. If you don't want to limit the max value,
 *     use something like UINT_MAX. If no value is given, then you must either
 *     supply your own validation, or refuse any value in the 'case
 *     X_SOMETHING' block. If you forget to define the min and max value, but
 *     call a standard function for validating user's value, it will cause an
 *     error message notifying you about this issue.
 *
 *     (Said in another way, you can't have minval and maxval both equal
 *     to zero. But if one value is different: minval=0 and maxval=1,
 *     then it is OK.)
 *
 *   defaultval MANDATORY
 *     The value used if user specifies the subopt, but no value.
 *     If the subopt accepts some values (-d file=[1|0]), then this
 *     sets what is used with simple specifying the subopt (-d file).
 *     A special SUBOPT_NEEDS_VAL can be used to require a user-given
 *     value in any case.
 */
struct opt_params {
	const char	name;
#define MAX_INI_NAME_LEN	32
	const char	ini_section[MAX_INI_NAME_LEN];
	const char	*subopts[MAX_SUBOPTS];

	struct subopt_param {
		int		index;
		bool		seen;
		bool		str_seen;
		bool		convert;
		bool		is_power_2;
		struct _conflict {
			struct opt_params	*opts;
			int			subopt;
		}		conflicts[MAX_CONFLICTS];
		long long	minval;
		long long	maxval;
		long long	defaultval;
	}		subopt_params[MAX_SUBOPTS];
};

/*
 * The two dimensional conflict array requires some initialisations to know
 * about tables that haven't yet been defined. Work around this ordering
 * issue with extern definitions here.
 */
static struct opt_params sopts;

static struct opt_params bopts = {
	.name = 'b',
	.ini_section = "block",
	.subopts = {
		[B_SIZE] = "size",
		[B_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = B_SIZE,
		  .convert = true,
		  .is_power_2 = true,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = XFS_MIN_BLOCKSIZE,
		  .maxval = XFS_MAX_BLOCKSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
	},
};

/*
 * Config file specification. Usage is:
 *
 * mkfs.xfs -c options=<name>
 *
 * A subopt is used for the filename so in future we can extend the behaviour
 * of the config file (e.g. specified defaults rather than options) if we ever
 * have a need to do that sort of thing.
 */
static struct opt_params copts = {
	.name = 'c',
	.subopts = {
		[C_OPTFILE] = "options",
		[C_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = C_OPTFILE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
	},
};

static struct opt_params dopts = {
	.name = 'd',
	.ini_section = "data",
	.subopts = {
		[D_AGCOUNT] = "agcount",
		[D_FILE] = "file",
		[D_NAME] = "name",
		[D_SIZE] = "size",
		[D_SUNIT] = "sunit",
		[D_SWIDTH] = "swidth",
		[D_AGSIZE] = "agsize",
		[D_SU] = "su",
		[D_SW] = "sw",
		[D_SECTSIZE] = "sectsize",
		[D_NOALIGN] = "noalign",
		[D_RTINHERIT] = "rtinherit",
		[D_PROJINHERIT] = "projinherit",
		[D_EXTSZINHERIT] = "extszinherit",
		[D_COWEXTSIZE] = "cowextsize",
		[D_DAXINHERIT] = "daxinherit",
		[D_CONCURRENCY] = "concurrency",
		[D_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = D_AGCOUNT,
		  .conflicts = { { &dopts, D_AGSIZE },
				 { &dopts, D_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .minval = 1,
		  .maxval = XFS_MAX_AGNUMBER,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_FILE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = D_NAME,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SIZE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = XFS_AG_MIN_BYTES,
		  .maxval = LLONG_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SUNIT,
		  .conflicts = { { &dopts, D_NOALIGN },
				 { &dopts, D_SU },
				 { &dopts, D_SW },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SWIDTH,
		  .conflicts = { { &dopts, D_NOALIGN },
				 { &dopts, D_SU },
				 { &dopts, D_SW },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_AGSIZE,
		  .conflicts = { { &dopts, D_AGCOUNT },
				 { &dopts, D_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = XFS_AG_MIN_BYTES,
		  .maxval = XFS_AG_MAX_BYTES,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SU,
		  .conflicts = { { &dopts, D_NOALIGN },
				 { &dopts, D_SUNIT },
				 { &dopts, D_SWIDTH },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SW,
		  .conflicts = { { &dopts, D_NOALIGN },
				 { &dopts, D_SUNIT },
				 { &dopts, D_SWIDTH },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_SECTSIZE,
		  .conflicts = { { &sopts, S_SIZE },
				 { &sopts, S_SECTSIZE },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .is_power_2 = true,
		  .minval = XFS_MIN_SECTORSIZE,
		  .maxval = XFS_MAX_SECTORSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_NOALIGN,
		  .conflicts = { { &dopts, D_SU },
				 { &dopts, D_SW },
				 { &dopts, D_SUNIT },
				 { &dopts, D_SWIDTH },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = D_RTINHERIT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = D_PROJINHERIT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_EXTSZINHERIT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_COWEXTSIZE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = D_DAXINHERIT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = D_CONCURRENCY,
		  .conflicts = { { &dopts, D_AGCOUNT },
				 { &dopts, D_AGSIZE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = INT_MAX,
		  .defaultval = 1,
		},
	},
};


static struct opt_params iopts = {
	.name = 'i',
	.ini_section = "inode",
	.subopts = {
		[I_ALIGN] = "align",
		[I_MAXPCT] = "maxpct",
		[I_PERBLOCK] = "perblock",
		[I_SIZE] = "size",
		[I_ATTR] = "attr",
		[I_PROJID32BIT] = "projid32bit",
		[I_SPINODES] = "sparse",
		[I_NREXT64] = "nrext64",
		[I_EXCHANGE] = "exchange",
		[I_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = I_ALIGN,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = I_MAXPCT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 100,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = I_PERBLOCK,
		  .conflicts = { { &iopts, I_SIZE },
				 { NULL, LAST_CONFLICT } },
		  .is_power_2 = true,
		  .minval = XFS_MIN_INODE_PERBLOCK,
		  .maxval = XFS_MAX_BLOCKSIZE / XFS_DINODE_MIN_SIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = I_SIZE,
		  .conflicts = { { &iopts, I_PERBLOCK },
				 { NULL, LAST_CONFLICT } },
		  .is_power_2 = true,
		  .minval = XFS_DINODE_MIN_SIZE,
		  .maxval = XFS_DINODE_MAX_SIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = I_ATTR,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 2,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = I_PROJID32BIT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = I_SPINODES,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = I_NREXT64,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = I_EXCHANGE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
	},
};

static struct opt_params lopts = {
	.name = 'l',
	.ini_section = "log",
	.subopts = {
		[L_AGNUM] = "agnum",
		[L_INTERNAL] = "internal",
		[L_SIZE] = "size",
		[L_VERSION] = "version",
		[L_SUNIT] = "sunit",
		[L_SU] = "su",
		[L_DEV] = "logdev",
		[L_SECTSIZE] = "sectsize",
		[L_FILE] = "file",
		[L_NAME] = "name",
		[L_LAZYSBCNTR] = "lazy-count",
		[L_CONCURRENCY] = "concurrency",
		[L_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = L_AGNUM,
		  .conflicts = { { &lopts, L_DEV },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = UINT_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_INTERNAL,
		  .conflicts = { { &lopts, L_FILE },
				 { &lopts, L_DEV },
				 { &lopts, L_SECTSIZE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = L_SIZE,
		  .conflicts = { { &lopts, L_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 2 * 1024 * 1024LL,	/* XXX: XFS_MIN_LOG_BYTES */
		  .maxval = XFS_MAX_LOG_BYTES,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_VERSION,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 1,
		  .maxval = 2,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_SUNIT,
		  .conflicts = { { &lopts, L_SU },
				 { NULL, LAST_CONFLICT } },
		  .minval = 1,
		  .maxval = BTOBB(XLOG_MAX_RECORD_BSIZE),
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_SU,
		  .conflicts = { { &lopts, L_SUNIT },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = BBTOB(1),
		  .maxval = XLOG_MAX_RECORD_BSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_DEV,
		  .conflicts = { { &lopts, L_AGNUM },
				 { &lopts, L_NAME },
				 { &lopts, L_INTERNAL },
				 { &lopts, L_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_SECTSIZE,
		  .conflicts = { { &lopts, L_INTERNAL },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .is_power_2 = true,
		  .minval = XFS_MIN_SECTORSIZE,
		  .maxval = XFS_MAX_SECTORSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_FILE,
		  .conflicts = { { &lopts, L_INTERNAL },
				 { &lopts, L_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = L_NAME,
		  .conflicts = { { &lopts, L_AGNUM },
				 { &lopts, L_DEV },
				 { &lopts, L_INTERNAL },
				 { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = L_LAZYSBCNTR,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = L_CONCURRENCY,
		  .conflicts = { { &lopts, L_SIZE },
				 { &lopts, L_FILE },
				 { &lopts, L_DEV },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = INT_MAX,
		  .defaultval = 1,
		},
	},
};

static struct opt_params nopts = {
	.name = 'n',
	.ini_section = "naming",
	.subopts = {
		[N_SIZE] = "size",
		[N_VERSION] = "version",
		[N_FTYPE] = "ftype",
		[N_PARENT] = "parent",
		[N_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = N_SIZE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .is_power_2 = true,
		  .minval = 1 << XFS_MIN_REC_DIRSIZE,
		  .maxval = XFS_MAX_BLOCKSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = N_VERSION,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 2,
		  .maxval = 2,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = N_FTYPE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = N_PARENT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},


	},
};

static struct opt_params popts = {
	.name = 'p',
	.ini_section = "proto",
	.subopts = {
		[P_FILE] = "file",
		[P_SLASHES] = "slashes_are_spaces",
		[P_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = P_FILE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = P_SLASHES,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
	},
};

static struct opt_params ropts = {
	.name = 'r',
	.ini_section = "realtime",
	.subopts = {
		[R_EXTSIZE] = "extsize",
		[R_SIZE] = "size",
		[R_DEV] = "rtdev",
		[R_FILE] = "file",
		[R_NAME] = "name",
		[R_NOALIGN] = "noalign",
		[R_RGCOUNT] = "rgcount",
		[R_RGSIZE] = "rgsize",
		[R_CONCURRENCY] = "concurrency",
		[R_ZONED] = "zoned",
		[R_START] = "start",
		[R_RESERVED] = "reserved",
		[R_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = R_EXTSIZE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = XFS_MIN_RTEXTSIZE,
		  .maxval = XFS_MAX_RTEXTSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_SIZE,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = LLONG_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_DEV,
		  .conflicts = { { &ropts, R_NAME },
			         { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_FILE,
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		},
		{ .index = R_NAME,
		  .conflicts = { { &ropts, R_DEV },
			         { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_NOALIGN,
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		},
		{ .index = R_RGCOUNT,
		  .conflicts = { { &ropts, R_RGSIZE },
				 { &ropts, R_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .minval = 1,
		  .maxval = XFS_MAX_RGNUMBER,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_RGSIZE,
		  .conflicts = { { &ropts, R_RGCOUNT },
				 { &ropts, R_CONCURRENCY },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = (unsigned long long)XFS_MAX_RGBLOCKS << XFS_MAX_BLOCKSIZE_LOG,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_CONCURRENCY,
		  .conflicts = { { &ropts, R_RGCOUNT },
				 { &ropts, R_RGSIZE },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = INT_MAX,
		  .defaultval = 1,
		},
		{ .index = R_ZONED,
		  .conflicts = { { &ropts, R_EXTSIZE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = R_START,
		  .conflicts = { { &ropts, R_DEV },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = LLONG_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = R_RESERVED,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .minval = 0,
		  .maxval = LLONG_MAX,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
	},
};

static struct opt_params sopts = {
	.name = 's',
	.ini_section = "sector",
	.subopts = {
		[S_SIZE] = "size",
		[S_SECTSIZE] = "sectsize",
		[S_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = S_SIZE,
		  .conflicts = { { &sopts, S_SECTSIZE },
				 { &dopts, D_SECTSIZE },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .is_power_2 = true,
		  .minval = XFS_MIN_SECTORSIZE,
		  .maxval = XFS_MAX_SECTORSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = S_SECTSIZE,
		  .conflicts = { { &sopts, S_SIZE },
				 { &dopts, D_SECTSIZE },
				 { NULL, LAST_CONFLICT } },
		  .convert = true,
		  .is_power_2 = true,
		  .minval = XFS_MIN_SECTORSIZE,
		  .maxval = XFS_MAX_SECTORSIZE,
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
	},
};

static struct opt_params mopts = {
	.name = 'm',
	.ini_section = "metadata",
	.subopts = {
		[M_CRC] = "crc",
		[M_FINOBT] = "finobt",
		[M_UUID] = "uuid",
		[M_RMAPBT] = "rmapbt",
		[M_REFLINK] = "reflink",
		[M_INOBTCNT] = "inobtcount",
		[M_BIGTIME] = "bigtime",
		[M_AUTOFSCK] = "autofsck",
		[M_METADIR] = "metadir",
		[M_UQUOTA] = "uquota",
		[M_GQUOTA] = "gquota",
		[M_PQUOTA] = "pquota",
		[M_UQNOENFORCE] = "uqnoenforce",
		[M_GQNOENFORCE] = "gqnoenforce",
		[M_PQNOENFORCE] = "pqnoenforce",
		[M_MAX_OPTS] = NULL,
	},
	.subopt_params = {
		{ .index = M_CRC,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_FINOBT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_UUID,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .defaultval = SUBOPT_NEEDS_VAL,
		},
		{ .index = M_RMAPBT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_REFLINK,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_INOBTCNT,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_BIGTIME,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_AUTOFSCK,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_METADIR,
		  .conflicts = { { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_UQUOTA,
		  .conflicts = { { &mopts, M_UQNOENFORCE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_GQUOTA,
		  .conflicts = { { &mopts, M_GQNOENFORCE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_PQUOTA,
		  .conflicts = { { &mopts, M_GQNOENFORCE },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_UQNOENFORCE,
		  .conflicts = { { &mopts, M_UQUOTA },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_GQNOENFORCE,
		  .conflicts = { { &mopts, M_GQUOTA },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
		{ .index = M_PQNOENFORCE,
		  .conflicts = { { &mopts, M_PQUOTA },
				 { NULL, LAST_CONFLICT } },
		  .minval = 0,
		  .maxval = 1,
		  .defaultval = 1,
		},
	},
};

/* quick way of checking if a parameter was set on the CLI */
static bool
cli_opt_set(
	struct opt_params	*opts,
	int			subopt)
{
	return opts->subopt_params[subopt].seen ||
	       opts->subopt_params[subopt].str_seen;
}

/*
 * Options configured on the command line.
 *
 * This stores all the specific config parameters the user sets on the command
 * line. We do not use these values directly - they are inputs to the mkfs
 * geometry validation and override any default configuration value we have.
 *
 * We don't keep flags to indicate what parameters are set - if we need to check
 * if an option was set on the command line, we check the relevant entry in the
 * option table which records whether it was specified in the .seen and
 * .str_seen variables in the table.
 *
 * Some parameters are stored as strings for post-parsing after their dependent
 * options have been resolved (e.g. block size and sector size have been parsed
 * and validated).
 *
 * This allows us to check that values have been set without needing separate
 * flags for each value, and hence avoids needing to record and check for each
 * specific option that can set the value later on in the code. In the cases
 * where we don't have a cli_params structure around, the above cli_opt_set()
 * function can be used.
 */
struct sb_feat_args {
	int	log_version;
	int	attr_version;
	int	dir_version;
	bool	inode_align;		/* XFS_SB_VERSION_ALIGNBIT */
	bool	nci;			/* XFS_SB_VERSION_BORGBIT */
	bool	lazy_sb_counters;	/* XFS_SB_VERSION2_LAZYSBCOUNTBIT */
	bool	parent_pointers;	/* XFS_SB_VERSION2_PARENTBIT */
	bool	projid32bit;		/* XFS_SB_VERSION2_PROJID32BIT */
	bool	crcs_enabled;		/* XFS_SB_VERSION2_CRCBIT */
	bool	dirftype;		/* XFS_SB_VERSION2_FTYPE */
	bool	finobt;			/* XFS_SB_FEAT_RO_COMPAT_FINOBT */
	bool	spinodes;		/* XFS_SB_FEAT_INCOMPAT_SPINODES */
	bool	rmapbt;			/* XFS_SB_FEAT_RO_COMPAT_RMAPBT */
	bool	reflink;		/* XFS_SB_FEAT_RO_COMPAT_REFLINK */
	bool	inobtcnt;		/* XFS_SB_FEAT_RO_COMPAT_INOBTCNT */
	bool	bigtime;		/* XFS_SB_FEAT_INCOMPAT_BIGTIME */
	bool	metadir;		/* XFS_SB_FEAT_INCOMPAT_METADIR */
	bool	nodalign;
	bool	nortalign;
	bool	nrext64;
	bool	exchrange;		/* XFS_SB_FEAT_INCOMPAT_EXCHRANGE */
	bool	zoned;
	bool	zone_gaps;

	uint16_t qflags;
};

struct cli_params {
	int	sectorsize;
	int	blocksize;

	char	*cfgfile;
	char	*protofile;

	enum fsprop_autofsck autofsck;

	/* parameters that depend on sector/block size being validated. */
	char	*dsize;
	char	*agsize;
	char	*rgsize;
	char	*dsu;
	char	*dirblocksize;
	char	*logsize;
	char	*lsu;
	char	*rtextsize;
	char	*rtsize;
	char	*rtstart;
	uint64_t rtreserved;

	/* parameters where 0 is a valid CLI value */
	int	dsunit;
	int	dswidth;
	int	dsw;
	int64_t	logagno;
	int	loginternal;
	int	lsunit;
	int	is_supported;
	int	proto_slashes_are_spaces;
	int	data_concurrency;
	int	log_concurrency;
	int	rtvol_concurrency;
	int	imaxpct;

	/* parameters where 0 is not a valid value */
	int64_t	agcount;
	int64_t	rgcount;
	int	inodesize;
	int	inopblock;
	int	lsectorsize;
	uuid_t	uuid;

	/* feature flags that are set */
	struct sb_feat_args	sb_feat;

	/* root inode characteristics */
	struct fsxattr		fsx;

	/* libxfs device setup */
	struct libxfs_init	*xi;
};

/*
 * Calculated filesystem feature and geometry information.
 *
 * This structure contains the information we will use to create the on-disk
 * filesystem from. The validation and calculation code uses it to store all the
 * temporary and final config state for the filesystem.
 *
 * The information in this structure will contain a mix of validated CLI input
 * variables, default feature state and calculated values that are needed to
 * construct the superblock and other on disk features. These are all in one
 * place so that we don't have to pass handfuls of seemingly arbitrary variables
 * around to different functions to do the work we need to do.
 */
struct mkfs_params {
	int		blocksize;
	int		blocklog;
	int		sectorsize;
	int		sectorlog;
	int		lsectorsize;
	int		lsectorlog;
	int		dirblocksize;
	int		dirblocklog;
	int		inodesize;
	int		inodelog;
	int		inopblock;

	uint64_t	dblocks;
	uint64_t	logblocks;
	uint64_t	rtblocks;
	uint64_t	rtextblocks;
	uint64_t	rtextents;
	uint64_t	rtbmblocks;	/* rt bitmap blocks */

	int		dsunit;		/* in FSBs */
	int		dswidth;	/* in FSBs */
	int		lsunit;		/* in FSBs */

	uint64_t	agsize;
	uint64_t	agcount;

	uint64_t	rgsize;
	uint64_t	rgcount;

	int		imaxpct;

	bool		loginternal;
	uint64_t	logstart;
	uint64_t	logagno;

	uuid_t		uuid;
	char		*label;

	struct sb_feat_args	sb_feat;
	uint64_t	rtstart;
	uint64_t	rtreserved;
};

/*
 * Default filesystem features and configuration values
 *
 * This structure contains the default mkfs values that are to be used when
 * a user does not specify the option on the command line. We do not use these
 * values directly - they are inputs to the mkfs geometry validation and
 * calculations.
 */
struct mkfs_default_params {
	char	*source;	/* where the defaults came from */

	int	sectorsize;
	int	blocksize;

	/* feature flags that are set */
	struct sb_feat_args	sb_feat;

	/* root inode characteristics */
	struct fsxattr		fsx;
};

static void __attribute__((noreturn))
usage( void )
{
	fprintf(stderr, _("Usage: %s\n\
/* blocksize */		[-b size=num]\n\
/* config file */	[-c options=xxx]\n\
/* metadata */		[-m crc=0|1,finobt=0|1,uuid=xxx,rmapbt=0|1,reflink=0|1,\n\
			    inobtcount=0|1,bigtime=0|1,autofsck=xxx,\n\
			    metadir=0|1]\n\
/* quota */		[-m uquota|uqnoenforce,gquota|gqnoenforce,\n\
			    pquota|pqnoenforce]\n\
/* data subvol */	[-d agcount=n,agsize=n,file,name=xxx,size=num,\n\
			    (sunit=value,swidth=value|su=num,sw=num|noalign),\n\
			    sectsize=num,concurrency=num]\n\
/* force overwrite */	[-f]\n\
/* inode size */	[-i perblock=n|size=num,maxpct=n,attr=0|1|2,\n\
			    projid32bit=0|1,sparse=0|1,nrext64=0|1,\n\
			    exchange=0|1]\n\
/* no discard */	[-K]\n\
/* log subvol */	[-l agnum=n,internal,size=num,logdev=xxx,version=n\n\
			    sunit=value|su=num,sectsize=num,lazy-count=0|1,\n\
			    concurrency=num]\n\
/* label */		[-L label (maximum 12 characters)]\n\
/* naming */		[-n size=num,version=2|ci,ftype=0|1,parent=0|1]]\n\
/* no-op info only */	[-N]\n\
/* prototype file */	[-p fname]\n\
/* quiet */		[-q]\n\
/* realtime subvol */	[-r extsize=num,size=num,rtdev=xxx,rgcount=n,rgsize=n,\n\
			    concurrency=num,zoned=0|1,start=n,reserved=n]\n\
/* sectorsize */	[-s size=num]\n\
/* version */		[-V]\n\
			devicename\n\
<devicename> is required unless -d name=xxx is given.\n\
<num> is xxx (bytes), xxxs (sectors), xxxb (fs blocks), xxxk (xxx KiB),\n\
      xxxm (xxx MiB), xxxg (xxx GiB), xxxt (xxx TiB) or xxxp (xxx PiB).\n\
<value> is xxx (512 byte blocks).\n"),
		progname);
	exit(1);
}

static void
conflict(
	struct opt_params       *opts,
	int			option,
	struct opt_params       *con_opts,
	int			conflict)
{
	fprintf(stderr, _("Cannot specify both -%c %s and -%c %s\n"),
			con_opts->name, con_opts->subopts[conflict],
			opts->name, opts->subopts[option]);
	usage();
}


static void
illegal(
	const char	*value,
	const char	*opt)
{
	fprintf(stderr, _("Invalid value %s for -%s option\n"), value, opt);
	usage();
}

static int
ispow2(
	unsigned int	i)
{
	return (i & (i - 1)) == 0;
}

static void __attribute__((noreturn))
reqval(
	char		opt,
	const char	*tab[],
	int		idx)
{
	fprintf(stderr, _("-%c %s option requires a value\n"), opt, tab[idx]);
	usage();
}

static void
respec(
	char		opt,
	const char	*tab[],
	int		idx)
{
	fprintf(stderr, "-%c ", opt);
	if (tab)
		fprintf(stderr, "%s ", tab[idx]);
	fprintf(stderr, _("option respecified\n"));
	usage();
}

static void
unknown(
	const char	opt,
	const char	*s)
{
	fprintf(stderr, _("unknown option -%c %s\n"), opt, s);
	usage();
}

static void
invalid_cfgfile_opt(
	const char	*filename,
	const char	*section,
	const char	*name,
	const char	*value)
{
	fprintf(stderr, _("%s: invalid config file option: [%s]: %s=%s\n"),
		filename, section, name, value);
}

static int
nr_cpus(void)
{
	static long	cpus = -1;

	if (cpus < 0)
		cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpus < 0)
		return 0;

	return min(INT_MAX, cpus);
}

static void
check_device_type(
	struct libxfs_dev	*dev,
	bool			no_size,
	bool			dry_run,
	const char		*optname)
{
	struct stat statbuf;

	if (dev->isfile && (no_size || !dev->name)) {
		fprintf(stderr,
	_("if -%s file then -%s name and -%s size are required\n"),
			optname, optname, optname);
		usage();
	}

	if (!dev->name) {
		fprintf(stderr, _("No device name specified\n"));
		usage();
	}

	if (stat(dev->name, &statbuf)) {
		if (errno == ENOENT && dev->isfile) {
			if (!dry_run)
				dev->create = 1;
			return;
		}

		fprintf(stderr,
	_("Error accessing specified device %s: %s\n"),
				dev->name, strerror(errno));
		usage();
		return;
	}

	/*
	 * We only want to completely truncate and recreate an existing file if
	 * we were specifically told it was a file. Set the create flag only in
	 * this case to trigger that behaviour.
	 */
	if (S_ISREG(statbuf.st_mode)) {
		if (!dev->isfile)
			dev->isfile = 1;
		else if (!dry_run)
			dev->create = 1;
		return;
	}

	if (S_ISBLK(statbuf.st_mode)) {
		if (dev->isfile) {
			fprintf(stderr,
	_("specified \"-%s file\" on a block device %s\n"),
				optname, dev->name);
			usage();
		}
		return;
	}

	fprintf(stderr,
	_("specified device %s not a file or block device\n"),
		dev->name);
	usage();
}

static void
validate_overwrite(
	const char	*name,
	bool		force_overwrite)
{
	if (!force_overwrite && check_overwrite(name)) {
		fprintf(stderr,
	_("%s: Use the -f option to force overwrite.\n"),
			progname);
		exit(1);
	}

}

static void
validate_ag_geometry(
	int		blocklog,
	uint64_t	dblocks,
	uint64_t	agsize,
	uint64_t	agcount)
{
	if (agsize < XFS_AG_MIN_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("agsize (%lld blocks) too small, need at least %lld blocks\n"),
			(long long)agsize,
			(long long)XFS_AG_MIN_BLOCKS(blocklog));
		usage();
	}

	if (agsize > XFS_AG_MAX_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("agsize (%lld blocks) too big, maximum is %lld blocks\n"),
			(long long)agsize,
			(long long)XFS_AG_MAX_BLOCKS(blocklog));
		usage();
	}

	if (agsize > dblocks) {
		fprintf(stderr,
	_("agsize (%lld blocks) too big, data area is %lld blocks\n"),
			(long long)agsize, (long long)dblocks);
			usage();
	}

	if (agsize < XFS_AG_MIN_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("too many allocation groups for size = %lld\n"),
				(long long)agsize);
		fprintf(stderr, _("need at most %lld allocation groups\n"),
			(long long)(dblocks / XFS_AG_MIN_BLOCKS(blocklog) +
				(dblocks % XFS_AG_MIN_BLOCKS(blocklog) != 0)));
		usage();
	}

	if (agsize > XFS_AG_MAX_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("too few allocation groups for size = %lld\n"), (long long)agsize);
		fprintf(stderr,
	_("need at least %lld allocation groups\n"),
		(long long)(dblocks / XFS_AG_MAX_BLOCKS(blocklog) +
			(dblocks % XFS_AG_MAX_BLOCKS(blocklog) != 0)));
		usage();
	}

	/*
	 * If the last AG is too small, reduce the filesystem size
	 * and drop the blocks.
	 */
	if ( dblocks % agsize != 0 &&
	     (dblocks % agsize < XFS_AG_MIN_BLOCKS(blocklog))) {
		fprintf(stderr,
	_("last AG size %lld blocks too small, minimum size is %lld blocks\n"),
			(long long)(dblocks % agsize),
			(long long)XFS_AG_MIN_BLOCKS(blocklog));
		usage();
	}

	/*
	 * If agcount is too large, make it smaller.
	 */
	if (agcount > XFS_MAX_AGNUMBER + 1) {
		fprintf(stderr,
	_("%lld allocation groups is too many, maximum is %lld\n"),
			(long long)agcount, (long long)XFS_MAX_AGNUMBER + 1);
		usage();
	}
}

static void
zero_old_xfs_structures(
	struct libxfs_init	*xi,
	xfs_sb_t		*new_sb)
{
	void 			*buf;
	xfs_sb_t 		sb;
	uint32_t		bsize;
	int			i;
	xfs_off_t		off;

	/*
	 * We open regular files with O_TRUNC|O_CREAT. Nothing to do here...
	 */
	if (xi->data.isfile && xi->data.create)
		return;

	/*
	 * read in existing filesystem superblock, use its geometry
	 * settings and zero the existing secondary superblocks.
	 */
	buf = memalign(libxfs_device_alignment(), new_sb->sb_sectsize);
	if (!buf) {
		fprintf(stderr,
	_("error reading existing superblock -- failed to memalign buffer\n"));
		return;
	}
	memset(buf, 0, new_sb->sb_sectsize);

	/*
	 * If we are creating an image file, it might be of zero length at this
	 * point in time. Hence reading the existing superblock is going to
	 * return zero bytes. It's not a failure we need to warn about in this
	 * case.
	 */
	off = pread(xi->data.fd, buf, new_sb->sb_sectsize, 0);
	if (off != new_sb->sb_sectsize) {
		if (!xi->data.isfile)
			fprintf(stderr,
	_("error reading existing superblock: %s\n"),
				strerror(errno));
		goto done;
	}
	libxfs_sb_from_disk(&sb, buf);

	/*
	 * perform same basic superblock validation to make sure we
	 * actually zero secondary blocks
	 */
	if (sb.sb_magicnum != XFS_SB_MAGIC || sb.sb_blocksize == 0)
		goto done;

	for (bsize = 1, i = 0; bsize < sb.sb_blocksize &&
			i < sizeof(sb.sb_blocksize) * NBBY; i++)
		bsize <<= 1;

	if (i < XFS_MIN_BLOCKSIZE_LOG || i > XFS_MAX_BLOCKSIZE_LOG ||
			i != sb.sb_blocklog)
		goto done;

	if (sb.sb_dblocks > ((uint64_t)sb.sb_agcount * sb.sb_agblocks) ||
			sb.sb_dblocks < ((uint64_t)(sb.sb_agcount - 1) *
					 sb.sb_agblocks + XFS_MIN_AG_BLOCKS))
		goto done;

	/*
	 * block size and basic geometry seems alright, zero the secondaries.
	 */
	memset(buf, 0, new_sb->sb_sectsize);
	off = 0;
	for (i = 1; i < sb.sb_agcount; i++)  {
		off += sb.sb_agblocks;
		if (pwrite(xi->data.fd, buf, new_sb->sb_sectsize,
					off << sb.sb_blocklog) == -1)
			break;
	}
done:
	free(buf);
}

static void
discard_blocks(int fd, uint64_t nsectors, int quiet)
{
	uint64_t	offset = 0;
	/* Discard the device 2G at a time */
	const uint64_t	step = 2ULL << 30;
	const uint64_t	count = BBTOB(nsectors);

	/*
	 * The block discarding happens in smaller batches so it can be
	 * interrupted prematurely
	 */
	while (offset < count) {
		uint64_t	tmp_step = min(step, count - offset);

		/*
		 * We intentionally ignore errors from the discard ioctl. It is
		 * not necessary for the mkfs functionality but just an
		 * optimization. However we should stop on error.
		 */
		if (platform_discard_blocks(fd, offset, tmp_step) == 0) {
			if (offset == 0 && !quiet) {
				printf("Discarding blocks...");
				fflush(stdout);
			}
		} else {
			if (offset > 0 && !quiet)
				printf("\n");
			return;
		}

		offset += tmp_step;
	}
	if (offset > 0 && !quiet)
		printf("Done.\n");
}

static void
reset_zones(
	struct mkfs_params	*cfg,
	int			fd,
	uint64_t		start_sector,
	uint64_t		nsectors,
	int			quiet)
{
	struct blk_zone_range range = {
		.sector		= start_sector,
		.nr_sectors	= nsectors,
	};

	if (!quiet) {
		printf("Resetting zones...");
		fflush(stdout);
	}

	if (ioctl(fd, BLKRESETZONE, &range) < 0) {
		if (!quiet)
			printf(" FAILED (%d)\n", -errno);
		exit(1);
	}

	if (!quiet)
		printf("Done.\n");
}

static __attribute__((noreturn)) void
illegal_option(
	const char		*value,
	struct opt_params	*opts,
	int			index,
	const char		*reason)
{
	fprintf(stderr,
		_("Invalid value %s for -%c %s option. %s\n"),
		value, opts->name, opts->subopts[index],
		reason);
	usage();
}

/*
 * Check for conflicts and option respecification.
 */
static void
check_opt(
	struct opt_params	*opts,
	int			index,
	bool			str_seen)
{
	struct subopt_param	*sp = &opts->subopt_params[index];
	int			i;

	if (sp->index != index) {
		fprintf(stderr,
	_("Developer screwed up option parsing (%d/%d)! Please report!\n"),
			sp->index, index);
		reqval(opts->name, opts->subopts, index);
	}

	/*
	 * Check for respecification of the option. This is more complex than it
	 * seems because some options are parsed twice - once as a string during
	 * input parsing, then later the string is passed to getnum for
	 * conversion into a number and bounds checking. Hence the two variables
	 * used to track the different uses based on the @str parameter passed
	 * to us.
	 */
	if (!str_seen) {
		if (sp->seen)
			respec(opts->name, opts->subopts, index);
		sp->seen = true;
	} else {
		if (sp->str_seen)
			respec(opts->name, opts->subopts, index);
		sp->str_seen = true;
	}

	/* check for conflicts with the option */
	for (i = 0; i < MAX_CONFLICTS; i++) {
		struct _conflict *con = &sp->conflicts[i];

		if (con->subopt == LAST_CONFLICT)
			break;
		if (con->opts->subopt_params[con->subopt].seen ||
		    con->opts->subopt_params[con->subopt].str_seen)
			conflict(opts, index, con->opts, con->subopt);
	}
}

static long long
getnum(
	const char		*str,
	struct opt_params	*opts,
	int			index)
{
	struct subopt_param	*sp = &opts->subopt_params[index];
	long long		c;

	check_opt(opts, index, false);
	/* empty strings might just return a default value */
	if (!str || *str == '\0') {
		if (sp->defaultval == SUBOPT_NEEDS_VAL)
			reqval(opts->name, opts->subopts, index);
		return sp->defaultval;
	}

	if (sp->minval == 0 && sp->maxval == 0) {
		fprintf(stderr,
			_("Option -%c %s has undefined minval/maxval."
			  "Can't verify value range. This is a bug.\n"),
			opts->name, opts->subopts[index]);
		exit(1);
	}

	/*
	 * Some values are pure numbers, others can have suffixes that define
	 * the units of the number. Those get passed to cvtnum(), otherwise we
	 * convert it ourselves to guarantee there is no trailing garbage in the
	 * number.
	 */
	if (sp->convert) {
		c = cvtnum(blocksize, sectorsize, str);
		if (c == -1LL) {
			illegal_option(str, opts, index,
				_("Not a valid value or illegal suffix"));
		}
	} else {
		char		*str_end;

		c = strtoll(str, &str_end, 0);
		if (c == 0 && str_end == str)
			illegal_option(str, opts, index,
					_("Value not recognized as number."));
		if (*str_end != '\0')
			illegal_option(str, opts, index,
					_("Unit suffixes are not allowed."));
	}

	/* Validity check the result. */
	if (c < sp->minval)
		illegal_option(str, opts, index, _("Value is too small."));
	else if (c > sp->maxval)
		illegal_option(str, opts, index, _("Value is too large."));
	if (sp->is_power_2 && !ispow2(c))
		illegal_option(str, opts, index, _("Value must be a power of 2."));
	return c;
}

/*
 * Option is a string - do all the option table work, and check there
 * is actually an option string. Otherwise we don't do anything with the string
 * here - validation will be done later when the string is converted to a value
 * or used as a file/device path.
 */
static char *
getstr(
	const char		*str,
	struct opt_params	*opts,
	int			index)
{
	char			*ret;

	check_opt(opts, index, true);

	/* empty strings for string options are not valid */
	if (!str || *str == '\0')
		reqval(opts->name, opts->subopts, index);

	ret = strdup(str);
	if (!ret) {
		fprintf(stderr, _("Out of memory while saving suboptions.\n"));
		exit(1);
	}

	return ret;
}

static int
block_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case B_SIZE:
		cli->blocksize = getnum(value, opts, subopt);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
cfgfile_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case C_OPTFILE:
		cli->cfgfile = getstr(value, opts, subopt);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
set_data_concurrency(
	struct opt_params	*opts,
	int			subopt,
	struct cli_params	*cli,
	const char		*value)
{
	long long		optnum;

	/*
	 * "nr_cpus" or "1" means set the concurrency level to the CPU count.
	 * If this cannot be determined, fall back to the default AG geometry.
	 */
	if (!value || !strcmp(value, "nr_cpus"))
		optnum = 1;
	else
		optnum = getnum(value, opts, subopt);

	if (optnum == 1)
		cli->data_concurrency = nr_cpus();
	else
		cli->data_concurrency = optnum;
}

static int
data_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case D_AGCOUNT:
		cli->agcount = getnum(value, opts, subopt);
		break;
	case D_AGSIZE:
		cli->agsize = getstr(value, opts, subopt);
		break;
	case D_FILE:
		cli->xi->data.isfile = getnum(value, opts, subopt);
		break;
	case D_NAME:
		cli->xi->data.name = getstr(value, opts, subopt);
		break;
	case D_SIZE:
		cli->dsize = getstr(value, opts, subopt);
		break;
	case D_SUNIT:
		cli->dsunit = getnum(value, opts, subopt);
		break;
	case D_SWIDTH:
		cli->dswidth = getnum(value, opts, subopt);
		break;
	case D_SU:
		cli->dsu = getstr(value, opts, subopt);
		break;
	case D_SW:
		cli->dsw = getnum(value, opts, subopt);
		break;
	case D_NOALIGN:
		cli->sb_feat.nodalign = getnum(value, opts, subopt);
		break;
	case D_SECTSIZE:
		cli->sectorsize = getnum(value, opts, subopt);
		break;
	case D_RTINHERIT:
		if (getnum(value, opts, subopt))
			cli->fsx.fsx_xflags |= FS_XFLAG_RTINHERIT;
		else
			cli->fsx.fsx_xflags &= ~FS_XFLAG_RTINHERIT;
		break;
	case D_PROJINHERIT:
		cli->fsx.fsx_projid = getnum(value, opts, subopt);
		cli->fsx.fsx_xflags |= FS_XFLAG_PROJINHERIT;
		break;
	case D_EXTSZINHERIT:
		cli->fsx.fsx_extsize = getnum(value, opts, subopt);
		if (cli->fsx.fsx_extsize)
			cli->fsx.fsx_xflags |= FS_XFLAG_EXTSZINHERIT;
		else
			cli->fsx.fsx_xflags &= ~FS_XFLAG_EXTSZINHERIT;
		break;
	case D_COWEXTSIZE:
		cli->fsx.fsx_cowextsize = getnum(value, opts, subopt);
		if (cli->fsx.fsx_cowextsize)
			cli->fsx.fsx_xflags |= FS_XFLAG_COWEXTSIZE;
		else
			cli->fsx.fsx_xflags &= ~FS_XFLAG_COWEXTSIZE;
		break;
	case D_DAXINHERIT:
		if (getnum(value, opts, subopt))
			cli->fsx.fsx_xflags |= FS_XFLAG_DAX;
		else
			cli->fsx.fsx_xflags &= ~FS_XFLAG_DAX;
		break;
	case D_CONCURRENCY:
		set_data_concurrency(opts, subopt, cli, value);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
inode_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case I_ALIGN:
		cli->sb_feat.inode_align = getnum(value, opts, subopt);
		break;
	case I_MAXPCT:
		cli->imaxpct = getnum(value, opts, subopt);
		break;
	case I_PERBLOCK:
		cli->inopblock = getnum(value, opts, subopt);
		break;
	case I_SIZE:
		cli->inodesize = getnum(value, opts, subopt);
		break;
	case I_ATTR:
		cli->sb_feat.attr_version = getnum(value, opts, subopt);
		break;
	case I_PROJID32BIT:
		cli->sb_feat.projid32bit = getnum(value, opts, subopt);
		break;
	case I_SPINODES:
		cli->sb_feat.spinodes = getnum(value, opts, subopt);
		break;
	case I_NREXT64:
		cli->sb_feat.nrext64 = getnum(value, opts, subopt);
		break;
	case I_EXCHANGE:
		cli->sb_feat.exchrange = getnum(value, opts, subopt);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
set_log_concurrency(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	long long		optnum;

	/*
	 * "nr_cpus" or 1 means set the concurrency level to the CPU count.  If
	 * this cannot be determined, fall back to the default computation.
	 */
	if (!value || !strcmp(value, "nr_cpus"))
		optnum = 1;
	else
		optnum = getnum(value, opts, subopt);

	if (optnum == 1)
		cli->log_concurrency = nr_cpus();
	else
		cli->log_concurrency = optnum;
}

static int
log_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case L_AGNUM:
		cli->logagno = getnum(value, opts, subopt);
		break;
	case L_FILE:
		cli->xi->log.isfile = getnum(value, opts, subopt);
		break;
	case L_INTERNAL:
		cli->loginternal = getnum(value, opts, subopt);
		break;
	case L_SU:
		cli->lsu = getstr(value, opts, subopt);
		break;
	case L_SUNIT:
		cli->lsunit = getnum(value, opts, subopt);
		break;
	case L_NAME:
	case L_DEV:
		cli->xi->log.name = getstr(value, opts, subopt);
		cli->loginternal = 0;
		break;
	case L_VERSION:
		cli->sb_feat.log_version = getnum(value, opts, subopt);
		break;
	case L_SIZE:
		cli->logsize = getstr(value, opts, subopt);
		break;
	case L_SECTSIZE:
		cli->lsectorsize = getnum(value, opts, subopt);
		break;
	case L_LAZYSBCNTR:
		cli->sb_feat.lazy_sb_counters = getnum(value, opts, subopt);
		break;
	case L_CONCURRENCY:
		set_log_concurrency(opts, subopt, value, cli);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
meta_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case M_CRC:
		cli->sb_feat.crcs_enabled = getnum(value, opts, subopt);
		if (cli->sb_feat.crcs_enabled)
			cli->sb_feat.dirftype = true;
		break;
	case M_FINOBT:
		cli->sb_feat.finobt = getnum(value, opts, subopt);
		break;
	case M_UUID:
		if (!value || *value == '\0')
			reqval('m', opts->subopts, subopt);
		if (platform_uuid_parse(value, &cli->uuid))
			illegal(value, "m uuid");
		break;
	case M_RMAPBT:
		cli->sb_feat.rmapbt = getnum(value, opts, subopt);
		break;
	case M_REFLINK:
		cli->sb_feat.reflink = getnum(value, opts, subopt);
		break;
	case M_INOBTCNT:
		cli->sb_feat.inobtcnt = getnum(value, opts, subopt);
		break;
	case M_BIGTIME:
		cli->sb_feat.bigtime = getnum(value, opts, subopt);
		break;
	case M_AUTOFSCK:
		if (!value || value[0] == 0 || isdigit(value[0])) {
			long long	ival = getnum(value, opts, subopt);

			if (ival)
				cli->autofsck = FSPROP_AUTOFSCK_REPAIR;
			else
				cli->autofsck = FSPROP_AUTOFSCK_NONE;
		} else {
			cli->autofsck = fsprop_autofsck_read(value);
			if (cli->autofsck == FSPROP_AUTOFSCK_UNSET)
				illegal(value, "m autofsck");
		}
		break;
	case M_METADIR:
		cli->sb_feat.metadir = getnum(value, opts, subopt);
		break;
	case M_UQUOTA:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_UQUOTA_ACCT | XFS_UQUOTA_ENFD;
		break;
	case M_GQUOTA:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_GQUOTA_ACCT | XFS_GQUOTA_ENFD;
		break;
	case M_PQUOTA:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_PQUOTA_ACCT | XFS_PQUOTA_ENFD;
		break;
	case M_UQNOENFORCE:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_UQUOTA_ACCT;
		break;
	case M_GQNOENFORCE:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_GQUOTA_ACCT;
		break;
	case M_PQNOENFORCE:
		if (getnum(value, opts, subopt))
			cli->sb_feat.qflags |= XFS_PQUOTA_ACCT;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
naming_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case N_SIZE:
		cli->dirblocksize = getstr(value, opts, subopt);
		break;
	case N_VERSION:
		value = getstr(value, &nopts, subopt);
		if (!strcasecmp(value, "ci")) {
			/* ASCII CI mode */
			cli->sb_feat.nci = true;
		} else {
			cli->sb_feat.dir_version = getnum(value, opts, subopt);
		}
		free((char *)value);
		break;
	case N_FTYPE:
		cli->sb_feat.dirftype = getnum(value, opts, subopt);
		break;
	case N_PARENT:
		cli->sb_feat.parent_pointers = getnum(value, &nopts, N_PARENT);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
proto_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case P_SLASHES:
		cli->proto_slashes_are_spaces = getnum(value, opts, subopt);
		break;
	case P_FILE:
		fallthrough;
	default:
		if (cli->protofile) {
			if (subopt < 0)
				subopt = P_FILE;
			respec(opts->name, opts->subopts, subopt);
		}
		cli->protofile = strdup(value);
		if (!cli->protofile) {
			fprintf(stderr,
 _("Out of memory while saving protofile option.\n"));
			exit(1);
		}
		break;
	}
	return 0;
}

static void
set_rtvol_concurrency(
	struct opt_params	*opts,
	int			subopt,
	struct cli_params	*cli,
	const char		*value)
{
	long long		optnum;

	/*
	 * "nr_cpus" or "1" means set the concurrency level to the CPU count.
	 * If this cannot be determined, fall back to the default rtgroup
	 * geometry.
	 */
	if (!value || !strcmp(value, "nr_cpus"))
		optnum = 1;
	else
		optnum = getnum(value, opts, subopt);

	if (optnum == 1)
		cli->rtvol_concurrency = nr_cpus();
	else
		cli->rtvol_concurrency = optnum;
}

static int
rtdev_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case R_EXTSIZE:
		cli->rtextsize = getstr(value, opts, subopt);
		break;
	case R_FILE:
		cli->xi->rt.isfile = getnum(value, opts, subopt);
		break;
	case R_NAME:
	case R_DEV:
		cli->xi->rt.name = getstr(value, opts, subopt);
		break;
	case R_SIZE:
		cli->rtsize = getstr(value, opts, subopt);
		break;
	case R_NOALIGN:
		cli->sb_feat.nortalign = getnum(value, opts, subopt);
		break;
	case R_RGCOUNT:
		cli->rgcount = getnum(value, opts, subopt);
		break;
	case R_RGSIZE:
		cli->rgsize = getstr(value, opts, subopt);
		break;
	case R_CONCURRENCY:
		set_rtvol_concurrency(opts, subopt, cli, value);
		break;
	case R_ZONED:
		cli->sb_feat.zoned = getnum(value, opts, subopt);
		break;
	case R_START:
		cli->rtstart = getstr(value, opts, subopt);
		break;
	case R_RESERVED:
		cli->rtreserved = getnum(value, opts, subopt);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int
sector_opts_parser(
	struct opt_params	*opts,
	int			subopt,
	const char		*value,
	struct cli_params	*cli)
{
	switch (subopt) {
	case S_SIZE:
	case S_SECTSIZE:
		cli->sectorsize = getnum(value, opts, subopt);
		cli->lsectorsize = cli->sectorsize;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static struct subopts {
	struct opt_params *opts;
	int		(*parser)(struct opt_params	*opts,
				  int			subopt,
				  const char		*value,
				  struct cli_params	*cli);
} subopt_tab[] = {
	{ &bopts, block_opts_parser },
	{ &copts, cfgfile_opts_parser },
	{ &dopts, data_opts_parser },
	{ &iopts, inode_opts_parser },
	{ &lopts, log_opts_parser },
	{ &mopts, meta_opts_parser },
	{ &nopts, naming_opts_parser },
	{ &popts, proto_opts_parser },
	{ &ropts, rtdev_opts_parser },
	{ &sopts, sector_opts_parser },
	{ NULL, NULL },
};

static void
parse_subopts(
	char		opt,
	char		*arg,
	struct cli_params *cli)
{
	struct subopts	*sop = &subopt_tab[0];
	char		*p;
	int		ret = 0;

	while (sop->opts) {
		if (sop->opts->name == opt)
			break;
		sop++;
	}

	/* should never happen */
	if (!sop->opts)
		return;

	p = arg;
	while (*p != '\0') {
		char	**subopts = (char **)sop->opts->subopts;
		char	*value;
		int	subopt;

		subopt = getsubopt(&p, subopts, &value);

		ret = (sop->parser)(sop->opts, subopt, value, cli);
		if (ret)
			unknown(opt, value);
	}
}

static bool
parse_cfgopt(
	const char	*section,
	const char	*name,
	const char	*value,
	struct cli_params *cli)
{
	struct subopts	*sop = &subopt_tab[0];
	char		**subopts;
	int		ret = 0;
	int		i;

	while (sop->opts) {
		if (sop->opts->ini_section[0] != '\0' &&
		    strcasecmp(section, sop->opts->ini_section) == 0)
			break;
		sop++;
	}

	/* Config files with unknown sections get caught here. */
	if (!sop->opts)
		goto invalid_opt;

	subopts = (char **)sop->opts->subopts;
	for (i = 0; i < MAX_SUBOPTS; i++) {
		if (!subopts[i])
			break;
		if (strcasecmp(name, subopts[i]) == 0) {
			ret = (sop->parser)(sop->opts, i, value, cli);
			if (ret)
				goto invalid_opt;
			return true;
		}
	}
invalid_opt:
	invalid_cfgfile_opt(cli->cfgfile, section, name, value);
	return false;
}

static void
validate_sectorsize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct mkfs_default_params *dft,
	struct fs_topology	*ft,
	int			dry_run,
	int			force_overwrite)
{
	/*
	 * Before anything else, verify that we are correctly operating on
	 * files or block devices and set the control parameters correctly.
	 */
	check_device_type(&cli->xi->data, !cli->dsize, dry_run, "d");
	if (!cli->loginternal)
		check_device_type(&cli->xi->log, !cli->logsize, dry_run, "l");
	if (cli->xi->rt.name)
		check_device_type(&cli->xi->rt, !cli->rtsize, dry_run, "r");

	/*
	 * Explicitly disable direct IO for image files so we don't error out on
	 * sector size mismatches between the new filesystem and the underlying
	 * host filesystem.
	 */
	if (cli->xi->data.isfile || cli->xi->log.isfile || cli->xi->rt.isfile)
		cli->xi->flags &= ~LIBXFS_DIRECT;

	memset(ft, 0, sizeof(*ft));
	get_topology(cli->xi, ft, force_overwrite);

	/* set configured sector sizes in preparation for checks */
	if (!cli->sectorsize) {
		/*
		 * Unless specified manually on the command line use the
		 * advertised sector size of the device.  We use the physical
		 * sector size unless the requested block size is smaller
		 * than that, then we can use logical, but warn about the
		 * inefficiency.  If the file system has a RT device, the
		 * sectorsize needs to be the maximum of the data and RT
		 * device.
		 *
		 * Some architectures have a page size > XFS_MAX_SECTORSIZE.
		 * In that case, a ramdisk or persistent memory device may
		 * advertise a physical sector size that is too big to use.
		 */
		if (ft->data.physical_sector_size > XFS_MAX_SECTORSIZE) {
			ft->data.physical_sector_size =
				ft->data.logical_sector_size;
		}
		cfg->sectorsize = ft->data.physical_sector_size;

		if (cli->xi->rt.name) {
			if (ft->rt.physical_sector_size > XFS_MAX_SECTORSIZE) {
				ft->rt.physical_sector_size =
					ft->rt.logical_sector_size;
			}

			if (cfg->sectorsize < ft->rt.physical_sector_size)
				cfg->sectorsize = ft->rt.physical_sector_size;
		}

		if (cfg->blocksize < cfg->sectorsize &&
		    cfg->blocksize >= ft->data.logical_sector_size) {
			fprintf(stderr,
_("specified blocksize %d is less than device physical sector size %d\n"
  "switching to logical sector size %d\n"),
				cfg->blocksize, ft->data.physical_sector_size,
				ft->data.logical_sector_size);
			cfg->sectorsize = ft->data.logical_sector_size;
		}
	} else
		cfg->sectorsize = cli->sectorsize;

	cfg->sectorlog = libxfs_highbit32(cfg->sectorsize);

	/* validate specified/probed sector size */
	if (cfg->sectorsize < XFS_MIN_SECTORSIZE ||
	    cfg->sectorsize > XFS_MAX_SECTORSIZE) {
		fprintf(stderr, _("illegal sector size %d\n"), cfg->sectorsize);
		usage();
	}

	if (cfg->blocksize < cfg->sectorsize) {
		fprintf(stderr,
_("block size %d cannot be smaller than sector size %d\n"),
			cfg->blocksize, cfg->sectorsize);
		usage();
	}

	if (cfg->sectorsize < ft->data.logical_sector_size) {
		fprintf(stderr, _("illegal sector size %d; hw sector is %d\n"),
			cfg->sectorsize, ft->data.logical_sector_size);
		usage();
	}
}

static void
validate_blocksize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct mkfs_default_params *dft)
{
	/*
	 * Blocksize and sectorsize first, other things depend on them
	 * For RAID4/5/6 we want to align sector size and block size,
	 * so we need to start with the device geometry extraction too.
	 */
	if (!cli->blocksize)
		cfg->blocksize = dft->blocksize;
	else
		cfg->blocksize = cli->blocksize;
	cfg->blocklog = libxfs_highbit32(cfg->blocksize);

	/* validate block sizes are in range */
	if (cfg->blocksize < XFS_MIN_BLOCKSIZE ||
	    cfg->blocksize > XFS_MAX_BLOCKSIZE) {
		fprintf(stderr, _("illegal block size %d\n"), cfg->blocksize);
		usage();
	}

	if (cli->sb_feat.crcs_enabled &&
	    cfg->blocksize < XFS_MIN_CRC_BLOCKSIZE) {
		fprintf(stderr,
_("Minimum block size for CRC enabled filesystems is %d bytes.\n"),
			XFS_MIN_CRC_BLOCKSIZE);
		usage();
	}

}

/*
 * Grab log sector size and validate.
 *
 * XXX: should we probe sector size on external log device rather than using
 * the data device sector size?
 */
static void
validate_log_sectorsize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct mkfs_default_params *dft,
	struct fs_topology	*ft)
{

	if (cli->loginternal && cli->lsectorsize &&
	    cli->lsectorsize != cfg->sectorsize) {
		fprintf(stderr,
_("Can't change sector size on internal log!\n"));
		usage();
	}

	if (cli->lsectorsize)
		cfg->lsectorsize = cli->lsectorsize;
	else if (cli->loginternal)
		cfg->lsectorsize = cfg->sectorsize;
	else
		cfg->lsectorsize = ft->log.logical_sector_size;
	cfg->lsectorlog = libxfs_highbit32(cfg->lsectorsize);

	if (cfg->lsectorsize < XFS_MIN_SECTORSIZE ||
	    cfg->lsectorsize > XFS_MAX_SECTORSIZE ||
	    cfg->lsectorsize > cfg->blocksize) {
		fprintf(stderr, _("illegal log sector size %d\n"),
			cfg->lsectorsize);
		usage();
	}
	if (cfg->lsectorsize > XFS_MIN_SECTORSIZE) {
		if (cli->sb_feat.log_version < 2) {
			/* user specified non-default log version */
			fprintf(stderr,
_("Version 1 logs do not support sector size %d\n"),
				cfg->lsectorsize);
			usage();
		}
	}

	/* if lsu or lsunit was specified, automatically use v2 logs */
	if ((cli_opt_set(&lopts, L_SU) || cli_opt_set(&lopts, L_SUNIT)) &&
	    cli->sb_feat.log_version == 1) {
		fprintf(stderr,
_("log stripe unit specified, using v2 logs\n"));
		cli->sb_feat.log_version = 2;
	}
}

struct zone_info {
	/* number of zones, conventional or sequential */
	unsigned int		nr_zones;
	/* number of conventional zones */
	unsigned int		nr_conv_zones;

	/* size of the address space for a zone, in 512b blocks */
	xfs_daddr_t		zone_size;
	/* write capacity of a zone, in 512b blocks */
	xfs_daddr_t		zone_capacity;
};

struct zone_topology {
	struct zone_info	data;
	struct zone_info	rt;
	struct zone_info	log;
};

/* random size that allows efficient processing */
#define ZONES_PER_IOCTL			16384

static void
report_zones(
	const char		*name,
	struct zone_info	*zi)
{
	struct blk_zone_report	*rep;
	bool			found_seq = false;
	int			fd, ret = 0;
	uint64_t		device_size;
	uint64_t		sector = 0;
	size_t			rep_size;
	unsigned int		i, n = 0;
	struct stat		st;

	fd = open(name, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, _("Failed to open RT device: %d.\n"), -errno);
		exit(1);
	}

	if (fstat(fd, &st) < 0) {
		ret = -EIO;
		goto out_close;
	}
	if (!S_ISBLK(st.st_mode))
		goto out_close;

	if (ioctl(fd, BLKGETSIZE64, &device_size)) {
		fprintf(stderr, _("Failed to get block size: %d.\n"), -errno);
		exit(1);
	}
	if (ioctl(fd, BLKGETZONESZ, &zi->zone_size) || !zi->zone_size)
		goto out_close; /* not zoned */

	/* BLKGETSIZE64 reports a byte value */
	device_size = BTOBB(device_size);
	zi->nr_zones = device_size / zi->zone_size;
	zi->nr_conv_zones = 0;

	rep_size = sizeof(struct blk_zone_report) +
		   sizeof(struct blk_zone) * ZONES_PER_IOCTL;
	rep = malloc(rep_size);
	if (!rep) {
		fprintf(stderr,
_("Failed to allocate memory for zone reporting.\n"));
		exit(1);
	}

	while (n < zi->nr_zones) {
		struct blk_zone *zones = (struct blk_zone *)(rep + 1);

		memset(rep, 0, rep_size);
		rep->sector = sector;
		rep->nr_zones = ZONES_PER_IOCTL;

		ret = ioctl(fd, BLKREPORTZONE, rep);
		if (ret) {
			fprintf(stderr,
_("ioctl(BLKREPORTZONE) failed: %d!\n"), -errno);
			exit(1);
		}
		if (!rep->nr_zones)
			break;

		for (i = 0; i < rep->nr_zones; i++) {
			if (n >= zi->nr_zones)
				break;

			if (zones[i].len != zi->zone_size) {
				fprintf(stderr,
_("Inconsistent zone size!\n"));
				exit(1);
			}

			switch (zones[i].type) {
			case BLK_ZONE_TYPE_CONVENTIONAL:
				/*
				 * We can only use the conventional space at the
				 * start of the device for metadata, so don't
				 * count later conventional zones.  This is
				 * not an error because we can use them for data
				 * just fine.
				 */
				if (!found_seq)
					zi->nr_conv_zones++;
				break;
			case BLK_ZONE_TYPE_SEQWRITE_REQ:
				found_seq = true;
				break;
			case BLK_ZONE_TYPE_SEQWRITE_PREF:
				fprintf(stderr,
_("Sequential write preferred zones not supported.\n"));
				exit(1);
			default:
				fprintf(stderr,
_("Unknown zone type (0x%x) found.\n"), zones[i].type);
				exit(1);
			}

			if (!n) {
				zi->zone_capacity = zones[i].capacity;
				if (zi->zone_capacity > zi->zone_size) {
					fprintf(stderr,
_("Zone capacity larger than zone size!\n"));
					exit(1);
				}
			} else if (zones[i].capacity != zi->zone_capacity) {
				fprintf(stderr,
_("Inconsistent zone capacity!\n"));
				exit(1);
			}

			n++;
		}
		sector = zones[rep->nr_zones - 1].start +
			 zones[rep->nr_zones - 1].len;
	}

	free(rep);
out_close:
	close(fd);
}

static void
validate_zoned(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct mkfs_default_params *dft,
	struct zone_topology	*zt)
{
	if (!cli->xi->data.isfile) {
		report_zones(cli->xi->data.name, &zt->data);
		if (zt->data.nr_zones) {
			if (!zt->data.nr_conv_zones) {
				fprintf(stderr,
_("Data devices requires conventional zones.\n"));
				usage();
			}
			if (zt->data.zone_capacity != zt->data.zone_size) {
				fprintf(stderr,
_("Zone capacity equal to Zone size required for conventional zones.\n"));
				usage();
			}

			cli->sb_feat.zoned = true;
			cfg->rtstart =
				zt->data.nr_conv_zones * zt->data.zone_capacity;
		}
	}

	if (cli->xi->rt.name && !cli->xi->rt.isfile) {
		report_zones(cli->xi->rt.name, &zt->rt);
		if (zt->rt.nr_zones && !cli->sb_feat.zoned)
			cli->sb_feat.zoned = true;
		if (zt->rt.zone_size != zt->rt.zone_capacity)
			cli->sb_feat.zone_gaps = true;
	}

	if (cli->xi->log.name && !cli->xi->log.isfile) {
		report_zones(cli->xi->log.name, &zt->log);
		if (zt->log.nr_zones) {
			fprintf(stderr,
_("Zoned devices not supported as log device!\n"));
			usage();
		}
	}

	if (cli->rtstart) {
		/*
		 * For zoned devices with conventional zones, cfg->rtstart is
		 * set to the start of the first sequential write required zoned
		 * above.  Don't allow the user to override it as that won't
		 * work.
		 */
		if (cfg->rtstart) {
			fprintf(stderr,
_("rtstart override not allowed on zoned devices.\n"));
			usage();
		}
		cfg->rtstart = getnum(cli->rtstart, &ropts, R_START) / 512;
	}

	if (cli->rtreserved)
		cfg->rtreserved = cli->rtreserved;
}

/*
 * Check that the incoming features make sense. The CLI structure was
 * initialised with the default values before parsing, so we can just
 * check it and copy it straight across to the cfg structure if it
 * checks out.
 */
static void
validate_sb_features(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{
	if (cli->sb_feat.nci) {
		/*
		 * The ascii-ci feature is deprecated in the upstream Linux
		 * kernel.  In September 2025 it will be turned off by default
		 * in the kernel and in September 2030 support will be removed
		 * entirely.
		 */
		fprintf(stdout,
_("ascii-ci filesystems are deprecated and will not be supported by future versions.\n"));
	}

	/*
	 * Now we have blocks and sector sizes set up, check parameters that are
	 * no longer optional for CRC enabled filesystems.  Catch them up front
	 * here before doing anything else.
	 */
	if (cli->sb_feat.crcs_enabled) {
		/* minimum inode size is 512 bytes, rest checked later */
		if (cli->inodesize &&
		    cli->inodesize < (1 << XFS_DINODE_DFL_CRC_LOG)) {
			fprintf(stderr,
_("Minimum inode size for CRCs is %d bytes\n"),
				1 << XFS_DINODE_DFL_CRC_LOG);
			usage();
		}

		/* inodes always aligned */
		if (!cli->sb_feat.inode_align) {
			fprintf(stderr,
_("Inodes always aligned for CRC enabled filesystems\n"));
			usage();
		}

		/* lazy sb counters always on */
		if (!cli->sb_feat.lazy_sb_counters) {
			fprintf(stderr,
_("Lazy superblock counters always enabled for CRC enabled filesystems\n"));
			usage();
		}

		/* version 2 logs always on */
		if (cli->sb_feat.log_version != 2) {
			fprintf(stderr,
_("V2 logs always enabled for CRC enabled filesystems\n"));
			usage();
		}

		/* attr2 always on */
		if (cli->sb_feat.attr_version != 2) {
			fprintf(stderr,
_("V2 attribute format always enabled on CRC enabled filesystems\n"));
			usage();
		}

		/* 32 bit project quota always on */
		/* attr2 always on */
		if (!cli->sb_feat.projid32bit) {
			fprintf(stderr,
_("32 bit Project IDs always enabled on CRC enabled filesystems\n"));
			usage();
		}

		/* ftype always on */
		if (!cli->sb_feat.dirftype) {
			fprintf(stderr,
_("Directory ftype field always enabled on CRC enabled filesystems\n"));
			usage();
		}

		/*
		 * Self-healing through online fsck relies heavily on back
		 * reference metadata, so we really want to try to enable rmap
		 * and parent pointers.
		 */
		if (cli->autofsck >= FSPROP_AUTOFSCK_CHECK) {
			if (!cli->sb_feat.rmapbt) {
				if (cli_opt_set(&mopts, M_RMAPBT)) {
					fprintf(stdout,
_("-m autofsck=%s is less effective without reverse mapping\n"),
						fsprop_autofsck_write(cli->autofsck));
				} else {
					cli->sb_feat.rmapbt = true;
				}
			}
			if (!cli->sb_feat.parent_pointers) {
				if (cli_opt_set(&nopts, N_PARENT)) {
					fprintf(stdout,
_("-m autofsck=%s is less effective without parent pointers\n"),
						fsprop_autofsck_write(cli->autofsck));
				} else {
					cli->sb_feat.parent_pointers = true;
				}
			}
		}

	} else {	/* !crcs_enabled */
		/*
		 * The V4 filesystem format is deprecated in the upstream Linux
		 * kernel.  In September 2025 it will be turned off by default
		 * in the kernel and in September 2030 support will be removed
		 * entirely.
		 */
		fprintf(stdout,
_("V4 filesystems are deprecated and will not be supported by future versions.\n"));

		/*
		 * The kernel doesn't support crc=0,finobt=1 filesystems.
		 * If crcs are not enabled and the user has not explicitly
		 * turned finobt on, then silently turn it off to avoid an
		 * unnecessary warning.
		 * If the user explicitly tried to use crc=0,finobt=1,
		 * then issue an error.
		 * The same is also true for sparse inodes and reflink.
		 */
		if (cli->sb_feat.finobt && cli_opt_set(&mopts, M_FINOBT)) {
			fprintf(stderr,
_("finobt not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.finobt = false;

		if (cli->sb_feat.spinodes && cli_opt_set(&iopts, I_SPINODES)) {
			fprintf(stderr,
_("sparse inodes not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.spinodes = false;

		if (cli->sb_feat.rmapbt && cli_opt_set(&mopts, M_RMAPBT)) {
			fprintf(stderr,
_("rmapbt not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.rmapbt = false;

		if (cli->sb_feat.reflink && cli_opt_set(&mopts, M_REFLINK)) {
			fprintf(stderr,
_("reflink not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.reflink = false;

		if (cli->sb_feat.inobtcnt && cli_opt_set(&mopts, M_INOBTCNT)) {
			fprintf(stderr,
_("inode btree counters not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.inobtcnt = false;

		if (cli->sb_feat.bigtime && cli_opt_set(&mopts, M_BIGTIME)) {
			fprintf(stderr,
_("timestamps later than 2038 not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.bigtime = false;

		if (cli->sb_feat.nrext64 &&
		    cli_opt_set(&iopts, I_NREXT64)) {
			fprintf(stderr,
_("64 bit extent count not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.nrext64 = false;

		if (cli->sb_feat.exchrange && cli_opt_set(&iopts, I_EXCHANGE)) {
			fprintf(stderr,
_("exchange-range not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.exchrange = false;

		if (cli->sb_feat.parent_pointers &&
		    cli_opt_set(&nopts, N_PARENT)) {
			fprintf(stderr,
_("parent pointers not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.parent_pointers = false;

		if (cli->autofsck != FSPROP_AUTOFSCK_UNSET &&
		    cli_opt_set(&mopts, M_AUTOFSCK)) {
			fprintf(stderr,
_("autofsck not supported without CRC support\n"));
			usage();
		}
		cli->autofsck = FSPROP_AUTOFSCK_UNSET;

		if (cli->sb_feat.metadir &&
		    cli_opt_set(&mopts, M_METADIR)) {
			fprintf(stderr,
_("metadata directory not supported without CRC support\n"));
			usage();
		}
		cli->sb_feat.metadir = false;

		if (cli->sb_feat.qflags) {
			fprintf(stderr,
_("persistent quota flags not supported without CRC support\n"));
			usage();
		}
	}

	if (!cli->sb_feat.finobt) {
		if (cli->sb_feat.inobtcnt && cli_opt_set(&mopts, M_INOBTCNT)) {
			fprintf(stderr,
_("inode btree counters not supported without finobt support\n"));
			usage();
		}
		cli->sb_feat.inobtcnt = false;
	}

	if (cli->sb_feat.zoned) {
		if (!cli->sb_feat.metadir) {
			if (cli_opt_set(&mopts, M_METADIR)) {
				fprintf(stderr,
_("zoned realtime device not supported without metadir support\n"));
				usage();
			}
			cli->sb_feat.metadir = true;
		}
		if (cli->rtextsize) {
			if (cli_opt_set(&ropts, R_EXTSIZE)) {
				fprintf(stderr,
_("rt extent size not supported on realtime devices with zoned mode\n"));
				usage();
			}
			cli->rtextsize = 0;
		}
		if (cli->sb_feat.reflink) {
			if (cli_opt_set(&mopts, M_REFLINK)) {
				fprintf(stderr,
_("reflink not supported on realtime devices with zoned mode specified\n"));
				usage();
			}
			cli->sb_feat.reflink = false;
		}

		/*
		 * Set the rtinherit by default for zoned file systems as they
		 * usually use the data device purely as a metadata container.
		 */
		if (!cli_opt_set(&dopts, D_RTINHERIT))
			cli->fsx.fsx_xflags |= FS_XFLAG_RTINHERIT;
	} else {
		if (cli->rtstart) {
			fprintf(stderr,
_("internal RT section only supported in zoned mode\n"));
			usage();
		}
		if (cli->rtreserved) {
			fprintf(stderr,
_("reserved RT blocks only supported in zoned mode\n"));
			usage();
		}
	}

	if (cli->xi->rt.name || cfg->rtstart) {
		if (cli->rtextsize && cli->sb_feat.reflink) {
			if (cli_opt_set(&mopts, M_REFLINK)) {
				fprintf(stderr,
_("reflink not supported on realtime devices with rt extent size specified\n"));
				usage();
			}
			cli->sb_feat.reflink = false;
		}
		if (cfg->blocksize < XFS_MIN_RTEXTSIZE && cli->sb_feat.reflink) {
			if (cli_opt_set(&mopts, M_REFLINK)) {
				fprintf(stderr,
_("reflink not supported on realtime devices with blocksize %d < %d\n"),
						cli->blocksize,
						XFS_MIN_RTEXTSIZE);
				usage();
			}
			cli->sb_feat.reflink = false;
		}
		if (!cli->sb_feat.metadir && cli->sb_feat.reflink) {
			if (cli_opt_set(&mopts, M_REFLINK) &&
			    cli_opt_set(&mopts, M_METADIR)) {
				fprintf(stderr,
_("reflink not supported on realtime devices without metadir feature\n"));
				usage();
			} else if (cli_opt_set(&mopts, M_REFLINK)) {
				cli->sb_feat.metadir = true;
			} else {
				cli->sb_feat.reflink = false;
			}
		}

		if (!cli->sb_feat.metadir && cli->sb_feat.rmapbt) {
			if (cli_opt_set(&mopts, M_RMAPBT) &&
			    cli_opt_set(&mopts, M_METADIR)) {
				fprintf(stderr,
_("rmapbt not supported on realtime devices without metadir feature\n"));
				usage();
			} else if (cli_opt_set(&mopts, M_RMAPBT)) {
				cli->sb_feat.metadir = true;
			} else {
				cli->sb_feat.rmapbt = false;
			}
		}
	}

	if ((cli->fsx.fsx_xflags & FS_XFLAG_COWEXTSIZE) &&
	    !cli->sb_feat.reflink) {
		fprintf(stderr,
_("cowextsize not supported without reflink support\n"));
		usage();
	}

	/*
	 * Turn on exchange-range if parent pointers are enabled and the caller
	 * did not provide an explicit exchange-range parameter so that users
	 * can take advantage of online repair.  It's not required for correct
	 * operation, but it costs us nothing to enable it.
	 */
	if (cli->sb_feat.parent_pointers && !cli->sb_feat.exchrange &&
	    !cli_opt_set(&iopts, I_EXCHANGE)) {
		cli->sb_feat.exchrange = true;
	}

	/*
	 * Persistent quota flags requires metadir support because older
	 * kernels (or current kernels with old filesystems) will reset qflags
	 * in the absence of any quota mount options.
	 */
	if (cli->sb_feat.qflags && !cli->sb_feat.metadir) {
		if (cli_opt_set(&mopts, M_METADIR)) {
			fprintf(stderr,
_("persistent quota flags not supported without metadir support\n"));
			usage();
		}
		cli->sb_feat.metadir = true;
	}

	/*
	 * Exchange-range will be needed for space reorganization on filesystems
	 * with realtime rmap or realtime reflink enabled, and there is no good
	 * reason to ever disable it on a file system with new enough features.
	 */
	if (cli->sb_feat.metadir && !cli->sb_feat.exchrange) {
		if (cli_opt_set(&iopts, I_EXCHANGE)) {
			fprintf(stderr,
_("metadir not supported without exchange-range support\n"));
			usage();
		}
		cli->sb_feat.exchrange = true;
	}

	/*
	 * Copy features across to config structure now.
	 */
	cfg->sb_feat = cli->sb_feat;
	if (!platform_uuid_is_null(&cli->uuid))
		platform_uuid_copy(&cfg->uuid, &cli->uuid);
}

static void
validate_dirblocksize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{

	if (cli->dirblocksize)
		cfg->dirblocksize = getnum(cli->dirblocksize, &nopts, N_SIZE);

	if (cfg->dirblocksize) {
		if (cfg->dirblocksize < cfg->blocksize ||
		    cfg->dirblocksize > XFS_MAX_BLOCKSIZE) {
			fprintf(stderr, _("illegal directory block size %d\n"),
				cfg->dirblocksize);
			usage();
		}
		cfg->dirblocklog = libxfs_highbit32(cfg->dirblocksize);
		return;
	}

	/* use default size based on current block size */
	if (cfg->blocksize < (1 << XFS_MIN_REC_DIRSIZE))
		cfg->dirblocklog = XFS_MIN_REC_DIRSIZE;
	else
		cfg->dirblocklog = cfg->blocklog;
	cfg->dirblocksize = 1 << cfg->dirblocklog;
}

static void
validate_inodesize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{

	if (cli->inopblock)
		cfg->inodelog = cfg->blocklog - libxfs_highbit32(cli->inopblock);
	else if (cli->inodesize)
		cfg->inodelog = libxfs_highbit32(cli->inodesize);
	else if (cfg->sb_feat.crcs_enabled)
		cfg->inodelog = XFS_DINODE_DFL_CRC_LOG;
	else
		cfg->inodelog = XFS_DINODE_DFL_LOG;

	cfg->inodesize = 1 << cfg->inodelog;
	cfg->inopblock = cfg->blocksize / cfg->inodesize;

	/* input parsing has already validated non-crc inode size range */
	if (cfg->sb_feat.crcs_enabled &&
	    cfg->inodelog < XFS_DINODE_DFL_CRC_LOG) {
		fprintf(stderr,
		_("Minimum inode size for CRCs is %d bytes\n"),
			1 << XFS_DINODE_DFL_CRC_LOG);
		usage();
	}

	if (cfg->inodesize > cfg->blocksize / XFS_MIN_INODE_PERBLOCK ||
	    cfg->inopblock < XFS_MIN_INODE_PERBLOCK ||
	    cfg->inodesize < XFS_DINODE_MIN_SIZE ||
	    cfg->inodesize > XFS_DINODE_MAX_SIZE) {
		int	maxsz;

		fprintf(stderr, _("illegal inode size %d\n"), cfg->inodesize);
		maxsz = min(cfg->blocksize / XFS_MIN_INODE_PERBLOCK,
			    XFS_DINODE_MAX_SIZE);
		if (XFS_DINODE_MIN_SIZE == maxsz)
			fprintf(stderr,
			_("allowable inode size with %d byte blocks is %d\n"),
				cfg->blocksize, XFS_DINODE_MIN_SIZE);
		else
			fprintf(stderr,
	_("allowable inode size with %d byte blocks is between %d and %d\n"),
				cfg->blocksize, XFS_DINODE_MIN_SIZE, maxsz);
		exit(1);
	}
}

static xfs_rfsblock_t
calc_dev_size(
	char			*size,
	struct mkfs_params	*cfg,
	struct opt_params	*opts,
	int			sizeopt,
	char			*type)
{
	uint64_t		dbytes;
	xfs_rfsblock_t		dblocks;

	if (!size)
		return 0;

	dbytes = getnum(size, opts, sizeopt);
	if (dbytes % XFS_MIN_BLOCKSIZE) {
		fprintf(stderr,
		_("illegal %s length %lld, not a multiple of %d\n"),
			type, (long long)dbytes, XFS_MIN_BLOCKSIZE);
		usage();
	}
	dblocks = (xfs_rfsblock_t)(dbytes >> cfg->blocklog);
	if (dbytes % cfg->blocksize) {
		fprintf(stderr,
_("warning: %s length %lld not a multiple of %d, truncated to %lld\n"),
			type, (long long)dbytes, cfg->blocksize,
			(long long)(dblocks << cfg->blocklog));
	}
	return dblocks;
}

static void
validate_rtextsize(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct fs_topology	*ft)
{
	uint64_t		rtextbytes;

	/*
	 * If specified, check rt extent size against its constraints.
	 */
	if (cli->rtextsize) {

		rtextbytes = getnum(cli->rtextsize, &ropts, R_EXTSIZE);
		if (rtextbytes % cfg->blocksize) {
			fprintf(stderr,
		_("illegal rt extent size %lld, not a multiple of %d\n"),
				(long long)rtextbytes, cfg->blocksize);
			usage();
		}
		cfg->rtextblocks = (xfs_extlen_t)(rtextbytes >> cfg->blocklog);
	} else if (cli->sb_feat.reflink && cli->xi->rt.name) {
		/*
		 * reflink doesn't support rt extent size > 1FSB yet, so set
		 * an extent size of 1FSB.  Make sure we still satisfy the
		 * minimum rt extent size.
		 */
		if (cfg->blocksize < XFS_MIN_RTEXTSIZE) {
			fprintf(stderr,
		_("reflink not supported on rt volume with blocksize %d\n"),
				cfg->blocksize);
			usage();
		}
		cfg->rtextblocks = 1;
	} else if (cli->sb_feat.zoned) {
		/*
		 * Zoned mode only supports a rtextsize of 1.
		 */
		cfg->rtextblocks = 1;
	} else {
		/*
		 * If realtime extsize has not been specified by the user,
		 * and the underlying volume is striped, then set rtextblocks
		 * to the stripe width.
		 */
		uint64_t	rswidth;

		if (!cfg->sb_feat.nortalign && !cli->xi->rt.isfile &&
		    !(!cli->rtsize && cli->xi->data.isfile))
			rswidth = ft->rt.swidth;
		else
			rswidth = 0;

		/* check that rswidth is a multiple of fs blocksize */
		if (!cfg->sb_feat.nortalign && rswidth &&
		    !(BBTOB(rswidth) % cfg->blocksize)) {
			rswidth = DTOBT(rswidth, cfg->blocklog);
			rtextbytes = rswidth << cfg->blocklog;
			if (rtextbytes > XFS_MIN_RTEXTSIZE &&
			    rtextbytes <= XFS_MAX_RTEXTSIZE) {
				cfg->rtextblocks = rswidth;
			}
		}
		if (!cfg->rtextblocks) {
			cfg->rtextblocks = (cfg->blocksize < XFS_MIN_RTEXTSIZE)
					? XFS_MIN_RTEXTSIZE >> cfg->blocklog
					: 1;
		}
	}
	ASSERT(cfg->rtextblocks);

	if (cli->sb_feat.reflink && cfg->rtblocks > 0 && cfg->rtextblocks > 1) {
		fprintf(stderr,
_("reflink not supported on realtime with extent sizes > 1\n"));
		usage();
	}
}

/* Validate the incoming extsize hint. */
static void
validate_extsize_hint(
	struct xfs_mount	*mp,
	struct cli_params	*cli)
{
	xfs_failaddr_t		fa;
	uint16_t		flags = 0;

	/*
	 * First we validate the extent size inherit hint on a directory so
	 * that we know that we'll be propagating a correct hint and flag to
	 * new files on the data device.
	 */
	if (cli->fsx.fsx_xflags & FS_XFLAG_EXTSZINHERIT)
		flags |= XFS_DIFLAG_EXTSZINHERIT;

	fa = libxfs_inode_validate_extsize(mp, cli->fsx.fsx_extsize, S_IFDIR,
			flags);
	if (fa) {
		fprintf(stderr,
_("illegal extent size hint %lld, must be less than %u.\n"),
				(long long)cli->fsx.fsx_extsize,
				min(XFS_MAX_BMBT_EXTLEN, mp->m_sb.sb_agblocks / 2));
		usage();
	}

	/*
	 * If the value is to be passed on to realtime files, revalidate with
	 * a realtime file so that we know the hint and flag that get passed on
	 * to realtime files will be correct.
	 */
	if (!(cli->fsx.fsx_xflags & FS_XFLAG_RTINHERIT))
		return;

	flags = XFS_DIFLAG_REALTIME;
	if (cli->fsx.fsx_xflags & FS_XFLAG_EXTSZINHERIT)
		flags |= XFS_DIFLAG_EXTSIZE;

	fa = libxfs_inode_validate_extsize(mp, cli->fsx.fsx_extsize, S_IFREG,
			flags);

	if (fa) {
		fprintf(stderr,
_("illegal extent size hint %lld, must be less than %u and a multiple of %u.\n"),
				(long long)cli->fsx.fsx_extsize,
				min(XFS_MAX_BMBT_EXTLEN, mp->m_sb.sb_agblocks / 2),
				mp->m_sb.sb_rextsize);
		usage();
	}
}

/* Validate the incoming CoW extsize hint. */
static void
validate_cowextsize_hint(
	struct xfs_mount	*mp,
	struct cli_params	*cli)
{
	xfs_failaddr_t		fa;
	uint64_t		flags2 = 0;

	/*
	 * Validate the copy on write extent size inherit hint on a directory
	 * so that we know that we'll be propagating a correct hint and flag to
	 * new files on the data device.
	 */
	if (cli->fsx.fsx_xflags & FS_XFLAG_COWEXTSIZE)
		flags2 |= XFS_DIFLAG2_COWEXTSIZE;

	fa = libxfs_inode_validate_cowextsize(mp, cli->fsx.fsx_cowextsize,
			S_IFDIR, 0, flags2);
	if (fa) {
		fprintf(stderr,
_("illegal CoW extent size hint %lld, must be less than %u.\n"),
				(long long)cli->fsx.fsx_cowextsize,
				min(XFS_MAX_BMBT_EXTLEN, mp->m_sb.sb_agblocks / 2));
		usage();
	}

	/*
	 * If the value is to be passed on to realtime files, revalidate with
	 * a realtime file so that we know the hint and flag that get passed on
	 * to realtime files will be correct.
	 */
	if (!(cli->fsx.fsx_xflags & FS_XFLAG_RTINHERIT))
		return;

	fa = libxfs_inode_validate_cowextsize(mp, cli->fsx.fsx_cowextsize,
			S_IFREG, XFS_DIFLAG_REALTIME, flags2);

	if (fa) {
		fprintf(stderr,
_("illegal CoW extent size hint %lld, must be less than %u and a multiple of %u. %p\n"),
				(long long)cli->fsx.fsx_cowextsize,
				min(XFS_MAX_BMBT_EXTLEN, mp->m_sb.sb_agblocks / 2),
				mp->m_sb.sb_rextsize, fa);
		usage();
	}
}

/* Complain if this filesystem is not a supported configuration. */
static void
validate_supported(
	struct xfs_mount	*mp,
	struct cli_params	*cli)
{
	/* Undocumented option to enable unsupported tiny filesystems. */
	if (!cli->is_supported) {
		printf(
 _("Filesystems formatted with --unsupported are not supported!!\n"));
		return;
	}

	/*
	 * fstests has a large number of tests that create tiny filesystems to
	 * perform specific regression and resource depletion tests in a
	 * controlled environment.  Avoid breaking fstests by allowing
	 * unsupported configurations if TEST_DIR, TEST_DEV, and QA_CHECK_FS
	 * are all set.
	 */
	if (getenv("TEST_DIR") && getenv("TEST_DEV") && getenv("QA_CHECK_FS"))
		return;

	/*
	 * We don't support filesystems smaller than 300MB anymore.  Tiny
	 * filesystems have never been XFS' design target.  This limit has been
	 * carefully calculated to prevent formatting with a log smaller than
	 * the "realistic" size.
	 *
	 * If the realistic log size is 64MB, there are four AGs, and the log
	 * AG should be at least 1/8 free after formatting, this gives us:
	 *
	 * 64MB * (8 / 7) * 4 = 293MB
	 */
	if (mp->m_sb.sb_dblocks < MEGABYTES(300, mp->m_sb.sb_blocklog)) {
		fprintf(stderr,
 _("Filesystem must be larger than 300MB.\n"));
		usage();
	}

	/*
	 * For best performance, we don't allow unrealistically small logs.
	 * See the comment for XFS_MIN_REALISTIC_LOG_BLOCKS.
	 */
	if (mp->m_sb.sb_logblocks <
			XFS_MIN_REALISTIC_LOG_BLOCKS(mp->m_sb.sb_blocklog)) {
		fprintf(stderr,
 _("Log size must be at least 64MB.\n"));
		usage();
	}

	/*
	 * Filesystems should not have fewer than two AGs, because we need to
	 * have redundant superblocks.
	 */
	if (mp->m_sb.sb_agcount < 2) {
		fprintf(stderr,
 _("Filesystem must have at least 2 superblocks for redundancy!\n"));
		usage();
	}
}

/*
 * Validate the configured stripe geometry, or is none is specified, pull
 * the configuration from the underlying device.
 *
 * CLI parameters come in as different units, go out as filesystem blocks.
 */
static void
calc_stripe_factors(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct fs_topology	*ft)
{
	long long int	big_dswidth;
	int		dsunit = 0;
	int		dswidth = 0;
	int		lsunit = 0;
	int		dsu = 0;
	int		dsw = 0;
	int		lsu = 0;
	bool		use_dev = false;

	if (cli_opt_set(&dopts, D_SUNIT))
		dsunit = cli->dsunit;
	if (cli_opt_set(&dopts, D_SWIDTH))
		dswidth = cli->dswidth;

	if (cli_opt_set(&dopts, D_SU))
		dsu = getnum(cli->dsu, &dopts, D_SU);
	if (cli_opt_set(&dopts, D_SW))
		dsw = cli->dsw;

	/* data sunit/swidth options */
	if (cli_opt_set(&dopts, D_SUNIT) != cli_opt_set(&dopts, D_SWIDTH)) {
		fprintf(stderr,
_("both data sunit and data swidth options must be specified\n"));
		usage();
	}

	/* convert dsu/dsw to dsunit/dswidth and use them from now on */
	if (dsu || dsw) {
		if (cli_opt_set(&dopts, D_SU) != cli_opt_set(&dopts, D_SW)) {
			fprintf(stderr,
_("both data su and data sw options must be specified\n"));
			usage();
		}

		big_dswidth = (long long int)dsu * dsw;
		if (BTOBBT(big_dswidth) > INT_MAX) {
			fprintf(stderr,
_("data stripe width (%lld) is too large of a multiple of the data stripe unit (%d)\n"),
				big_dswidth, dsu);
			usage();
		}

		if (!libxfs_validate_stripe_geometry(NULL, dsu, big_dswidth,
					cfg->sectorsize, false, false))
			usage();

		dsunit = BTOBBT(dsu);
		dswidth = BTOBBT(big_dswidth);
	} else if (!libxfs_validate_stripe_geometry(NULL, BBTOB(dsunit),
			BBTOB(dswidth), cfg->sectorsize, false, false)) {
		usage();
	}

	/* If sunit & swidth were manually specified as 0, same as noalign */
	if ((cli_opt_set(&dopts, D_SUNIT) || cli_opt_set(&dopts, D_SU)) &&
	    !dsunit && !dswidth)
		cfg->sb_feat.nodalign = true;

	/* if we are not using alignment, don't apply device defaults */
	if (cfg->sb_feat.nodalign) {
		cfg->dsunit = 0;
		cfg->dswidth = 0;
		goto check_lsunit;
	}

	/* if no stripe config set, use the device default */
	if (!dsunit) {
		/* Ignore nonsense from device report. */
		if (!libxfs_validate_stripe_geometry(NULL, BBTOB(ft->data.sunit),
				BBTOB(ft->data.swidth), 0, false, true)) {
			fprintf(stderr,
_("%s: Volume reports invalid stripe unit (%d) and stripe width (%d), ignoring.\n"),
				progname,
				BBTOB(ft->data.sunit), BBTOB(ft->data.swidth));
			ft->data.sunit = 0;
			ft->data.swidth = 0;
		} else if (cfg->dblocks < GIGABYTES(1, cfg->blocklog)) {
			/*
			 * Don't use automatic stripe detection if the device
			 * size is less than 1GB because the performance gains
			 * on such a small system are not worth the risk that
			 * we'll end up with an undersized log.
			 */
			if (ft->data.sunit || ft->data.swidth)
				fprintf(stderr,
_("%s: small data volume, ignoring data volume stripe unit %d and stripe width %d\n"),
						progname, ft->data.sunit,
						ft->data.swidth);
			ft->data.sunit = 0;
			ft->data.swidth = 0;
		} else {
			dsunit = ft->data.sunit;
			dswidth = ft->data.swidth;
			use_dev = true;
		}
	} else {
		/* check and warn if user-specified alignment is sub-optimal */
		if (ft->data.sunit && ft->data.sunit != dsunit) {
			fprintf(stderr,
_("%s: Specified data stripe unit %d is not the same as the volume stripe unit %d\n"),
				progname, dsunit, ft->data.sunit);
		}
		if (ft->data.swidth && ft->data.swidth != dswidth) {
			fprintf(stderr,
_("%s: Specified data stripe width %d is not the same as the volume stripe width %d\n"),
				progname, dswidth, ft->data.swidth);
		}
	}

	/*
	 * now we have our stripe config, check it's a multiple of block
	 * size.
	 */
	if ((BBTOB(dsunit) % cfg->blocksize) ||
	    (BBTOB(dswidth) % cfg->blocksize)) {
		/*
		 * If we are using device defaults, just clear them and we're
		 * good to go. Otherwise bail out with an error.
		 */
		if (!use_dev) {
			fprintf(stderr,
_("%s: Stripe unit(%d) or stripe width(%d) is not a multiple of the block size(%d)\n"),
				progname, BBTOB(dsunit), BBTOB(dswidth),
				cfg->blocksize);
			exit(1);
		}
		dsunit = 0;
		dswidth = 0;
		cfg->sb_feat.nodalign = true;
	}

	/* convert from 512 byte blocks to fs blocksize */
	cfg->dsunit = DTOBT(dsunit, cfg->blocklog);
	cfg->dswidth = DTOBT(dswidth, cfg->blocklog);

check_lsunit:
	/* log sunit options */
	if (cli_opt_set(&lopts, L_SUNIT))
		lsunit = cli->lsunit;
	else if (cli_opt_set(&lopts, L_SU))
		lsu = getnum(cli->lsu, &lopts, L_SU);
	else if (cfg->lsectorsize > XLOG_HEADER_SIZE)
		lsu = cfg->blocksize; /* lsunit matches filesystem block size */

	if (lsu) {
		/* verify if lsu is a multiple block size */
		if (lsu % cfg->blocksize != 0) {
			fprintf(stderr,
	_("log stripe unit (%d) must be a multiple of the block size (%d)\n"),
				lsu, cfg->blocksize);
			usage();
		}
		lsunit = (int)BTOBBT(lsu);
	}
	if (BBTOB(lsunit) % cfg->blocksize != 0) {
		fprintf(stderr,
_("log stripe unit (%d) must be a multiple of the block size (%d)\n"),
			BBTOB(lsunit), cfg->blocksize);
		usage();
	}

	/*
	 * check that log sunit is modulo fsblksize or default it to dsunit.
	 */
	if (lsunit) {
		/* convert from 512 byte blocks to fs blocks */
		cfg->lsunit = DTOBT(lsunit, cfg->blocklog);
	} else if (cfg->sb_feat.log_version == 2 &&
		   cfg->loginternal && cfg->dsunit) {
		/* lsunit and dsunit now in fs blocks */
		cfg->lsunit = cfg->dsunit;
	}

	if (cfg->sb_feat.log_version == 2 &&
	    cfg->lsunit * cfg->blocksize > 256 * 1024) {
		/* Warn only if specified on commandline */
		if (cli->lsu || cli->lsunit != -1) {
			fprintf(stderr,
_("log stripe unit (%d bytes) is too large (maximum is 256KiB)\n"
  "log stripe unit adjusted to 32KiB\n"),
				(cfg->lsunit * cfg->blocksize));
		}
		/* XXX: 64k block size? */
		cfg->lsunit = (32 * 1024) / cfg->blocksize;
	}

}

static void
open_devices(
	struct mkfs_params	*cfg,
	struct libxfs_init	*xi,
	struct zone_topology	*zt)
{
	uint64_t		sector_mask;

	/*
	 * Initialize.  This will open the log and rt devices as well.
	 */
	xi->setblksize = cfg->sectorsize;
	if (!libxfs_init(xi))
		usage();
	if (!xi->data.dev) {
		fprintf(stderr, _("no device name given in argument list\n"));
		usage();
	}

	if (zt->data.nr_zones) {
		zt->rt.zone_size = zt->data.zone_size;
		zt->rt.zone_capacity = zt->data.zone_capacity;
		zt->rt.nr_zones = zt->data.nr_zones - zt->data.nr_conv_zones;
	} else if (cfg->sb_feat.zoned && !cfg->rtstart && !xi->rt.dev) {
		/*
		 * By default reserve at 1% of the total capacity (rounded up to
		 * the next power of two) for metadata, but match the minimum we
		 * enforce elsewhere. This matches what SMR HDDs provide.
		 */
		uint64_t rt_target_size = max((xi->data.size + 99) / 100,
					      BTOBB(300 * 1024 * 1024));

		cfg->rtstart = 1;
		while (cfg->rtstart < rt_target_size)
			cfg->rtstart <<= 1;
	}

	if (cfg->rtstart) {
		if (cfg->rtstart >= xi->data.size) {
			fprintf(stderr,
 _("device size %lld too small for zoned allocator\n"), xi->data.size);
			usage();
		}
		xi->rt.size = xi->data.size - cfg->rtstart;
		xi->data.size = cfg->rtstart;
	}

	/*
	 * Ok, Linux only has a 1024-byte resolution on device _size_,
	 * and the sizes below are in basic 512-byte blocks,
	 * so if we have (size % 2), on any partition, we can't get
	 * to the last 512 bytes.  The same issue exists for larger
	 * sector sizes - we cannot write past the last sector.
	 *
	 * So, we reduce the size (in basic blocks) to a perfect
	 * multiple of the sector size, or 1024, whichever is larger.
	 */
	sector_mask = (uint64_t)-1 << (max(cfg->sectorlog, 10) - BBSHIFT);
	xi->data.size &= sector_mask;
	xi->rt.size &= sector_mask;
	xi->log.size &= (uint64_t)-1 << (max(cfg->lsectorlog, 10) - BBSHIFT);
}

static void
discard_devices(
	struct mkfs_params	*cfg,
	struct libxfs_init	*xi,
	struct zone_topology	*zt,
	int			quiet)
{
	/*
	 * This function has to be called after libxfs has been initialized.
	 */

	if (!xi->data.isfile) {
		uint64_t	nsectors = xi->data.size;

		if (cfg->rtstart && zt->data.nr_zones) {
			/*
			 * Note that the zone reset here includes the LBA range
			 * for the data device.
			 *
			 * This is because doing a single zone reset all on the
			 * entire device (which the kernel automatically does
			 * for us for a full device range) is a lot faster than
			 * resetting each zone individually and resetting
			 * the conventional zones used for the data device is a
			 * no-op.
			 */
			reset_zones(cfg, xi->data.fd, 0,
					cfg->rtstart + xi->rt.size, quiet);
			nsectors -= cfg->rtstart;
		}
		discard_blocks(xi->data.fd, nsectors, quiet);
	}
	if (xi->rt.dev && !xi->rt.isfile) {
		if (zt->rt.nr_zones)
			reset_zones(cfg, xi->rt.fd, 0, xi->rt.size, quiet);
		else
			discard_blocks(xi->rt.fd, xi->rt.size, quiet);
	}
	if (xi->log.dev && xi->log.dev != xi->data.dev && !xi->log.isfile)
		discard_blocks(xi->log.fd, xi->log.size, quiet);
}

static void
validate_datadev(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{
	struct libxfs_init	*xi = cli->xi;

	if (!xi->data.size) {
		/*
		 * if the device is a file, we can't validate the size here.
		 * Instead, the file will be truncated to the correct length
		 * later on. if it's not a file, we've got a dud device.
		 */
		if (!xi->data.isfile) {
			fprintf(stderr, _("can't get size of data subvolume\n"));
			usage();
		}
		ASSERT(cfg->dblocks);
	} else if (cfg->dblocks) {
		/* check the size fits into the underlying device */
		if (cfg->dblocks > DTOBT(xi->data.size, cfg->blocklog)) {
			fprintf(stderr,
_("size %s specified for data subvolume is too large, maximum is %lld blocks\n"),
				cli->dsize,
				(long long)DTOBT(xi->data.size, cfg->blocklog));
			usage();
		}
	} else {
		/* no user size, so use the full block device */
		cfg->dblocks = DTOBT(xi->data.size, cfg->blocklog);
	}

	if (cfg->dblocks < XFS_MIN_DATA_BLOCKS(cfg)) {
		fprintf(stderr,
_("size %lld of data subvolume is too small, minimum %lld blocks\n"),
			(long long)cfg->dblocks, XFS_MIN_DATA_BLOCKS(cfg));
		usage();
	}

	if (xi->data.bsize > cfg->sectorsize) {
		fprintf(stderr, _(
"Warning: the data subvolume sector size %u is less than the sector size \n\
reported by the device (%u).\n"),
			cfg->sectorsize, xi->data.bsize);
	}
}

static void
validate_logdev(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{
	struct libxfs_init	*xi = cli->xi;

	cfg->loginternal = cli->loginternal;

	/* now run device checks */
	if (cfg->loginternal) {
		/*
		 * if no sector size has been specified on the command line,
		 * use what has been configured and validated for the data
		 * device.
		 */
		if (!cli->lsectorsize) {
			cfg->lsectorsize = cfg->sectorsize;
			cfg->lsectorlog = cfg->sectorlog;
		}

		if (cfg->sectorsize != cfg->lsectorsize) {
			fprintf(stderr,
_("data and log sector sizes must be equal for internal logs\n"));
			usage();
		}
		if (cli->logsize && cfg->logblocks >= cfg->dblocks) {
			fprintf(stderr,
_("log size %lld too large for internal log\n"),
				(long long)cfg->logblocks);
			usage();
		}
		return;
	}

	/* External/log subvolume checks */
	if (!*xi->log.name || !xi->log.dev) {
		fprintf(stderr, _("no log subvolume or external log.\n"));
		usage();
	}

	if (!cfg->logblocks) {
		if (xi->log.size == 0) {
			fprintf(stderr,
_("unable to get size of the log subvolume.\n"));
			usage();
		}
		cfg->logblocks = DTOBT(xi->log.size, cfg->blocklog);
	} else if (cfg->logblocks > DTOBT(xi->log.size, cfg->blocklog)) {
		fprintf(stderr,
_("size %s specified for log subvolume is too large, maximum is %lld blocks\n"),
			cli->logsize,
			(long long)DTOBT(xi->log.size, cfg->blocklog));
		usage();
	}

	if (xi->log.bsize > cfg->lsectorsize) {
		fprintf(stderr, _(
"Warning: the log subvolume sector size %u is less than the sector size\n\
reported by the device (%u).\n"),
			cfg->lsectorsize, xi->log.bsize);
	}
}

static void
validate_rtdev(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct zone_topology	*zt)
{
	struct libxfs_init	*xi = cli->xi;

	if (!xi->rt.dev && !cfg->rtstart) {
		if (cli->rtsize) {
			fprintf(stderr,
_("size specified for non-existent rt subvolume\n"));
			usage();
		}

		cfg->rtblocks = 0;
		cfg->rtextents = 0;
		cfg->rtbmblocks = 0;
		return;
	}
	if (!xi->rt.size) {
		fprintf(stderr, _("Invalid zero length rt subvolume found\n"));
		usage();
	}

	if (cli->rtsize) {
		if (cfg->rtblocks > DTOBT(xi->rt.size, cfg->blocklog)) {
			fprintf(stderr,
_("size %s specified for rt subvolume is too large, maximum is %lld blocks\n"),
				cli->rtsize,
				(long long)DTOBT(xi->rt.size, cfg->blocklog));
			usage();
		}
		if (xi->rt.bsize > cfg->sectorsize) {
			fprintf(stderr, _(
"Warning: the realtime subvolume sector size %u is less than the sector size\n\
reported by the device (%u).\n"),
				cfg->sectorsize, xi->rt.bsize);
		}
	} else if (zt->rt.nr_zones) {
		cfg->rtblocks = DTOBT(zt->rt.nr_zones * zt->rt.zone_capacity,
				      cfg->blocklog);
	} else {
		/* grab volume size */
		cfg->rtblocks = DTOBT(xi->rt.size, cfg->blocklog);
	}

	cfg->rtextents = cfg->rtblocks / cfg->rtextblocks;
	if (cfg->rtextents == 0) {
		fprintf(stderr,
_("cannot have an rt subvolume with zero extents\n"));
		usage();
	}

	/*
	 * Note for rtgroup file systems this will be overriden in
	 * calculate_rtgroup_geometry.
	 */
	cfg->rtbmblocks = (xfs_extlen_t)howmany(cfg->rtextents,
						NBBY * cfg->blocksize);
}

static bool
ddev_is_solidstate(
	struct libxfs_init	*xi)
{
	unsigned short		rotational = 1;
	int			error;

	error = ioctl(xi->data.fd, BLKROTATIONAL, &rotational);
	if (error)
		return false;

	return rotational == 0;
}

static void
calc_concurrency_ag_geometry(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi)
{
	uint64_t		try_agsize;
	uint64_t		def_agsize;
	uint64_t		def_agcount;
	int			nr_threads = cli->data_concurrency;
	int			try_threads;

	calc_default_ag_geometry(cfg->blocklog, cfg->dblocks, cfg->dsunit,
			&def_agsize, &def_agcount);
	try_agsize = def_agsize;

	/*
	 * If the caller doesn't have a particular concurrency level in mind,
	 * set it to the number of CPUs in the system.
	 */
	if (nr_threads < 0)
		nr_threads = nr_cpus();

	/*
	 * Don't create fewer AGs than what we would create with the default
	 * geometry calculation.
	 */
	if (!nr_threads || nr_threads < def_agcount)
		goto out;

	/*
	 * Let's try matching the number of AGs to the number of CPUs.  If the
	 * proposed geometry results in AGs smaller than 4GB, reduce the AG
	 * count until we have 4GB AGs.  Don't let the thread count go below
	 * the default geometry calculation.
	 */
	try_threads = nr_threads;
	try_agsize = cfg->dblocks / try_threads;
	if (try_agsize < GIGABYTES(4, cfg->blocklog)) {
		do {
			try_threads--;
			if (try_threads <= def_agcount) {
				try_agsize = def_agsize;
				goto out;
			}

			try_agsize = cfg->dblocks / try_threads;
		} while (try_agsize < GIGABYTES(4, cfg->blocklog));
		goto out;
	}

	/*
	 * For large filesystems we try to ensure that the AG count is a
	 * multiple of the desired thread count.  Specifically, if the proposed
	 * AG size is larger than both the maximum AG size and the AG size we
	 * would have gotten with the defaults, add the thread count to the AG
	 * count until we get an AG size below both of those factors.
	 */
	while (try_agsize > XFS_AG_MAX_BLOCKS(cfg->blocklog) &&
	       try_agsize > def_agsize) {
		try_threads += nr_threads;
		try_agsize = cfg->dblocks / try_threads;
	}

out:
	cfg->agsize = try_agsize;
	cfg->agcount = howmany(cfg->dblocks, cfg->agsize);
}

static void
calculate_initial_ag_geometry(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi)
{
	if (cli->data_concurrency > 0) {
		calc_concurrency_ag_geometry(cfg, cli, xi);
	} else if (cli->agsize) {	/* User-specified AG size */
		cfg->agsize = getnum(cli->agsize, &dopts, D_AGSIZE);

		/*
		 * Check specified agsize is a multiple of blocksize.
		 */
		if (cfg->agsize % cfg->blocksize) {
			fprintf(stderr,
_("agsize (%s) not a multiple of fs blk size (%d)\n"),
				cli->agsize, cfg->blocksize);
			usage();
		}
		cfg->agsize /= cfg->blocksize;
		cfg->agcount = cfg->dblocks / cfg->agsize +
				(cfg->dblocks % cfg->agsize != 0);

	} else if (cli->agcount) {	/* User-specified AG count */
		cfg->agcount = cli->agcount;
		cfg->agsize = cfg->dblocks / cfg->agcount +
				(cfg->dblocks % cfg->agcount != 0);
	} else if (cli->data_concurrency == -1 && ddev_is_solidstate(xi)) {
		calc_concurrency_ag_geometry(cfg, cli, xi);
	} else {
		calc_default_ag_geometry(cfg->blocklog, cfg->dblocks,
					 cfg->dsunit, &cfg->agsize,
					 &cfg->agcount);
	}
}

/*
 * Align the AG size to stripe geometry. If this fails and we are using
 * discovered stripe geometry, tell the caller to clear the stripe geometry.
 * Otherwise, set the aligned geometry (valid or invalid!) so that the
 * validation call will fail and exit.
 */
static void
align_ag_geometry(
	struct mkfs_params	*cfg)
{
	uint64_t	tmp_agsize;
	int		dsunit = cfg->dsunit;

	if (!dsunit)
		goto validate;

	/*
	 * agsize is not a multiple of dsunit
	 */
	if ((cfg->agsize % dsunit) != 0) {
		/*
		 * Round up to stripe unit boundary. Also make sure
		 * that agsize is still larger than
		 * XFS_AG_MIN_BLOCKS(blocklog)
		 */
		tmp_agsize = ((cfg->agsize + dsunit - 1) / dsunit) * dsunit;
		/*
		 * Round down to stripe unit boundary if rounding up
		 * created an AG size that is larger than the AG max.
		 */
		if (tmp_agsize > XFS_AG_MAX_BLOCKS(cfg->blocklog))
			tmp_agsize = (cfg->agsize / dsunit) * dsunit;

		if (tmp_agsize < XFS_AG_MIN_BLOCKS(cfg->blocklog) &&
		    tmp_agsize > XFS_AG_MAX_BLOCKS(cfg->blocklog)) {

			/*
			 * If the AG size is invalid and we are using device
			 * probed stripe alignment, just clear the alignment
			 * and continue on.
			 */
			if (!cli_opt_set(&dopts, D_SUNIT) &&
			    !cli_opt_set(&dopts, D_SU)) {
				cfg->dsunit = 0;
				cfg->dswidth = 0;
				goto validate;
			}
			/*
			 * set the agsize to the invalid value so the following
			 * validation of the ag will fail and print a nice error
			 * and exit.
			 */
			cfg->agsize = tmp_agsize;
			goto validate;
		}

		/* update geometry to be stripe unit aligned */
		cfg->agsize = tmp_agsize;
		if (!cli_opt_set(&dopts, D_AGCOUNT))
			cfg->agcount = cfg->dblocks / cfg->agsize +
					(cfg->dblocks % cfg->agsize != 0);
		if (cli_opt_set(&dopts, D_AGSIZE))
			fprintf(stderr,
_("agsize rounded to %lld, sunit = %d\n"),
				(long long)cfg->agsize, dsunit);
	}

	if ((cfg->agsize % cfg->dswidth) == 0 &&
	    cfg->dswidth != cfg->dsunit &&
	    cfg->agcount > 1) {

		if (cli_opt_set(&dopts, D_AGCOUNT) ||
		    cli_opt_set(&dopts, D_AGSIZE)) {
			printf(_(
"Warning: AG size is a multiple of stripe width.  This can cause performance\n\
problems by aligning all AGs on the same disk.  To avoid this, run mkfs with\n\
an AG size that is one stripe unit smaller or larger, for example %llu.\n"),
				(unsigned long long)cfg->agsize - dsunit);
			fflush(stdout);
			goto validate;
		}

		/*
		 * This is a non-optimal configuration because all AGs start on
		 * the same disk in the stripe.  Changing the AG size by one
		 * sunit will guarantee that this does not happen.
		 */
		tmp_agsize = cfg->agsize - dsunit;
		if (tmp_agsize < XFS_AG_MIN_BLOCKS(cfg->blocklog)) {
			tmp_agsize = cfg->agsize + dsunit;
			if (cfg->dblocks < cfg->agsize) {
				/* oh well, nothing to do */
				tmp_agsize = cfg->agsize;
			}
		}

		cfg->agsize = tmp_agsize;
		cfg->agcount = cfg->dblocks / cfg->agsize +
				(cfg->dblocks % cfg->agsize != 0);
	}

validate:
	/*
	 * If the last AG is too small, reduce the filesystem size
	 * and drop the blocks.
	 */
	if (cfg->dblocks % cfg->agsize != 0 &&
	     (cfg->dblocks % cfg->agsize < XFS_AG_MIN_BLOCKS(cfg->blocklog))) {
		ASSERT(!cli_opt_set(&dopts, D_AGCOUNT));
		cfg->dblocks = (xfs_rfsblock_t)((cfg->agcount - 1) * cfg->agsize);
		cfg->agcount--;
		ASSERT(cfg->agcount != 0);
	}

	validate_ag_geometry(cfg->blocklog, cfg->dblocks,
			     cfg->agsize, cfg->agcount);
}

static uint64_t
calc_rgsize_extsize_nonpower(
	struct mkfs_params	*cfg)
{
	uint64_t		try_rgsize, rgsize, rgcount;

	/*
	 * For non-power-of-two rt extent sizes, round the rtgroup size down to
	 * the nearest extent.
	 */
	calc_default_rtgroup_geometry(cfg->blocklog, cfg->rtblocks, &rgsize,
			&rgcount);
	rgsize -= rgsize % cfg->rtextblocks;
	rgsize = min(XFS_MAX_RGBLOCKS, rgsize);

	/*
	 * If we would be left with a too-small rtgroup, increase or decrease
	 * the size of the group until we have a working geometry.
	 */
	for (try_rgsize = rgsize;
	     try_rgsize <= XFS_MAX_RGBLOCKS - cfg->rtextblocks;
	     try_rgsize += cfg->rtextblocks) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}
	for (try_rgsize = rgsize;
	     try_rgsize > (2 * cfg->rtextblocks);
	     try_rgsize -= cfg->rtextblocks) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}

	fprintf(stderr,
_("realtime group size (%llu) not at all congruent with extent size (%llu)\n"),
			(unsigned long long)rgsize,
			(unsigned long long)cfg->rtextblocks);
	usage();
	return 0;
}

static uint64_t
calc_rgsize_extsize_power(
	struct mkfs_params	*cfg)
{
	uint64_t		try_rgsize, rgsize, rgcount;
	unsigned int		rgsizelog;

	/*
	 * Find the rt group size that is both a power of two and yields at
	 * least as many rt groups as the default geometry specified.
	 */
	calc_default_rtgroup_geometry(cfg->blocklog, cfg->rtblocks, &rgsize,
			&rgcount);
	rgsizelog = log2_rounddown(rgsize);
	rgsize = min(XFS_MAX_RGBLOCKS, 1U << rgsizelog);

	/*
	 * If we would be left with a too-small rtgroup, increase or decrease
	 * the size of the group by powers of 2 until we have a working
	 * geometry.  If that doesn't work, try bumping by the extent size.
	 */
	for (try_rgsize = rgsize;
	     try_rgsize <= XFS_MAX_RGBLOCKS - cfg->rtextblocks;
	     try_rgsize <<= 2) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}
	for (try_rgsize = rgsize;
	     try_rgsize > (2 * cfg->rtextblocks);
	     try_rgsize >>= 2) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}
	for (try_rgsize = rgsize;
	     try_rgsize <= XFS_MAX_RGBLOCKS - cfg->rtextblocks;
	     try_rgsize += cfg->rtextblocks) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}
	for (try_rgsize = rgsize;
	     try_rgsize > (2 * cfg->rtextblocks);
	     try_rgsize -= cfg->rtextblocks) {
		if (cfg->rtblocks % try_rgsize >= (2 * cfg->rtextblocks))
			return try_rgsize;
	}

	fprintf(stderr,
_("realtime group size (%llu) not at all congruent with extent size (%llu)\n"),
			(unsigned long long)rgsize,
			(unsigned long long)cfg->rtextblocks);
	usage();
	return 0;
}

static bool
rtdev_is_solidstate(
	struct libxfs_init	*xi)
{
	unsigned short		rotational = 1;
	int			error;

	error = ioctl(xi->rt.fd, BLKROTATIONAL, &rotational);
	if (error)
		return false;

	return rotational == 0;
}

static void
calc_concurrency_rtgroup_geometry(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi)
{
	uint64_t		try_rgsize;
	uint64_t		def_rgsize;
	uint64_t		def_rgcount;
	int			nr_threads = cli->rtvol_concurrency;
	int			try_threads;

	if (is_power_of_2(cfg->rtextblocks))
		def_rgsize = calc_rgsize_extsize_power(cfg);
	else
		def_rgsize = calc_rgsize_extsize_nonpower(cfg);
	def_rgcount = howmany(cfg->rtblocks, def_rgsize);
	try_rgsize = def_rgsize;

	/*
	 * If the caller doesn't have a particular concurrency level in mind,
	 * set it to the number of CPUs in the system.
	 */
	if (nr_threads < 0)
		nr_threads = nr_cpus();

	/*
	 * Don't create fewer rtgroups than what we would create with the
	 * default geometry calculation.
	 */
	if (!nr_threads || nr_threads < def_rgcount)
		goto out;

	/*
	 * Let's try matching the number of rtgroups to the number of CPUs.  If
	 * the proposed geometry results in rtgroups smaller than 4GB, reduce
	 * the rtgroup count until we have 4GB rtgroups.  Don't let the thread
	 * count go below the default geometry calculation.
	 */
	try_threads = nr_threads;
	try_rgsize = cfg->rtblocks / try_threads;
	if (try_rgsize < GIGABYTES(4, cfg->blocklog)) {
		do {
			try_threads--;
			if (try_threads <= def_rgcount) {
				try_rgsize = def_rgsize;
				goto out;
			}

			try_rgsize = cfg->rtblocks / try_threads;
		} while (try_rgsize < GIGABYTES(4, cfg->blocklog));
		goto out;
	}

	/*
	 * For large filesystems we try to ensure that the rtgroup count is a
	 * multiple of the desired thread count.  Specifically, if the proposed
	 * rtgroup size is larger than both the maximum rtgroup size and the
	 * rtgroup size we would have gotten with the defaults, add the thread
	 * count to the rtgroup count until we get an rtgroup size below both
	 * of those factors.
	 */
	while (try_rgsize > XFS_MAX_RGBLOCKS && try_rgsize > def_rgsize) {
		try_threads += nr_threads;
		try_rgsize = cfg->dblocks / try_threads;
	}

out:
	cfg->rgsize = try_rgsize;
	cfg->rgcount = howmany(cfg->rtblocks, cfg->rgsize);
}

static void
validate_rtgroup_geometry(
	struct mkfs_params	*cfg)
{
	if (cfg->rgsize > XFS_MAX_RGBLOCKS) {
		fprintf(stderr,
_("realtime group size (%llu) must be less than the maximum (%u)\n"),
				(unsigned long long)cfg->rgsize,
				XFS_MAX_RGBLOCKS);
		usage();
	}

	if (cfg->rgsize % cfg->rtextblocks != 0) {
		fprintf(stderr,
_("realtime group size (%llu) not a multiple of rt extent size (%llu)\n"),
				(unsigned long long)cfg->rgsize,
				(unsigned long long)cfg->rtextblocks);
		usage();
	}

	if (cfg->rgsize <= cfg->rtextblocks) {
		fprintf(stderr,
_("realtime group size (%llu) must be at least two realtime extents\n"),
				(unsigned long long)cfg->rgsize);
		usage();
	}

	if (cfg->rgcount > XFS_MAX_RGNUMBER) {
		fprintf(stderr,
_("realtime group count (%llu) must be less than the maximum (%u)\n"),
				(unsigned long long)cfg->rgcount,
				XFS_MAX_RGNUMBER);
		usage();
	}
}

static void
calculate_rtgroup_geometry(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi)
{
	if (!cli->sb_feat.metadir) {
		cfg->rgcount = 0;
		cfg->rgsize = 0;
		return;
	}

	if (cli->rgsize) {	/* User-specified rtgroup size */
		cfg->rgsize = getnum(cli->rgsize, &ropts, R_RGSIZE);

		/*
		 * Check specified agsize is a multiple of blocksize.
		 */
		if (cfg->rgsize % cfg->blocksize) {
			fprintf(stderr,
_("rgsize (%s) not a multiple of fs blk size (%d)\n"),
				cli->rgsize, cfg->blocksize);
			usage();
		}
		cfg->rgsize /= cfg->blocksize;
		cfg->rgcount = cfg->rtblocks / cfg->rgsize +
				(cfg->rtblocks % cfg->rgsize != 0);

	} else if (cli->rgcount) {	/* User-specified rtgroup count */
		cfg->rgcount = cli->rgcount;
		cfg->rgsize = cfg->rtblocks / cfg->rgcount +
				(cfg->rtblocks % cfg->rgcount != 0);
	} else if (cfg->rtblocks == 0) {
		/*
		 * If nobody specified a realtime device or the rtgroup size,
		 * try 1TB, rounded down to the nearest rt extent.
		 */
		cfg->rgsize = TERABYTES(1, cfg->blocklog);
		cfg->rgsize -= cfg->rgsize % cfg->rtextblocks;
		cfg->rgcount = 0;
	} else if (cfg->rtblocks < cfg->rtextblocks * 2) {
		/* too small even for a single group */
		cfg->rgsize = cfg->rtblocks;
		cfg->rgcount = 0;
	} else if (cli->rtvol_concurrency > 0 ||
		   (cli->data_concurrency == -1 && rtdev_is_solidstate(xi))) {
		calc_concurrency_rtgroup_geometry(cfg, cli, xi);
	} else if (is_power_of_2(cfg->rtextblocks)) {
		cfg->rgsize = calc_rgsize_extsize_power(cfg);
		cfg->rgcount = cfg->rtblocks / cfg->rgsize +
				(cfg->rtblocks % cfg->rgsize != 0);
	} else {
		cfg->rgsize = calc_rgsize_extsize_nonpower(cfg);
		cfg->rgcount = cfg->rtblocks / cfg->rgsize +
				(cfg->rtblocks % cfg->rgsize != 0);
	}

	validate_rtgroup_geometry(cfg);

	if (cfg->rtextents)
		cfg->rtbmblocks = howmany(cfg->rgsize / cfg->rtextblocks,
			NBBY * (cfg->blocksize - sizeof(struct xfs_rtbuf_blkinfo)));
}

/*
 * If we're creating a zoned filesystem and the user specified a size, add
 * enough over-provisioning to be able to back the requested amount of
 * writable space.
 */
static void
adjust_nr_zones(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi,
	struct zone_topology	*zt)
{
	uint64_t		new_rtblocks, slack;
	unsigned int		max_zones;

	if (zt->rt.nr_zones)
		max_zones = zt->rt.nr_zones;
	else
		max_zones = DTOBT(xi->rt.size, cfg->blocklog) / cfg->rgsize;

	if (!cli->rgcount)
		cfg->rgcount += XFS_RESERVED_ZONES;
	if (cfg->rgcount > max_zones) {
		cfg->rgcount = max_zones;
		fprintf(stderr,
_("Warning: not enough zones for backing requested rt size due to\n"
  "over-provisioning needs, writable size will be less than %s\n"),
			cli->rtsize);
	}
	new_rtblocks = (cfg->rgcount * cfg->rgsize);
	slack = (new_rtblocks - cfg->rtblocks) % cfg->rgsize;

	cfg->rtblocks = new_rtblocks;
	cfg->rtextents = cfg->rtblocks / cfg->rtextblocks;

	/*
	 * Add the slack to the end of the last zone to the reserved blocks.
	 * This ensures the visible user capacity is exactly the one that the
	 * user asked for.
	 */
	cfg->rtreserved += (slack * cfg->blocksize);
}

static void
calculate_zone_geometry(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi,
	struct zone_topology	*zt)
{
	if (cfg->rtblocks == 0) {
		fprintf(stderr,
_("empty zoned realtime device not supported.\n"));
		usage();
	}

	if (zt->rt.nr_zones) {
		/* The RT device has hardware zones */
		cfg->rgsize = zt->rt.zone_capacity * 512;

		if (cfg->rgsize % cfg->blocksize) {
			fprintf(stderr,
_("rgsize (%s) not a multiple of fs blk size (%d)\n"),
				cli->rgsize, cfg->blocksize);
			usage();
		}
		if (cli->rgsize) {
			fprintf(stderr,
_("rgsize (%s) may not be specified when the rt device is zoned\n"),
				cli->rgsize);
			usage();
		}

		cfg->rgsize /= cfg->blocksize;
		cfg->rgcount = howmany(cfg->rtblocks, cfg->rgsize);

		if (cli->rgcount > cfg->rgcount) {
			fprintf(stderr,
_("rgcount (%llu) is larger than hardware zone count (%llu)\n"),
					(unsigned long long)cli->rgcount,
					(unsigned long long)cfg->rgcount);
			usage();
		} else if (cli->rgcount && cli->rgcount < cfg->rgcount) {
			/* constrain the rt device to the given rgcount */
			cfg->rgcount = cli->rgcount;
		}
	} else {
		/* No hardware zones */
		if (cli->rgsize) {
			/* User-specified rtgroup size */
			cfg->rgsize = getnum(cli->rgsize, &ropts, R_RGSIZE);

			/* Check specified agsize is a multiple of blocksize. */
			if (cfg->rgsize % cfg->blocksize) {
				fprintf(stderr,
_("rgsize (%s) not a multiple of fs blk size (%d)\n"),
					cli->rgsize, cfg->blocksize);
				usage();
			}
			cfg->rgsize /= cfg->blocksize;
			cfg->rgcount = cfg->rtblocks / cfg->rgsize +
					(cfg->rtblocks % cfg->rgsize != 0);
		} else if (cli->rgcount) {
			/* User-specified rtgroup count */
			cfg->rgcount = cli->rgcount;
			cfg->rgsize = cfg->rtblocks / cfg->rgcount +
					(cfg->rtblocks % cfg->rgcount != 0);
		} else {
			/* 256MB zones just like typical SMR HDDs */
			cfg->rgsize = MEGABYTES(256, cfg->blocklog);
			cfg->rgcount = cfg->rtblocks / cfg->rgsize +
					(cfg->rtblocks % cfg->rgsize != 0);
		}
	}

	if (cli->rtsize || cli->rgcount)
		adjust_nr_zones(cfg, cli, xi, zt);

	if (cfg->rgcount < XFS_MIN_ZONES)  {
		fprintf(stderr,
_("realtime group count (%llu) must be greater than the minimum zone count (%u)\n"),
				(unsigned long long)cfg->rgcount,
				XFS_MIN_ZONES);
		usage();
	}

	validate_rtgroup_geometry(cfg);

	/* Zoned RT devices don't use the rtbitmap, and have no bitmap blocks */
	cfg->rtbmblocks = 0;
}

static void
calculate_imaxpct(
	struct mkfs_params	*cfg,
	struct cli_params	*cli)
{
	if (cli->imaxpct >= 0) {
		cfg->imaxpct = cli->imaxpct;
		return;
	}

	/*
	 * This returns the % of the disk space that is used for
	 * inodes, it changes relatively to the FS size:
	 *  - over  50 TB, use 1%,
	 *  - 1TB - 50 TB, use 5%,
	 *  - under  1 TB, use XFS_DFL_IMAXIMUM_PCT (25%).
	 */

	if (cfg->dblocks < TERABYTES(1, cfg->blocklog))
		cfg->imaxpct = XFS_DFL_IMAXIMUM_PCT;
	else if (cfg->dblocks < TERABYTES(50, cfg->blocklog))
		cfg->imaxpct = 5;
	else
		cfg->imaxpct = 1;
}

/*
 * Set up the initial state of the superblock so we can start using the
 * libxfs geometry macros.
 */
static void
sb_set_features(
	struct mkfs_params	*cfg,
	struct xfs_sb		*sbp)
{
	struct sb_feat_args	*fp = &cfg->sb_feat;

	sbp->sb_versionnum = XFS_DFL_SB_VERSION_BITS;
	if (fp->crcs_enabled)
		sbp->sb_versionnum |= XFS_SB_VERSION_5;
	else
		sbp->sb_versionnum |= XFS_SB_VERSION_4;

	if (fp->inode_align) {
		int     cluster_size = XFS_INODE_BIG_CLUSTER_SIZE;

		sbp->sb_versionnum |= XFS_SB_VERSION_ALIGNBIT;
		if (cfg->sb_feat.crcs_enabled)
			cluster_size *= cfg->inodesize / XFS_DINODE_MIN_SIZE;
		sbp->sb_inoalignmt = cluster_size >> cfg->blocklog;
	} else
		sbp->sb_inoalignmt = 0;

	if (cfg->dsunit)
		sbp->sb_versionnum |= XFS_SB_VERSION_DALIGNBIT;
	if (fp->log_version == 2)
		sbp->sb_versionnum |= XFS_SB_VERSION_LOGV2BIT;
	if (fp->attr_version == 1)
		sbp->sb_versionnum |= XFS_SB_VERSION_ATTRBIT;
	if (fp->nci)
		sbp->sb_versionnum |= XFS_SB_VERSION_BORGBIT;

	if (cfg->sectorsize > BBSIZE || cfg->lsectorsize > BBSIZE) {
		sbp->sb_versionnum |= XFS_SB_VERSION_SECTORBIT;
		sbp->sb_logsectlog = (uint8_t)cfg->lsectorlog;
		sbp->sb_logsectsize = (uint16_t)cfg->lsectorsize;
	} else {
		sbp->sb_logsectlog = 0;
		sbp->sb_logsectsize = 0;
	}

	sbp->sb_features2 = 0;
	if (fp->lazy_sb_counters)
		sbp->sb_features2 |= XFS_SB_VERSION2_LAZYSBCOUNTBIT;
	if (fp->projid32bit)
		sbp->sb_features2 |= XFS_SB_VERSION2_PROJID32BIT;
	if (fp->crcs_enabled)
		sbp->sb_features2 |= XFS_SB_VERSION2_CRCBIT;
	if (fp->attr_version == 2)
		sbp->sb_features2 |= XFS_SB_VERSION2_ATTR2BIT;

	/* v5 superblocks have their own feature bit for dirftype */
	if (fp->dirftype && !fp->crcs_enabled)
		sbp->sb_features2 |= XFS_SB_VERSION2_FTYPE;

	if (fp->qflags)
		sbp->sb_versionnum |= XFS_SB_VERSION_QUOTABIT;

	/* update whether extended features are in use */
	if (sbp->sb_features2 != 0)
		sbp->sb_versionnum |= XFS_SB_VERSION_MOREBITSBIT;

	/*
	 * Due to a structure alignment issue, sb_features2 ended up in one
	 * of two locations, the second "incorrect" location represented by
	 * the sb_bad_features2 field. To avoid older kernels mounting
	 * filesystems they shouldn't, set both field to the same value.
	 */
	if (!fp->metadir)
		sbp->sb_bad_features2 = sbp->sb_features2;

	/*
	 * This will be overriden later for real rtgroup file systems.  For
	 * !rtgroups filesystems, we pretend that there's one huge group, just
	 * like __xfs_sb_from_disk does.
	 */
	sbp->sb_rgcount = 1;
	sbp->sb_rgextents = 0;

	if (!fp->crcs_enabled)
		return;

	/* default features for v5 filesystems */
	sbp->sb_features_compat = 0;
	sbp->sb_features_ro_compat = 0;
	sbp->sb_features_incompat = XFS_SB_FEAT_INCOMPAT_FTYPE;
	sbp->sb_features_log_incompat = 0;

	if (fp->finobt)
		sbp->sb_features_ro_compat = XFS_SB_FEAT_RO_COMPAT_FINOBT;
	if (fp->rmapbt)
		sbp->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_RMAPBT;
	if (fp->reflink)
		sbp->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_REFLINK;
	if (fp->inobtcnt)
		sbp->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_INOBTCNT;
	if (fp->bigtime)
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_BIGTIME;

	/*
	 * Sparse inode chunk support has two main inode alignment requirements.
	 * First, sparse chunk alignment must match the cluster size. Second,
	 * full chunk alignment must match the inode chunk size.
	 *
	 * Copy the already calculated/scaled inoalignmt to spino_align and
	 * update the former to the full inode chunk size.
	 */
	if (fp->spinodes) {
		sbp->sb_spino_align = sbp->sb_inoalignmt;
		sbp->sb_inoalignmt = XFS_INODES_PER_CHUNK *
				cfg->inodesize >> cfg->blocklog;
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_SPINODES;
	}

	if (fp->nrext64)
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_NREXT64;
	if (fp->exchrange)
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_EXCHRANGE;
	if (fp->parent_pointers) {
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_PARENT;
		/*
		 * Set ATTRBIT even if mkfs doesn't write out a single parent
		 * pointer so that the kernel doesn't have to do that for us
		 * with a synchronous write to the primary super at runtime.
		 */
		sbp->sb_versionnum |= XFS_SB_VERSION_ATTRBIT;
	}
	if (fp->metadir) {
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_METADIR;
		sbp->sb_rgcount = cfg->rgcount;
		sbp->sb_rgextents = cfg->rgsize / cfg->rtextblocks;
		sbp->sb_rgblklog = libxfs_compute_rgblklog(sbp->sb_rgextents,
							   cfg->rtextblocks);
	}

	if (fp->zoned) {
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_ZONED;
		sbp->sb_rtstart = (cfg->rtstart * 512) / cfg->blocksize;
		sbp->sb_rtreserved = cfg->rtreserved / cfg->blocksize;
	}
	if (fp->zone_gaps)
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_ZONE_GAPS;
}

/*
 * Make sure that the log size is a multiple of the stripe unit
 */
static void
align_log_size(
	struct mkfs_params	*cfg,
	int			sunit,
	int			max_logblocks)
{
	uint64_t		tmp_logblocks;

	/* nothing to do if it's already aligned. */
	if ((cfg->logblocks % sunit) == 0)
		return;

	if (cli_opt_set(&lopts, L_SIZE)) {
		fprintf(stderr,
_("log size %lld is not a multiple of the log stripe unit %d\n"),
			(long long) cfg->logblocks, sunit);
		usage();
	}

	tmp_logblocks = roundup_64(cfg->logblocks, sunit);

	/* If the log is too large, round down instead of round up */
	if ((tmp_logblocks > XFS_MAX_LOG_BLOCKS) ||
	    ((tmp_logblocks << cfg->blocklog) > XFS_MAX_LOG_BYTES) ||
	    tmp_logblocks > max_logblocks) {
		tmp_logblocks = rounddown_64(cfg->logblocks, sunit);
	}
	cfg->logblocks = tmp_logblocks;
}

/*
 * Make sure that the internal log is correctly aligned to the specified
 * stripe unit.
 */
static void
align_internal_log(
	struct mkfs_params	*cfg,
	struct xfs_mount	*mp,
	int			sunit,
	int			max_logblocks)
{
	/* round up log start if necessary */
	if ((cfg->logstart % sunit) != 0)
		cfg->logstart = ((cfg->logstart + (sunit - 1)) / sunit) * sunit;

	/* If our log start overlaps the next AG's metadata, fail. */
	if (!libxfs_verify_fsbno(mp, cfg->logstart)) {
		fprintf(stderr,
_("Due to stripe alignment, the internal log start (%lld) cannot be aligned\n"
  "within an allocation group.\n"),
			(long long) cfg->logstart);
		usage();
	}

	/* round up/down the log size now */
	align_log_size(cfg, sunit, max_logblocks);

	/*
	 * If the end of the log has been rounded past the end of the AG,
	 * reduce logblocks by a stripe unit to try to get it back under EOAG.
	 */
	if (!libxfs_verify_fsbext(mp, cfg->logstart, cfg->logblocks) &&
	    cfg->logblocks > sunit) {
		cfg->logblocks -= sunit;
	}

	/* check the aligned log still starts and ends in the same AG. */
	if (!libxfs_verify_fsbext(mp, cfg->logstart, cfg->logblocks)) {
		fprintf(stderr,
_("Due to stripe alignment, the internal log size (%lld) is too large.\n"
  "Must fit within an allocation group.\n"),
			(long long) cfg->logblocks);
		usage();
	}
}

static void
validate_log_size(uint64_t logblocks, int blocklog, int min_logblocks)
{
	if (logblocks < min_logblocks) {
		fprintf(stderr,
	_("log size %lld blocks too small, minimum size is %d blocks\n"),
			(long long)logblocks, min_logblocks);
		usage();
	}
	if (logblocks > XFS_MAX_LOG_BLOCKS) {
		fprintf(stderr,
	_("log size %lld blocks too large, maximum size is %lld blocks\n"),
			(long long)logblocks, XFS_MAX_LOG_BLOCKS);
		usage();
	}
	if ((logblocks << blocklog) > XFS_MAX_LOG_BYTES) {
		fprintf(stderr,
	_("log size %lld bytes too large, maximum size is %lld bytes\n"),
			(long long)(logblocks << blocklog), XFS_MAX_LOG_BYTES);
		usage();
	}
}

static void
adjust_ag0_internal_logblocks(
	struct mkfs_params	*cfg,
	struct xfs_mount	*mp,
	int			min_logblocks,
	int			*max_logblocks)
{
	int			backoff = 0;
	int			ichunk_blocks;

	/*
	 * mkfs will trip over the write verifiers if the log is allocated in
	 * AG 0 and consumes enough space that we cannot allocate a non-sparse
	 * inode chunk for the root directory.  The inode allocator requires
	 * that the AG have enough free space for the chunk itself plus enough
	 * to fix up the freelist with aligned blocks if we need to fill the
	 * allocation from the AGFL.
	 */
	ichunk_blocks = XFS_INODES_PER_CHUNK * cfg->inodesize >> cfg->blocklog;
	backoff = ichunk_blocks * 4;

	/*
	 * We try to align inode allocations to the data device stripe unit,
	 * so ensure there's enough space to perform an aligned allocation.
	 * The inode geometry structure isn't set up yet, so compute this by
	 * hand.
	 */
	backoff = max(backoff, cfg->dsunit * 2);

	*max_logblocks -= backoff;

	/* If the specified log size is too big, complain. */
	if (cli_opt_set(&lopts, L_SIZE) && cfg->logblocks > *max_logblocks) {
		fprintf(stderr,
_("internal log size %lld too large, must be less than %d\n"),
			(long long)cfg->logblocks,
			*max_logblocks);
		usage();
	}

	cfg->logblocks = min(cfg->logblocks, *max_logblocks);
}

static uint64_t
calc_concurrency_logblocks(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi,
	unsigned int		max_tx_bytes)
{
	uint64_t		log_bytes;
	uint64_t		logblocks = cfg->logblocks;
	unsigned int		new_logblocks;

	if (cli->log_concurrency < 0) {
		if (!ddev_is_solidstate(xi))
			goto out;

		cli->log_concurrency = nr_cpus();
	}
	if (cli->log_concurrency == 0)
		goto out;

	/*
	 * If this filesystem is smaller than a gigabyte, there's little to be
	 * gained from making the log larger.
	 */
	if (cfg->dblocks < GIGABYTES(1, cfg->blocklog))
		goto out;

	/*
	 * Create a log that is large enough to handle simultaneous maximally
	 * sized transactions at the concurrency level specified by the user
	 * without blocking for space.  Increase the figure by 50% so that
	 * background threads can also run.
	 */
	log_bytes = (uint64_t)max_tx_bytes * 3 * cli->log_concurrency / 2;
	new_logblocks = min(XFS_MAX_LOG_BYTES >> cfg->blocklog,
				log_bytes >> cfg->blocklog);

	logblocks = max(logblocks, new_logblocks);
out:
	return logblocks;
}

static void
calculate_log_size(
	struct mkfs_params	*cfg,
	struct cli_params	*cli,
	struct libxfs_init	*xi,
	struct xfs_mount	*mp)
{
	struct xfs_sb		*sbp = &mp->m_sb;
	int			min_logblocks;	/* absolute minimum */
	int			max_logblocks;	/* absolute max for this AG */
	unsigned int		max_tx_bytes = 0;
	struct xfs_mount	mount;
	struct libxfs_init	dummy_init = { };

	/* we need a temporary mount to calculate the minimum log size. */
	memset(&mount, 0, sizeof(mount));
	mount.m_sb = *sbp;
	libxfs_mount(&mount, &mp->m_sb, &dummy_init, 0);
	min_logblocks = libxfs_log_calc_minimum_size(&mount);
	if (cli->log_concurrency != 0) {
		struct xfs_trans_res	res;

		libxfs_log_get_max_trans_res(&mount, &res);
		max_tx_bytes = res.tr_logres * res.tr_logcount;
	}
	libxfs_umount(&mount);

	ASSERT(min_logblocks);
	min_logblocks = max(XFS_MIN_LOG_BLOCKS, min_logblocks);

	/* if we have lots of blocks, check against XFS_MIN_LOG_BYTES, too */
	if (!cli->logsize &&
	    cfg->dblocks >= (1024*1024*1024) >> cfg->blocklog)
		min_logblocks = max(min_logblocks,
				    XFS_MIN_LOG_BYTES >> cfg->blocklog);

	/*
	 * external logs will have a device and size by now, so all we have
	 * to do is validate it against minimum size and align it.
	 */
	if (!cfg->loginternal) {
		if (min_logblocks > cfg->logblocks) {
			fprintf(stderr,
_("external log device size %lld blocks too small, must be at least %lld blocks\n"),
				(long long)cfg->logblocks,
				(long long)min_logblocks);
			usage();
		}
		cfg->logstart = 0;
		cfg->logagno = 0;
		if (cfg->lsunit) {
			uint64_t	max_logblocks;

			max_logblocks = min(DTOBT(xi->log.size, cfg->blocklog),
					    XFS_MAX_LOG_BLOCKS);
			align_log_size(cfg, cfg->lsunit, max_logblocks);
		}

		validate_log_size(cfg->logblocks, cfg->blocklog, min_logblocks);
		return;
	}

	/*
	 * Make sure the log fits wholly within an AG
	 *
	 * XXX: If agf->freeblks ends up as 0 because the log uses all
	 * the free space, it causes the kernel all sorts of problems
	 * with per-ag reservations. Right now just back it off one
	 * block, but there's a whole can of worms here that needs to be
	 * opened to decide what is the valid maximum size of a log in
	 * an AG.
	 */
	max_logblocks = libxfs_alloc_ag_max_usable(mp) - 1;
	if (max_logblocks < min_logblocks) {
		fprintf(stderr,
_("max log size %d smaller than min log size %d, filesystem is too small\n"),
				max_logblocks,
				min_logblocks);
		usage();
	}

	/* internal log - if no size specified, calculate automatically */
	if (!cfg->logblocks) {
		/* Use a 2048:1 fs:log ratio for most filesystems */
		cfg->logblocks = (cfg->dblocks << cfg->blocklog) / 2048;
		cfg->logblocks = cfg->logblocks >> cfg->blocklog;

		if (cli->log_concurrency != 0)
			cfg->logblocks = calc_concurrency_logblocks(cfg, cli,
							xi, max_tx_bytes);

		/* But don't go below a reasonable size */
		cfg->logblocks = max(cfg->logblocks,
				XFS_MIN_REALISTIC_LOG_BLOCKS(cfg->blocklog));

		/* And for a tiny filesystem, use the absolute minimum size */
		if (cfg->dblocks < MEGABYTES(300, cfg->blocklog))
			cfg->logblocks = min_logblocks;

		/* Ensure the chosen size fits within log size requirements */
		cfg->logblocks = max(min_logblocks, cfg->logblocks);
		cfg->logblocks = min(cfg->logblocks, max_logblocks);

		/* and now clamp the size to the maximum supported size */
		cfg->logblocks = min(cfg->logblocks, XFS_MAX_LOG_BLOCKS);
		if ((cfg->logblocks << cfg->blocklog) > XFS_MAX_LOG_BYTES)
			cfg->logblocks = XFS_MAX_LOG_BYTES >> cfg->blocklog;

		validate_log_size(cfg->logblocks, cfg->blocklog, min_logblocks);
	} else if (cfg->logblocks > max_logblocks) {
		/* check specified log size */
		fprintf(stderr,
_("internal log size %lld too large, must be less than %d\n"),
			(long long)cfg->logblocks,
			max_logblocks);
		usage();
	}

	if (cfg->logblocks > sbp->sb_agblocks - libxfs_prealloc_blocks(mp)) {
		fprintf(stderr,
_("internal log size %lld too large, must fit in allocation group\n"),
			(long long)cfg->logblocks);
		usage();
	}

	if (cli_opt_set(&lopts, L_AGNUM)) {
		if (cli->logagno >= sbp->sb_agcount) {
			fprintf(stderr,
_("log ag number %lld too large, must be less than %lld\n"),
				(long long)cli->logagno,
				(long long)sbp->sb_agcount);
			usage();
		}
		cfg->logagno = cli->logagno;
	} else
		cfg->logagno = (xfs_agnumber_t)(sbp->sb_agcount / 2);

	if (cfg->logagno == 0)
		adjust_ag0_internal_logblocks(cfg, mp, min_logblocks,
				&max_logblocks);

	cfg->logstart = XFS_AGB_TO_FSB(mp, cfg->logagno,
				       libxfs_prealloc_blocks(mp));

	/*
	 * Align the logstart at stripe unit boundary.
	 */
	if (cfg->lsunit) {
		align_internal_log(cfg, mp, cfg->lsunit, max_logblocks);
	} else if (cfg->dsunit) {
		align_internal_log(cfg, mp, cfg->dsunit, max_logblocks);
	}
	validate_log_size(cfg->logblocks, cfg->blocklog, min_logblocks);
}

/*
 * Set up superblock with the minimum parameters required for
 * the libxfs macros needed by the log sizing code to run successfully.
 * This includes a minimum log size calculation, so we need everything
 * that goes into that calculation to be setup here including feature
 * flags.
 */
static void
start_superblock_setup(
	struct mkfs_params	*cfg,
	struct xfs_mount	*mp,
	struct xfs_sb		*sbp)
{
	sbp->sb_magicnum = XFS_SB_MAGIC;
	sbp->sb_sectsize = (uint16_t)cfg->sectorsize;
	sbp->sb_sectlog = (uint8_t)cfg->sectorlog;
	sbp->sb_blocksize = cfg->blocksize;
	sbp->sb_blocklog = (uint8_t)cfg->blocklog;

	sbp->sb_agblocks = (xfs_agblock_t)cfg->agsize;
	sbp->sb_agblklog = (uint8_t)log2_roundup(cfg->agsize);
	sbp->sb_agcount = (xfs_agnumber_t)cfg->agcount;
	sbp->sb_dblocks = (xfs_rfsblock_t)cfg->dblocks;

	sbp->sb_inodesize = (uint16_t)cfg->inodesize;
	sbp->sb_inodelog = (uint8_t)cfg->inodelog;
	sbp->sb_inopblock = (uint16_t)(cfg->blocksize / cfg->inodesize);
	sbp->sb_inopblog = (uint8_t)(cfg->blocklog - cfg->inodelog);

	sbp->sb_dirblklog = cfg->dirblocklog - cfg->blocklog;

	sb_set_features(cfg, sbp);

	/*
	 * log stripe unit is stored in bytes on disk and cannot be zero
	 * for v2 logs.
	 */
	if (cfg->sb_feat.log_version == 2) {
		if (cfg->lsunit)
			sbp->sb_logsunit = XFS_FSB_TO_B(mp, cfg->lsunit);
		else
			sbp->sb_logsunit = 1;
	} else
		sbp->sb_logsunit = 0;

	/* log reservation calculations depend on rt geometry */
	sbp->sb_rblocks = cfg->rtblocks;
	sbp->sb_rextsize = cfg->rtextblocks;
	mp->m_features |= libxfs_sb_version_to_features(sbp);
	libxfs_sb_mount_rextsize(mp, sbp);
}

static void
initialise_mount(
	struct xfs_mount	*mp,
	struct xfs_sb		*sbp)
{
	/* Minimum needed for libxfs_prealloc_blocks() */
	mp->m_blkbb_log = sbp->sb_blocklog - BBSHIFT;
	mp->m_sectbb_log = sbp->sb_sectlog - BBSHIFT;
}

/*
 * Format everything from the generated config into the superblock that
 * will be used to initialise the on-disk superblock. This is the in-memory
 * copy, so no need to care about endian swapping here.
 */
static void
finish_superblock_setup(
	struct mkfs_params	*cfg,
	struct xfs_mount	*mp,
	struct xfs_sb		*sbp)
{
	if (cfg->label) {
		size_t		label_len;

		/*
		 * Labels are null terminated unless the string fits exactly
		 * in the label field, so assume sb_fname is zeroed and then
		 * do a memcpy because the destination isn't a normal C string.
		 */
		label_len = min(sizeof(sbp->sb_fname), strlen(cfg->label));
		memcpy(sbp->sb_fname, cfg->label, label_len);
	}

	sbp->sb_dblocks = cfg->dblocks;
	sbp->sb_rextents = cfg->rtextents;
	platform_uuid_copy(&sbp->sb_uuid, &cfg->uuid);
	/* Only in memory; libxfs expects this as if read from disk */
	platform_uuid_copy(&sbp->sb_meta_uuid, &cfg->uuid);
	sbp->sb_logstart = cfg->logstart;
	sbp->sb_rootino = sbp->sb_rbmino = sbp->sb_rsumino = NULLFSINO;
	sbp->sb_metadirino = NULLFSINO;
	sbp->sb_agcount = (xfs_agnumber_t)cfg->agcount;
	sbp->sb_rbmblocks = cfg->rtbmblocks;
	sbp->sb_logblocks = (xfs_extlen_t)cfg->logblocks;
	sbp->sb_rextslog = libxfs_compute_rextslog(cfg->rtextents);
	sbp->sb_inprogress = 1;	/* mkfs is in progress */
	sbp->sb_imax_pct = cfg->imaxpct;
	sbp->sb_icount = 0;
	sbp->sb_ifree = 0;
	sbp->sb_fdblocks = cfg->dblocks -
			   cfg->agcount * libxfs_prealloc_blocks(mp) -
			   (cfg->loginternal ? cfg->logblocks : 0);
	sbp->sb_frextents = 0;	/* will do a free later */
	sbp->sb_uquotino = sbp->sb_gquotino = sbp->sb_pquotino = 0;
	sbp->sb_qflags = cfg->sb_feat.qflags;
	sbp->sb_unit = cfg->dsunit;
	sbp->sb_width = cfg->dswidth;
	mp->m_features |= libxfs_sb_version_to_features(sbp);
	libxfs_sb_mount_rextsize(mp, sbp);
}

/* Prepare an uncached buffer, ready to write something out. */
static inline struct xfs_buf *
alloc_write_buf(
	struct xfs_buftarg	*btp,
	xfs_daddr_t		daddr,
	int			bblen)
{
	struct xfs_buf		*bp;
	int			error;

	error = -libxfs_buf_get_uncached(btp, bblen, &bp);
	if (error) {
		fprintf(stderr, _("Could not get memory for buffer, err=%d\n"),
				error);
		exit(1);
	}

	xfs_buf_set_daddr(bp, daddr);
	return bp;
}

/*
 * Sanitise the data and log devices and prepare them so libxfs can mount the
 * device successfully. Also check we can access the rt device if configured.
 */
static void
prepare_devices(
	struct mkfs_params	*cfg,
	struct libxfs_init	*xi,
	struct xfs_mount	*mp,
	struct xfs_sb		*sbp,
	bool			clear_stale)
{
	struct xfs_buf		*buf;
	int			whack_blks = BTOBB(WHACK_SIZE);
	int			lsunit;

	/*
	 * If there's an old XFS filesystem on the device with enough intact
	 * information that we can parse the superblock, there's enough
	 * information on disk to confuse a future xfs_repair call. To avoid
	 * this, whack all the old secondary superblocks that we can find.
	 */
	if (clear_stale)
		zero_old_xfs_structures(xi, sbp);

	/*
	 * If the data device is a file, grow out the file to its final size if
	 * needed so that the reads for the end of the device in the mount code
	 * will succeed.
	 */
	if (xi->data.isfile &&
	    xi->data.size * xi->data.bsize < cfg->dblocks * cfg->blocksize) {
		if (ftruncate(xi->data.fd, cfg->dblocks * cfg->blocksize) < 0) {
			fprintf(stderr,
				_("%s: Growing the data section failed\n"),
				progname);
			exit(1);
		}

		/* update size to be able to whack blocks correctly */
		xi->data.size = BTOBB(cfg->dblocks * cfg->blocksize);
	}

	/*
	 * Zero out the end to obliterate any old MD RAID (or other) metadata at
	 * the end of the device.  (MD sb is ~64k from the end, take out a wider
	 * swath to be sure)
	 */
	buf = alloc_write_buf(mp->m_ddev_targp, (xi->data.size - whack_blks),
			whack_blks);
	memset(buf->b_addr, 0, WHACK_SIZE);
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);

	/*
	 * Now zero out the beginning of the device, to obliterate any old
	 * filesystem signatures out there.  This should take care of
	 * swap (somewhere around the page size), jfs (32k),
	 * ext[2,3] and reiserfs (64k) - and hopefully all else.
	 */
	buf = alloc_write_buf(mp->m_ddev_targp, 0, whack_blks);
	memset(buf->b_addr, 0, WHACK_SIZE);
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);

	/* OK, now write the superblock... */
	buf = alloc_write_buf(mp->m_ddev_targp, XFS_SB_DADDR,
			XFS_FSS_TO_BB(mp, 1));
	buf->b_ops = &xfs_sb_buf_ops;
	memset(buf->b_addr, 0, cfg->sectorsize);
	libxfs_sb_to_disk(buf->b_addr, sbp);
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);

	/* ...and zero the log.... */
	lsunit = sbp->sb_logsunit;
	if (lsunit == 1)
		lsunit = sbp->sb_logsectsize;

	libxfs_log_clear(mp->m_logdev_targp, NULL,
			 XFS_FSB_TO_DADDR(mp, cfg->logstart),
			 (xfs_extlen_t)XFS_FSB_TO_BB(mp, cfg->logblocks),
			 &sbp->sb_uuid, cfg->sb_feat.log_version,
			 lsunit, XLOG_FMT, XLOG_INIT_CYCLE, false);
	/* finally, check we can write the last block in the realtime area */
	if (mp->m_rtdev_targp->bt_bdev &&
	    mp->m_rtdev_targp != mp->m_ddev_targp &&
	    cfg->rtblocks > 0 &&
	    !xfs_has_zoned(mp)) {
		buf = alloc_write_buf(mp->m_rtdev_targp,
				XFS_FSB_TO_BB(mp, cfg->rtblocks - 1LL),
				BTOBB(cfg->blocksize));
		memset(buf->b_addr, 0, cfg->blocksize);
		libxfs_buf_mark_dirty(buf);
		libxfs_buf_relse(buf);
	}

}

static void
initialise_ag_headers(
	struct mkfs_params	*cfg,
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	int			*worst_freelist,
	struct list_head	*buffer_list)
{
	struct aghdr_init_data	id = {
		.agno		= agno,
		.agsize		= cfg->agsize,
	};
	struct xfs_perag	*pag = libxfs_perag_get(mp, agno);
	int			error;

	if (agno == cfg->agcount - 1)
		id.agsize = cfg->dblocks - (xfs_rfsblock_t)(agno * cfg->agsize);

	INIT_LIST_HEAD(&id.buffer_list);
	error = -libxfs_ag_init_headers(mp, &id);
	if (error) {
		fprintf(stderr, _("AG header init failed, error %d\n"), error);
		exit(1);
	}

	list_splice_tail_init(&id.buffer_list, buffer_list);

	if (libxfs_alloc_min_freelist(mp, pag) > *worst_freelist)
		*worst_freelist = libxfs_alloc_min_freelist(mp, pag);
	libxfs_perag_put(pag);
}

static void
initialise_ag_freespace(
	struct xfs_mount	*mp,
	xfs_agnumber_t		agno,
	int			worst_freelist)
{
	struct xfs_alloc_arg	args;
	struct xfs_trans	*tp;
	int			c;

	c = -libxfs_trans_alloc_rollable(mp, worst_freelist, &tp);
	if (c)
		res_failed(c);

	memset(&args, 0, sizeof(args));
	args.tp = tp;
	args.mp = mp;
	args.agno = agno;
	args.alignment = 1;
	args.pag = libxfs_perag_get(mp, agno);

	libxfs_alloc_fix_freelist(&args, 0);
	libxfs_perag_put(args.pag);
	c = -libxfs_trans_commit(tp);
	if (c) {
		errno = c;
		perror(_("initializing AG free space list"));
		exit(1);
	}
}

/*
 * rewrite several secondary superblocks with the root inode number filled out.
 * This can help repair recovery from a trashed primary superblock without
 * losing the root inode.
 */
static void
rewrite_secondary_superblocks(
	struct xfs_mount	*mp)
{
	struct xfs_buf		*buf;
	struct xfs_dsb		*dsb;
	int			error;

	/* rewrite the last superblock */
	error = -libxfs_buf_read(mp->m_dev,
			XFS_AGB_TO_DADDR(mp, mp->m_sb.sb_agcount - 1,
				XFS_SB_DADDR),
			XFS_FSS_TO_BB(mp, 1), 0, &buf, &xfs_sb_buf_ops);
	if (error) {
		fprintf(stderr, _("%s: could not re-read AG %u superblock\n"),
				progname, mp->m_sb.sb_agcount - 1);
		exit(1);
	}
	dsb = buf->b_addr;
	dsb->sb_rootino = cpu_to_be64(mp->m_sb.sb_rootino);
	if (xfs_has_metadir(mp))
		dsb->sb_metadirino = cpu_to_be64(mp->m_sb.sb_metadirino);
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);

	/* and one in the middle for luck if there's enough AGs for that */
	if (mp->m_sb.sb_agcount <= 2)
		return;

	error = -libxfs_buf_read(mp->m_dev,
			XFS_AGB_TO_DADDR(mp, (mp->m_sb.sb_agcount - 1) / 2,
				XFS_SB_DADDR),
			XFS_FSS_TO_BB(mp, 1), 0, &buf, &xfs_sb_buf_ops);
	if (error) {
		fprintf(stderr, _("%s: could not re-read AG %u superblock\n"),
				progname, (mp->m_sb.sb_agcount - 1) / 2);
		exit(1);
	}
	dsb = buf->b_addr;
	dsb->sb_rootino = cpu_to_be64(mp->m_sb.sb_rootino);
	if (xfs_has_metadir(mp))
		dsb->sb_metadirino = cpu_to_be64(mp->m_sb.sb_metadirino);
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);
}

static void
check_root_ino(
	struct xfs_mount	*mp)
{
	xfs_ino_t		ino;

	if (XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino) != 0) {
		fprintf(stderr,
			_("%s: root inode created in AG %u, not AG 0\n"),
			progname, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino));
		exit(1);
	}

	/*
	 * The superblock points to the root directory inode, but xfs_repair
	 * expects to find the root inode in a very specific location computed
	 * from the filesystem geometry for an extra level of verification.
	 *
	 * Fail the format immediately if those assumptions ever break, because
	 * repair will toss the root directory.
	 */
	ino = libxfs_ialloc_calc_rootino(mp, mp->m_sb.sb_unit);
	if (mp->m_sb.sb_rootino != ino) {
		fprintf(stderr,
	_("%s: root inode (%llu) not allocated in expected location (%llu)\n"),
			progname,
			(unsigned long long)mp->m_sb.sb_rootino,
			(unsigned long long)ino);
		exit(1);
	}
}

/*
 * INI file format option parser.
 *
 * This is called by the file parser library for every valid option it finds in
 * the config file. The option is already broken down into a
 * {section,name,value} tuple, so all we need to do is feed it to the correct
 * suboption parser function and translate the return value.
 *
 * Returns 0 on failure, 1 for success.
 */
static int
cfgfile_parse_ini(
	void			*user,
	const char		*section,
	const char		*name,
	const char		*value)
{
	struct cli_params	*cli = user;

	if (!parse_cfgopt(section, name, value, cli))
		return 0;
	return 1;
}

static void
cfgfile_parse(
	struct cli_params	*cli)
{
	int			error;

	if (!cli->cfgfile)
		return;

	error = ini_parse(cli->cfgfile, cfgfile_parse_ini, cli);
	if (error) {
		if (error > 0) {
			fprintf(stderr,
		_("%s: Unrecognised input on line %d. Aborting.\n"),
				cli->cfgfile, error);
		} else if (error == -1) {
			fprintf(stderr,
		_("Unable to open config file %s. Aborting.\n"),
				cli->cfgfile);
		} else if (error == -2) {
			fprintf(stderr,
		_("Memory allocation failure parsing %s. Aborting.\n"),
				cli->cfgfile);
		} else {
			fprintf(stderr,
		_("Unknown error %d opening config file %s. Aborting.\n"),
				error, cli->cfgfile);
		}
		exit(1);
	}
	printf(_("Parameters parsed from config file %s successfully\n"),
		cli->cfgfile);
}

static void
set_autofsck(
	struct xfs_mount	*mp,
	struct cli_params	*cli)
{
	struct xfs_da_args	args = {
		.geo		= mp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
		.attr_filter	= LIBXFS_ATTR_ROOT,
		.owner		= mp->m_sb.sb_rootino,
	};
	const char		*word;
	char			*p;
	int			error;

	error = fsprop_name_to_attr_name(FSPROP_AUTOFSCK_NAME, &p);
	if (error < 0) {
		fprintf(stderr,
 _("%s: error %d while allocating fs property name\n"),
				progname, error);
		exit(1);
	}
	args.namelen = error;
	args.name = (const uint8_t *)p;

	word = fsprop_autofsck_write(cli->autofsck);
	if (!word) {
		fprintf(stderr,
 _("%s: not sure what to do with autofsck value %u\n"),
				progname, cli->autofsck);
		exit(1);
	}
	args.value = (void *)word;
	args.valuelen = strlen(word);

	error = -libxfs_iget(mp, NULL, mp->m_sb.sb_rootino, 0, &args.dp);
	if (error) {
		fprintf(stderr,
 _("%s: error %d while opening root directory\n"),
				progname, error);
		exit(1);
	}

	libxfs_attr_sethash(&args);

	error = -libxfs_attr_set(&args, XFS_ATTRUPDATE_UPSERT, false);
	if (error) {
		fprintf(stderr,
 _("%s: error %d while setting autofsck property\n"),
				progname, error);
		exit(1);
	}

	libxfs_irele(args.dp);
}

/* Write the realtime superblock */
static void
write_rtsb(
	struct xfs_mount	*mp)
{
	struct xfs_buf		*rtsb_bp;
	struct xfs_buf		*sb_bp = libxfs_getsb(mp);
	int			error;

	if (!sb_bp) {
		fprintf(stderr,
  _("%s: couldn't grab primary superblock buffer\n"), progname);
		exit(1);
	}

	error = -libxfs_buf_get_uncached(mp->m_rtdev_targp,
				XFS_FSB_TO_BB(mp, 1), &rtsb_bp);
	if (error) {
		fprintf(stderr,
 _("%s: couldn't grab realtime superblock buffer\n"), progname);
			exit(1);
	}

	rtsb_bp->b_maps[0].bm_bn = XFS_RTSB_DADDR;
	rtsb_bp->b_ops = &xfs_rtsb_buf_ops;

	libxfs_update_rtsb(rtsb_bp, sb_bp);
	libxfs_buf_mark_dirty(rtsb_bp);
	libxfs_buf_relse(rtsb_bp);
	libxfs_buf_relse(sb_bp);
}

static inline void
prealloc_fail(
	struct xfs_mount	*mp,
	int			error,
	xfs_filblks_t		ask,
	const char		*tag)
{
	if (error == ENOSPC)
		fprintf(stderr,
	_("%s: cannot handle expansion of %s; need %llu free blocks, have %llu\n"),
				progname, tag, (unsigned long long)ask,
				(unsigned long long)mp->m_sb.sb_fdblocks);
	else
		fprintf(stderr,
	_("%s: error %d while checking free space for %s\n"),
				progname, error, tag);
	exit(1);
}

/*
 * Make sure there's enough space on the data device to handle realtime
 * metadata btree expansions.
 */
static void
check_rt_meta_prealloc(
	struct xfs_mount	*mp)
{
	struct xfs_perag	*pag = NULL;
	int			error;

	/*
	 * First create all the per-AG reservations, since they take from the
	 * free block count.  Each AG should start with enough free space for
	 * the per-AG reservation.
	 */
	mp->m_finobt_nores = false;

	while ((pag = xfs_perag_next(mp, pag))) {
		error = -libxfs_ag_resv_init(pag, NULL);
		if (error && error != ENOSPC) {
			fprintf(stderr,
	_("%s: error %d while checking AG free space for realtime metadata\n"),
					progname, error);
			exit(1);
		}
	}

	error = -libxfs_metafile_resv_init(mp);
	if (error)
		prealloc_fail(mp, error, 0, _("metadata files"));

	libxfs_metafile_resv_free(mp);

	while ((pag = xfs_perag_next(mp, pag)))
		libxfs_ag_resv_free(pag);

	mp->m_finobt_nores = false;
}

int
main(
	int			argc,
	char			**argv)
{
	xfs_agnumber_t		agno;
	struct xfs_buf		*buf;
	int			c;
	int			dry_run = 0;
	int			discard = 1;
	int			force_overwrite = 0;
	int			quiet = 0;
	char			*protostring = NULL;
	int			worst_freelist = 0;

	struct libxfs_init	xi = {
		.flags = LIBXFS_EXCLUSIVELY | LIBXFS_DIRECT,
	};
	struct xfs_mount	mbuf = {};
	struct xfs_mount	*mp = &mbuf;
	struct xfs_sb		*sbp = &mp->m_sb;
	struct xfs_dsb		*dsb;
	struct fs_topology	ft = {};
	struct cli_params	cli = {
		.xi = &xi,
		.loginternal = 1,
		.is_supported	= 1,
		.data_concurrency = -1, /* auto detect non-mechanical storage */
		.log_concurrency = -1, /* auto detect non-mechanical ddev */
		.rtvol_concurrency = -1, /* auto detect non-mechanical rtdev */
		.autofsck = FSPROP_AUTOFSCK_UNSET,
		.imaxpct = -1, /* set sb_imax_pct automatically */
	};
	struct mkfs_params	cfg = {};

	struct option		long_options[] = {
	{
		.name		= "unsupported",
		.has_arg	= no_argument,
		.flag		= &cli.is_supported,
		.val		= 0,
	},
	{NULL, 0, NULL, 0 },
	};
	int			option_index = 0;

	/* build time defaults */
	struct mkfs_default_params	dft = {
		.source = _("package build definitions"),
		.sectorsize = XFS_MIN_SECTORSIZE,
		.blocksize = 1 << XFS_DFL_BLOCKSIZE_LOG,
		.sb_feat = {
			.log_version = 2,
			.attr_version = 2,
			.dir_version = 2,
			.inode_align = true,
			.nci = false,
			.lazy_sb_counters = true,
			.projid32bit = true,
			.crcs_enabled = true,
			.dirftype = true,
			.finobt = true,
			.spinodes = true,
			.rmapbt = true,
			.reflink = true,
			.inobtcnt = true,
			.parent_pointers = false,
			.nodalign = false,
			.nortalign = false,
			.bigtime = true,
			.nrext64 = true,
			/*
			 * When we decide to enable a new feature by default,
			 * please remember to update the mkfs conf files.
			 */
		},
	};
	struct zone_topology zt = {};
	struct list_head	buffer_list;
	int			error;

	platform_uuid_generate(&cli.uuid);
	progname = basename(argv[0]);
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	/*
	 * TODO: Sourcing defaults from a config file
	 *
	 * Before anything else, see if there's a config file with different
	 * defaults. If a file exists in <package location>, read in the new
	 * default values and overwrite them in the &dft structure. This way the
	 * new defaults will apply before we parse the CLI, and the CLI will
	 * still be able to override them. When more than one source is
	 * implemented, emit a message to indicate where the defaults being
	 * used came from.
	 *
	 * printf(_("Default configuration sourced from %s\n"), dft.source);
	 */

	/* copy new defaults into CLI parsing structure */
	memcpy(&cli.sb_feat, &dft.sb_feat, sizeof(cli.sb_feat));
	memcpy(&cli.fsx, &dft.fsx, sizeof(cli.fsx));

	while ((c = getopt_long(argc, argv, "b:c:d:i:l:L:m:n:KNp:qr:s:CfV",
					long_options, &option_index)) != EOF) {
		switch (c) {
		case 0:
			break;
		case 'C':
		case 'f':
			force_overwrite = 1;
			break;
		case 'b':
		case 'c':
		case 'd':
		case 'i':
		case 'l':
		case 'm':
		case 'n':
		case 'p':
		case 'r':
		case 's':
			parse_subopts(c, optarg, &cli);
			break;
		case 'L':
			if (strlen(optarg) > sizeof(sbp->sb_fname))
				illegal(optarg, "L");
			cfg.label = optarg;
			break;
		case 'N':
			dry_run = 1;
			break;
		case 'K':
			discard = 0;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'V':
			printf(_("%s version %s\n"), progname, VERSION);
			exit(0);
		default:
			unknown(optopt, "");
		}
	}
	if (argc - optind > 1) {
		fprintf(stderr, _("extra arguments\n"));
		usage();
	} else if (argc - optind == 1) {
		xi.data.name = getstr(argv[optind], &dopts, D_NAME);
	}

	/*
	 * Now we have all the options parsed, we can read in the option file
	 * specified on the command line via "-c options=xxx". Once we have all
	 * the options from this file parsed, we can then proceed with parameter
	 * and bounds checking and making the filesystem.
	 */
	cfgfile_parse(&cli);

	protostring = setup_proto(cli.protofile);

	/*
	 * Extract as much of the valid config as we can from the CLI input
	 * before opening the libxfs devices.
	 */
	validate_blocksize(&cfg, &cli, &dft);
	validate_sectorsize(&cfg, &cli, &dft, &ft, dry_run, force_overwrite);

	/*
	 * XXX: we still need to set block size and sector size global variables
	 * so that getnum/cvtnum works correctly
	 */
	blocksize = cfg.blocksize;
	sectorsize = cfg.sectorsize;

	validate_log_sectorsize(&cfg, &cli, &dft, &ft);
	validate_zoned(&cfg, &cli, &dft, &zt);
	validate_sb_features(&cfg, &cli);

	/*
	 * we've now completed basic validation of the features, sector and
	 * block sizes, so from this point onwards we use the values found in
	 * the cfg structure for them, not the command line structure.
	 */
	validate_dirblocksize(&cfg, &cli);
	validate_inodesize(&cfg, &cli);

	/*
	 * if the device size was specified convert it to a block count
	 * now we have a valid block size. These will be set to zero if
	 * nothing was specified, indicating we should use the full device.
	 */
	cfg.dblocks = calc_dev_size(cli.dsize, &cfg, &dopts, D_SIZE, "data");
	cfg.logblocks = calc_dev_size(cli.logsize, &cfg, &lopts, L_SIZE, "log");
	cfg.rtblocks = calc_dev_size(cli.rtsize, &cfg, &ropts, R_SIZE, "rt");

	validate_rtextsize(&cfg, &cli, &ft);

	/*
	 * Open and validate the device configurations
	 */
	open_devices(&cfg, &xi, &zt);
	validate_overwrite(xi.data.name, force_overwrite);
	validate_datadev(&cfg, &cli);
	validate_logdev(&cfg, &cli);
	validate_rtdev(&cfg, &cli, &zt);
	calc_stripe_factors(&cfg, &cli, &ft);

	/*
	 * At this point when know exactly what size all the devices are,
	 * so we can start validating and calculating layout options that are
	 * dependent on device sizes. Once calculated, make sure everything
	 * aligns to device geometry correctly.
	 */
	calculate_initial_ag_geometry(&cfg, &cli, &xi);
	align_ag_geometry(&cfg);
	if (cfg.sb_feat.zoned)
		calculate_zone_geometry(&cfg, &cli, &xi, &zt);
	else
		calculate_rtgroup_geometry(&cfg, &cli, &xi);

	calculate_imaxpct(&cfg, &cli);

	/*
	 * Set up the basic superblock parameters now so that we can use
	 * the geometry information we've already validated in libxfs
	 * provided functions to determine on-disk format information.
	 */
	start_superblock_setup(&cfg, mp, sbp);
	initialise_mount(mp, sbp);

	/*
	 * With the mount set up, we can finally calculate the log size
	 * constraints and do default size calculations and final validation
	 */
	calculate_log_size(&cfg, &cli, &xi, mp);

	finish_superblock_setup(&cfg, mp, sbp);

	/* Validate the extent size hints now that @mp is fully set up. */
	validate_extsize_hint(mp, &cli);
	validate_cowextsize_hint(mp, &cli);

	validate_supported(mp, &cli);

	/* Print the intended geometry of the fs. */
	if (!quiet || dry_run) {
		struct xfs_fsop_geom	geo;

		libxfs_fs_geometry(mp, &geo, XFS_FS_GEOM_MAX_STRUCT_VER);
		xfs_report_geom(&geo, xi.data.name, xi.log.name, xi.rt.name);
		if (dry_run)
			exit(0);
	}

	/* Make sure our checksum algorithm really works. */
	if (crc32c_test(CRC32CTEST_QUIET) != 0) {
		fprintf(stderr,
 _("crc32c self-test failed, will not create a filesystem here.\n"));
		return 1;
	}

	/* Make sure our dir/attr hash algorithm really works. */
	if (dahash_test(DAHASHTEST_QUIET) != 0) {
		fprintf(stderr,
 _("xfs dir/attr self-test failed, will not create a filesystem here.\n"));
		return 1;
	}

	/*
	 * All values have been validated, discard the old device layout.
	 */
	if (cli.sb_feat.zoned && !discard) {
		fprintf(stderr,
 _("-K not support for zoned file systems.\n"));
		return 1;
	}
	if (discard && !dry_run)
		discard_devices(&cfg, &xi, &zt, quiet);

	/*
	 * we need the libxfs buffer cache from here on in.
	 */
	libxfs_buftarg_init(mp, &xi);

	/*
	 * Before we mount the filesystem we need to make sure the devices have
	 * enough of the filesystem structure on them that allows libxfs to
	 * mount.
	 */
	prepare_devices(&cfg, &xi, mp, sbp, force_overwrite);
	mp = libxfs_mount(mp, sbp, &xi, 0);
	if (mp == NULL) {
		fprintf(stderr, _("%s: filesystem failed to initialize\n"),
			progname);
		exit(1);
	}

	/*
	 * Initialise all the static on disk metadata.
	 */
	INIT_LIST_HEAD(&buffer_list);
	for (agno = 0; agno < cfg.agcount; agno++) {
		initialise_ag_headers(&cfg, mp, agno, &worst_freelist,
				&buffer_list);

		if (agno % 16)
			continue;

		error = -libxfs_buf_delwri_submit(&buffer_list);
		if (error) {
			fprintf(stderr,
	_("%s: writing AG headers failed, err=%d\n"),
					progname, error);
			exit(1);
		}
	}

	error = -libxfs_buf_delwri_submit(&buffer_list);
	if (error) {
		fprintf(stderr, _("%s: writing AG headers failed, err=%d\n"),
				progname, error);
		exit(1);
	}

	if (xfs_has_rtsb(mp) && cfg.rtblocks > 0)
		write_rtsb(mp);

	/*
	 * Initialise the freespace freelists (i.e. AGFLs) in each AG.
	 */
	for (agno = 0; agno < cfg.agcount; agno++)
		initialise_ag_freespace(mp, agno, worst_freelist);

	/*
	 * Allocate the root inode and anything else in the proto file.
	 */
	parse_proto(mp, &cli.fsx, &protostring, cli.proto_slashes_are_spaces);

	/*
	 * Protect ourselves against possible stupidity
	 */
	check_root_ino(mp);

	/* Make sure we can handle space preallocations of rt metadata btrees */
	check_rt_meta_prealloc(mp);

	/*
	 * Re-write multiple secondary superblocks with rootinode field set
	 */
	if (mp->m_sb.sb_agcount > 1)
		rewrite_secondary_superblocks(mp);

	if (cli.autofsck != FSPROP_AUTOFSCK_UNSET)
		set_autofsck(mp, &cli);

	/*
	 * Dump all inodes and buffers before marking us all done.
	 * Need to drop references to inodes we still hold, first.
	 */
	libxfs_rtmount_destroy(mp);
	libxfs_bcache_purge(mp);

	/*
	 * Mark the filesystem ok.
	 */
	buf = libxfs_getsb(mp);
	if (!buf || buf->b_error)
		exit(1);
	dsb = buf->b_addr;
	dsb->sb_inprogress = 0;
	libxfs_buf_mark_dirty(buf);
	libxfs_buf_relse(buf);

	/* Exit w/ failure if anything failed to get written to our new fs. */
	error = -libxfs_umount(mp);
	if (error)
		exit(1);

	libxfs_destroy(&xi);
	return 0;
}
