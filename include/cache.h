// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __CACHE_H__
#define __CACHE_H__

/*
 * initialisation flags
 */
/*
 * xfs_db always writes changes immediately, and so we need to purge buffers
 * when we get a buffer lookup mismatch due to reading the same block with a
 * different buffer configuration.
 */
#define CACHE_MISCOMPARE_PURGE	(1 << 0)

/*
 * cache object campare return values
 */
enum {
	CACHE_HIT,
	CACHE_MISS,
	CACHE_PURGE,
};

#define	HASH_CACHE_RATIO	8

/*
 * Cache priorities range from BASE to MAX.
 *
 * For prefetch support, the top half of the range starts at
 * CACHE_PREFETCH_PRIORITY and everytime the buffer is fetched and is at or
 * above this priority level, it is reduced to below this level (refer to
 * libxfs_buf_get).
 *
 * If we have dirty nodes, we can't recycle them until they've been cleaned. To
 * keep these out of the reclaimable lists (as there can be lots of them) give
 * them their own priority that the shaker doesn't attempt to walk.
 */

#define CACHE_BASE_PRIORITY	0
#define CACHE_PREFETCH_PRIORITY	8
#define CACHE_MAX_PRIORITY	15
#define CACHE_DIRTY_PRIORITY	(CACHE_MAX_PRIORITY + 1)
#define CACHE_NR_PRIORITIES	CACHE_DIRTY_PRIORITY

/*
 * Simple, generic implementation of a cache (arbitrary data).
 * Provides a hash table with a capped number of cache entries.
 */

struct cache;
struct cache_node;

typedef void *cache_key_t;

typedef void (*cache_walk_t)(struct cache_node *);
typedef struct cache_node * (*cache_node_alloc_t)(cache_key_t);
typedef int (*cache_node_flush_t)(struct cache_node *);
typedef void (*cache_node_relse_t)(struct cache_node *);
typedef unsigned int (*cache_node_hash_t)(cache_key_t, unsigned int,
					  unsigned int);
typedef int (*cache_node_compare_t)(struct cache_node *, cache_key_t);
typedef unsigned int (*cache_bulk_relse_t)(struct cache *, struct list_head *);
typedef int (*cache_node_get_t)(struct cache_node *);
typedef void (*cache_node_put_t)(struct cache_node *);

struct cache_operations {
	cache_node_hash_t	hash;
	cache_node_alloc_t	alloc;
	cache_node_flush_t	flush;
	cache_node_relse_t	relse;
	cache_node_compare_t	compare;
	cache_bulk_relse_t	bulkrelse;	/* optional */
	cache_node_get_t	get;		/* optional */
	cache_node_put_t	put;		/* optional */
};

struct cache_hash {
	struct list_head	ch_list;	/* hash chain head */
	unsigned int		ch_count;	/* hash chain length */
	pthread_mutex_t		ch_mutex;	/* hash chain mutex */
};

struct cache_mru {
	struct list_head	cm_list;	/* MRU head */
	unsigned int		cm_count;	/* MRU length */
	pthread_mutex_t		cm_mutex;	/* MRU lock */
};

struct cache_node {
	struct list_head	cn_hash;	/* hash chain */
	struct list_head	cn_mru;		/* MRU chain */
	unsigned int		cn_count;	/* reference count */
	unsigned int		cn_hashidx;	/* hash chain index */
	int			cn_priority;	/* priority, -1 = free list */
	int			cn_old_priority;/* saved pre-dirty prio */
	pthread_mutex_t		cn_mutex;	/* node mutex */
};

struct cache {
	int			c_flags;	/* behavioural flags */
	unsigned int		c_maxcount;	/* max cache nodes */
	unsigned int		c_count;	/* count of nodes */
	pthread_mutex_t		c_mutex;	/* node count mutex */
	cache_node_hash_t	hash;		/* node hash function */
	cache_node_alloc_t	alloc;		/* allocation function */
	cache_node_flush_t	flush;		/* flush dirty data function */
	cache_node_relse_t	relse;		/* memory free function */
	cache_node_compare_t	compare;	/* comparison routine */
	cache_bulk_relse_t	bulkrelse;	/* bulk release routine */
	cache_node_get_t	get;		/* prepare cache node after get */
	cache_node_put_t	put;		/* prepare to put cache node */
	unsigned int		c_hashsize;	/* hash bucket count */
	unsigned int		c_hashshift;	/* hash key shift */
	struct cache_hash	*c_hash;	/* hash table buckets */
	struct cache_mru	c_mrus[CACHE_DIRTY_PRIORITY + 1];
	unsigned long long	c_misses;	/* cache misses */
	unsigned long long	c_hits;		/* cache hits */
	unsigned int 		c_max;		/* max nodes ever used */
};

struct cache *cache_init(int, unsigned int, struct cache_operations *);
void cache_destroy(struct cache *);
void cache_walk(struct cache *, cache_walk_t);
void cache_purge(struct cache *);
void cache_flush(struct cache *);

int cache_node_get(struct cache *, cache_key_t, struct cache_node **);
void cache_node_put(struct cache *, struct cache_node *);
void cache_node_set_priority(struct cache *, struct cache_node *, int);
int cache_node_get_priority(struct cache_node *);
int cache_node_purge(struct cache *, cache_key_t, struct cache_node *);
void cache_report(FILE *fp, const char *, struct cache *);
int cache_overflowed(struct cache *);

#endif	/* __CACHE_H__ */
