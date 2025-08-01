// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

/*
 * This header is effectively a "namespace multiplexor" for the
 * user level XFS code.  It provides all of the necessary stuff
 * such that we can build some parts of the XFS kernel code in
 * user space in a controlled fashion, and translates the names
 * used in the kernel into the names which libxfs is going to
 * make available to user tools.
 *
 * It should only ever be #include'd by XFS "kernel" code being
 * compiled in user space.
 *
 * Our goals here are to...
 *      o  "share" large amounts of complex code between user and
 *         kernel space;
 *      o  shield the user tools from changes in the bleeding
 *         edge kernel code, merging source changes when
 *         convenient and not immediately (no symlinks);
 *      o  i.e. be able to merge changes to the kernel source back
 *         into the affected user tools in a controlled fashion;
 *      o  provide a _minimalist_ life-support system for kernel
 *         code in user land, not the "everything + the kitchen
 *         sink" model which libsim had mutated into;
 *      o  allow the kernel code to be completely free of code
 *         specifically there to support the user level build.
 */

/*
 * define a guard and something we can check to determine what include context
 * we are running from.
 */
#ifndef __LIBXFS_INTERNAL_XFS_H__
#define __LIBXFS_INTERNAL_XFS_H__

/* CONFIG_XFS_* must be defined to 1 to work with IS_ENABLED() */
#define CONFIG_XFS_RT 1
#define CONFIG_XFS_BTREE_IN_MEM 1

#include "libxfs_api_defs.h"
#include "platform_defs.h"
#include "xfs.h"

#include "list.h"
#include "hlist.h"
#include "cache.h"
#include "bitops.h"
#include "kmem.h"
#include "libfrog/radix-tree.h"
#include "libfrog/bitmask.h"
#include "libfrog/div64.h"
#include "libfrog/util.h"
#include "atomic.h"
#include "spinlock.h"
#include "linux-err.h"

#include "xfs_types.h"
#include "xfs_arch.h"

#include "xfs_fs.h"
#include "libfrog/crc32c.h"

#include <sys/xattr.h>
#ifdef HAVE_GETRANDOM_NONBLOCK
#include <sys/random.h>
#endif

/* Zones used in libxfs allocations that aren't in shared header files */
extern struct kmem_cache *xfs_buf_item_cache;
extern struct kmem_cache *xfs_ili_cache;
extern struct kmem_cache *xfs_buf_cache;
extern struct kmem_cache *xfs_inode_cache;
extern struct kmem_cache *xfs_trans_cache;

/* fake up iomap, (not) used in xfs_bmap.[ch] */
#define IOMAP_F_SHARED				0x04
#define xfs_bmbt_to_iomap(a, b, c, d, e, f)	((void) 0)

/* CRC stuff, buffer API dependent on it */
#define crc32c(c,p,l)	crc32c_le((c),(unsigned char const *)(p),(l))

/* fake up kernel's iomap, (not) used in xfs_bmap.[ch] */
struct iomap {
	unsigned long long	offset;	/* do not use */
	unsigned long long	length;	/* do not use */
};

#define cancel_delayed_work_sync(work) do { } while(0)

#include "xfs_cksum.h"

/*
 * This mirrors the kernel include for xfs_buf.h - it's implicitly included in
 * every files via a similar include in the kernel xfs_linux.h.
 */
#include "libxfs_io.h"

/* for all the support code that uses progname in error messages */
extern char    *progname;

#undef ASSERT
#define ASSERT(ex) assert(ex)

/*
 * We have no need for the "linux" dev_t in userspace, so these
 * are no-ops, and an xfs_dev_t is stored in VFS_I(ip)->i_rdev
 */
#define xfs_to_linux_dev_t(dev)	dev
#define linux_to_xfs_dev_t(dev) dev

#ifndef EWRONGFS
#define EWRONGFS	EINVAL
#endif

#define xfs_error_level			0

#define STATIC				static

/*
 * Starting in Linux 4.15, the %p (raw pointer value) printk modifier
 * prints a hashed version of the pointer to avoid leaking kernel
 * pointers into dmesg.  If we're trying to debug the kernel we want the
 * raw values, so override this behavior as best we can.
 *
 * In userspace we don't have this problem.
 */
#define PTR_FMT "%p"

#define XFS_IGET_CREATE			0x1
#define XFS_IGET_UNTRUSTED		0x2

extern void cmn_err(int, char *, ...);
enum ce { CE_DEBUG, CE_CONT, CE_NOTE, CE_WARN, CE_ALERT, CE_PANIC };

#define xfs_info(mp,fmt,args...)	cmn_err(CE_CONT, _(fmt), ## args)
#define xfs_info_ratelimited(mp,fmt,args...) cmn_err(CE_CONT, _(fmt), ## args)
#define xfs_notice(mp,fmt,args...)	cmn_err(CE_NOTE, _(fmt), ## args)
#define xfs_warn(mp,fmt,args...)	cmn_err((mp) ? CE_WARN : CE_WARN, _(fmt), ## args)
#define xfs_err(mp,fmt,args...)		cmn_err(CE_ALERT, _(fmt), ## args)
#define xfs_alert(mp,fmt,args...)	cmn_err(CE_ALERT, _(fmt), ## args)

#define xfs_buf_ioerror_alert(bp,f)	((void) 0);

#define xfs_hex_dump(d,n)		((void) 0)
#define xfs_stack_trace()		((void) 0)


#define xfs_force_shutdown(d,n)		((void) 0)
#define xfs_mod_delalloc(a,b,c)		((void) 0)
#define xfs_mod_sb_delalloc(sb, d)	((void) 0)

/* stop unused var warnings by assigning mp to itself */

#define xfs_corruption_error(e,l,mp,b,sz,fi,ln,fa)	do { \
	(mp) = (mp); \
	cmn_err(CE_ALERT, "%s: XFS_CORRUPTION_ERROR", (e));  \
} while (0)

#define XFS_CORRUPTION_ERROR(e, lvl, mp, buf, bufsize)	do { \
	(mp) = (mp); \
	cmn_err(CE_ALERT, "%s: XFS_CORRUPTION_ERROR", (e));  \
} while (0)

#define XFS_ERROR_REPORT(e,l,mp)	do { \
	(mp) = (mp); \
	cmn_err(CE_ALERT, "%s: XFS_ERROR_REPORT", (e));  \
} while (0)

#define XFS_WARN_CORRUPT(mp, expr) \
	( xfs_is_reporting_corruption(mp) ? \
	   (printf("%s: XFS_WARN_CORRUPT at %s:%d", #expr, \
		   __func__, __LINE__), true) : true)

#define XFS_IS_CORRUPT(mp, expr)	\
	(unlikely(expr) ? XFS_WARN_CORRUPT((mp), (expr)) : false)

#define XFS_ERRLEVEL_LOW		1
#define XFS_ILOCK_EXCL			0
#define XFS_ILOCK_SHARED		0
#define XFS_IOLOCK_EXCL			0
#define XFS_STATS_INC(mp, count)	do { (mp) = (mp); } while (0)
#define XFS_STATS_DEC(mp, count, x)	do { (mp) = (mp); } while (0)
#define XFS_STATS_ADD(mp, count, x)	do { (mp) = (mp); } while (0)
#define XFS_TEST_ERROR(expr,a,b)	( expr )

#define __section(section)	__attribute__((__section__(section)))

#define xfs_printk_once(func, dev, fmt, ...)			\
({								\
	static bool __section(".data.once") __print_once;	\
	bool __ret_print_once = !__print_once;			\
								\
	if (!__print_once) {					\
		__print_once = true;				\
		func(dev, fmt, ##__VA_ARGS__);			\
	}							\
	unlikely(__ret_print_once);				\
})

#define xfs_info_once(dev, fmt, ...)				\
	xfs_printk_once(xfs_info, dev, fmt, ##__VA_ARGS__)

/* Need to be able to handle this bare or in control flow */
static inline bool WARN_ON(bool expr) {
	return (expr);
}

#define WARN_ON_ONCE(e)			WARN_ON(e)

#define percpu_counter_read_positive(x)	((*x) > 0 ? (*x) : 0)
#define percpu_counter_sum_positive(x)	((*x) > 0 ? (*x) : 0)

#ifdef HAVE_GETRANDOM_NONBLOCK
uint32_t get_random_u32(void);
#else
#define get_random_u32()	(0)
#endif

#define PAGE_SIZE		getpagesize()
extern unsigned int PAGE_SHIFT;

#define inode_peek_iversion(inode)	(inode)->i_version
#define inode_set_iversion_queried(inode, version) do { \
	(inode)->i_version = (version);	\
} while (0)

struct inode;
struct mnt_idmap;

void inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
		      const struct inode *dir, umode_t mode);

#define min_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#define max_t(type,x,y) \
	({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })

/**
 * swap - swap values of @a and @b
 * @a: first value
 * @b: second value
 */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

/*
 * Handling for kernel bitmap types.
 */
#define BITS_TO_LONGS(nr)       DIV_ROUND_UP(nr, NBBY * sizeof(long))
#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))

/*
 * This is a common helper function for find_next_bit and
 * find_next_zero_bit.  The difference is the "invert" argument, which
 * is XORed with each fetched word before searching it for one bits.
 */
static inline unsigned long
_find_next_bit(const unsigned long *addr, unsigned long nbits,
		unsigned long start, unsigned long invert)
{
	unsigned long tmp;

	if (!nbits || start >= nbits)
		return nbits;

	tmp = addr[start / BITS_PER_LONG] ^ invert;

	/* Handle 1st word. */
	tmp &= BITMAP_FIRST_WORD_MASK(start);
	start = round_down(start, BITS_PER_LONG);

	while (!tmp) {
		start += BITS_PER_LONG;
		if (start >= nbits)
			return nbits;

		tmp = addr[start / BITS_PER_LONG] ^ invert;
	}

	return min(start + ffs(tmp), nbits);
}

/*
 * Find the next set bit in a memory region.
 */
static inline unsigned long
find_next_bit(const unsigned long *addr, unsigned long size,
		unsigned long offset)
{
	return _find_next_bit(addr, size, offset, 0UL);
}
static inline unsigned long
find_next_zero_bit(const unsigned long *addr, unsigned long size,
		 unsigned long offset)
{
	return _find_next_bit(addr, size, offset, ~0UL);
}
#define find_first_zero_bit(addr, size) find_next_zero_bit((addr), (size), 0)

/*
 * xfs_iroundup: round up argument to next power of two
 */
static inline uint
roundup_pow_of_two(uint v)
{
	int	i;
	uint	m;

	if ((v & (v - 1)) == 0)
		return v;
	ASSERT((v & 0x80000000) == 0);
	if ((v & (v + 1)) == 0)
		return v + 1;
	for (i = 0, m = 1; i < 31; i++, m <<= 1) {
		if (v & m)
			continue;
		v |= m;
		if ((v & (v + 1)) == 0)
			return v + 1;
	}
	ASSERT(0);
	return 0;
}

/* If @b is a power of 2, return log2(b).  Else return -1. */
static inline int8_t log2_if_power2(unsigned long b)
{
	unsigned long	mask = 1;
	unsigned int	i;
	unsigned int	ret = 1;

	if (!is_power_of_2(b))
		return -1;

	for (i = 0; i < NBBY * sizeof(unsigned long); i++, mask <<= 1) {
		if (b & mask)
			ret = i;
	}

	return ret;
}

/* If @b is a power of 2, return a mask of the lower bits, else return zero. */
static inline unsigned long long mask64_if_power2(unsigned long b)
{
	return is_power_of_2(b) ? b - 1 : 0;
}

/* buffer management */
#define XBF_TRYLOCK			0
#define XBF_DONE			0
#define xfs_buf_stale(bp)		((bp)->b_flags |= LIBXFS_B_STALE)
#define XFS_BUF_UNDELAYWRITE(bp)	((bp)->b_flags &= ~LIBXFS_B_DIRTY)

/* buffer type flags for write callbacks */
#define _XBF_INODES	0 /* inode buffer */
#define _XBF_DQUOTS	0 /* dquot buffer */
#define _XBF_LOGRECOVERY	0 /* log recovery buffer */

static inline int
xfs_buf_incore(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks,
	xfs_buf_flags_t		flags,
	struct xfs_buf		**bpp)
{
	*bpp = NULL;
	return -ENOENT;
}

#define xfs_buf_oneshot(bp)		((void) 0)

#define xfs_buf_zero(bp, off, len) \
	memset((bp)->b_addr + off, 0, len);

void __xfs_buf_mark_corrupt(struct xfs_buf *bp, xfs_failaddr_t fa);
#define xfs_buf_mark_corrupt(bp) __xfs_buf_mark_corrupt((bp), __this_address)

/* mount stuff */
#define xfs_trans_set_sync(tp)		((void) 0)
#define xfs_trans_buf_set_type(tp, bp, t)	({	\
	int __t = (t);					\
	__t = __t; /* no set-but-unused warning */	\
	tp = tp;  /* no set-but-unused warning */	\
})

#define xfs_trans_buf_copy_type(dbp, sbp)

static inline void
xfs_buf_readahead(
	struct xfs_buftarg	*target,
	xfs_daddr_t		blkno,
	size_t			numblks,
	const struct xfs_buf_ops *ops)
{
}
#define xfs_buf_readahead_map(a,b,c,ops)	((void) 0)	/* no readahead */

#define xfs_sort					qsort

#define xfs_ilock(ip,mode)				((void) 0)
#define xfs_ilock_data_map_shared(ip)			(0)
#define xfs_ilock_attr_map_shared(ip)			(0)
#define xfs_iunlock(ip,mode)				({	\
	typeof(mode) __mode = mode;				\
	__mode = __mode; /* no set-but-unused warning */	\
})
#define xfs_lock_two_inodes(ip0,mode0,ip1,mode1)	((void) 0)
#define xfs_assert_ilocked(ip, flags)			((void) 0)
#define xfs_lock_inodes(i_tab, nr, mode)		((void) 0)
#define xfs_sort_inodes(i_tab, nr)			((void) 0)

/* space allocation */
#define XFS_EXTENT_BUSY_DISCARDED	0x01	/* undergoing a discard op. */
#define XFS_EXTENT_BUSY_SKIP_DISCARD	0x02	/* do not discard */

#define xfs_extent_busy_reuse(...)			((void) 0)
/* avoid unused variable warning */
#define xfs_extent_busy_insert(tp,xg,bno,len,flags)({ 	\
	struct xfs_group *__foo = xg;			\
	__foo = __foo; /* no set-but-unused warning */	\
})
#define xfs_extent_busy_trim(group,minlen,maxlen,bno,len,busy_gen) 	({	\
	unsigned __foo = *(busy_gen);				\
	*(busy_gen) = __foo;					\
	false;							\
})
#define xfs_extent_busy_flush(tp,pag,busy_gen,alloc_flags)	((int)(0))

#define xfs_rotorstep				1
#define xfs_bmap_rtalloc(a)			(-ENOSYS)
#define xfs_inode_is_filestream(ip)		(0)
#define xfs_filestream_lookup_ag(ip)		(0)
#define xfs_filestream_new_ag(ip,ag)		(0)
#define xfs_filestream_select_ag(...)		(-ENOSYS)

#define xfs_trans_inode_buf(tp, bp)		((void) 0)

/* quota bits */
#define xfs_trans_mod_dquot_byino(t,i,f,d)		({ \
	uint _f = (f); \
	_f = _f; /* shut up gcc */ \
})
#define xfs_trans_reserve_quota_nblks(t,i,b,n,f)	(0)

/* hack too silence gcc */
static inline int retzero(void) { return 0; }
#define xfs_trans_unreserve_quota_nblks(t,i,b,n,f)	retzero()
#define xfs_quota_unreserve_blkres(i,b) 		retzero()

#define xfs_quota_reserve_blkres(i,b)		(0)
#define xfs_qm_dqattach(i)			(0)

#define uuid_copy(s,d)		platform_uuid_copy((s),(d))
#define uuid_equal(s,d)		(platform_uuid_compare((s),(d)) == 0)

#define xfs_icreate_log(tp, agno, agbno, cnt, isize, len, gen) ((void) 0)
#define xfs_sb_validate_fsb_count(sbp, nblks)		(0)
#define xlog_calc_iovec_len(len)		roundup(len, sizeof(uint32_t))

#define xfs_zoned_add_available(mp, rtxnum)	do { } while (0)

/*
 * Prototypes for kernel static functions that are aren't in their
 * associated header files.
 */
struct xfs_da_args;
struct xfs_bmap_free;
struct xfs_bmap_free_item;
struct xfs_mount;
struct xfs_sb;
struct xfs_trans;
struct xfs_inode;
struct xfs_log_item;
struct xfs_buf;
struct xfs_buf_map;
struct xfs_buf_log_item;
struct xfs_buftarg;

/* xfs_attr.c */
int xfs_attr_rmtval_get(struct xfs_da_args *);

/* xfs_bmap.c */
void xfs_bmap_del_free(struct xfs_bmap_free *, struct xfs_bmap_free_item *);

/* xfs_mount.c */
void xfs_mount_common(struct xfs_mount *, struct xfs_sb *);

/*
 * logitem.c and trans.c prototypes
 */
void xfs_trans_init(struct xfs_mount *);
int xfs_trans_roll(struct xfs_trans **);

/* xfs_trans_item.c */
void xfs_trans_add_item(struct xfs_trans *, struct xfs_log_item *);
void xfs_trans_del_item(struct xfs_log_item *);

/* xfs_inode_item.c */
void xfs_inode_item_init(struct xfs_inode *, struct xfs_mount *);

/* xfs_buf_item.c */
void xfs_buf_item_init(struct xfs_buf *, struct xfs_mount *);
void xfs_buf_item_log(struct xfs_buf_log_item *, uint, uint);

/* xfs_trans_buf.c */
struct xfs_buf *xfs_trans_buf_item_match(struct xfs_trans *,
			struct xfs_buftarg *, struct xfs_buf_map *, int);

/* local source files */
#define xfs_add_fdblocks(mp, delta) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FDBLOCKS, delta, false)
#define xfs_dec_fdblocks(mp, delta, rsvd) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FDBLOCKS, -(int64_t)(delta), rsvd)
#define xfs_add_frextents(mp, delta) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FREXTENTS, delta, 0)
#define xfs_dec_frextents(mp, delta) \
	libxfs_mod_incore_sb(mp, XFS_TRANS_SB_FREXTENTS, -(int64_t)(delta), 0)
int  libxfs_mod_incore_sb(struct xfs_mount *, int, int64_t, int);
/* percpu counters in mp are #defined to the superblock sb_ counters */
#define xfs_reinit_percpu_counters(mp)

void xfs_trans_mod_sb(struct xfs_trans *, uint, long);

void xfs_verifier_error(struct xfs_buf *bp, int error,
			xfs_failaddr_t failaddr);
void xfs_inode_verifier_error(struct xfs_inode *ip, int error,
			const char *name, void *buf, size_t bufsz,
			xfs_failaddr_t failaddr);

#define xfs_buf_verifier_error(bp,e,n,bu,bus,fa) \
	xfs_verifier_error(bp, e, fa)
void
xfs_buf_corruption_error(struct xfs_buf *bp, xfs_failaddr_t fa);

int libxfs_zero_extent(struct xfs_inode *ip, xfs_fsblock_t start_fsb,
                        xfs_off_t count_fsb);

/* xfs_log.c */
struct xfs_item_ops;
bool xfs_log_check_lsn(struct xfs_mount *, xfs_lsn_t);
void xfs_log_item_init(struct xfs_mount *mp, struct xfs_log_item *lip, int type,
		const struct xfs_item_ops *ops);
#define xfs_attr_use_log_assist(mp)	(0)
#define xlog_drop_incompat_feat(log)	do { } while (0)
#define xfs_log_in_recovery(mp)		(false)

/* xfs_icache.c */
#define xfs_inode_clear_cowblocks_tag(ip)	do { } while (0)
#define xfs_inode_set_cowblocks_tag(ip)	do { } while (0)
#define xfs_inode_set_eofblocks_tag(ip)	do { } while (0)

/* xfs_stats.h */
#define XFS_STATS_CALC_INDEX(member)	0
#define XFS_STATS_INC_OFF(mp, off)
#define XFS_STATS_ADD_OFF(mp, off, val)

typedef unsigned char u8;
unsigned int hweight8(unsigned int w);
unsigned int hweight32(unsigned int w);
unsigned int hweight64(__u64 w);

#define xfs_buf_cache_init(bch)		(0)
#define xfs_buf_cache_destroy(bch)	((void)0)

static inline int xfs_iunlink_init(struct xfs_perag *pag) { return 0; }
static inline void xfs_iunlink_destroy(struct xfs_perag *pag) { }

xfs_agnumber_t xfs_set_inode_alloc(struct xfs_mount *mp,
		xfs_agnumber_t agcount);

/* Keep static checkers quiet about nonstatic functions by exporting */
/* xfs_bmap_util.h */
struct xfs_bmalloca;
int xfs_bmap_extsize_align(struct xfs_mount *mp, struct xfs_bmbt_irec *gotp,
			   struct xfs_bmbt_irec *prevp, xfs_extlen_t extsz,
			   int rt, int eof, int delay, int convert,
			   xfs_fileoff_t *offp, xfs_extlen_t *lenp);
bool xfs_bmap_adjacent(struct xfs_bmalloca *ap);
int xfs_bmap_last_extent(struct xfs_trans *tp, struct xfs_inode *ip,
			 int whichfork, struct xfs_bmbt_irec *rec,
			 int *is_empty);

/* xfs_inode.h */
#define xfs_iflags_set(ip, flags)	do { } while (0)
#define xfs_finish_inode_setup(ip)	((void) 0)

/* linux/wordpart.h */

/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 *
 * A basic shift-right of a 64- or 32-bit quantity.  Use this to suppress
 * the "right shift count >= width of type" warning when that quantity is
 * 32-bits.
 */
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffff))

#define cond_resched()	((void)0)

/* xfs_linux.h */
#define irix_sgid_inherit		(false)
#define vfsgid_in_group_p(...)		(false)

#endif	/* __LIBXFS_INTERNAL_XFS_H__ */
