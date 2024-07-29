// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2012 Red Hat, Inc.
 * Copyright (c) 2017-2024 Oracle.
 * All Rights Reserved.
 */
#include "xfs.h"
#include <stdlib.h>
#include <string.h>
#include "platform_defs.h"
#include "libfrog/histogram.h"

/* Create a new bucket with the given low value. */
int
hist_add_bucket(
	struct histogram	*hs,
	long long		bucket_low)
{
	struct histbucket	*buckets;

	if (hs->nr_buckets == INT_MAX)
		return EFBIG;

	buckets = realloc(hs->buckets,
			(hs->nr_buckets + 1) * sizeof(struct histbucket));
	if (!buckets)
		return errno;

	hs->buckets = buckets;
	hs->buckets[hs->nr_buckets].low = bucket_low;
	hs->buckets[hs->nr_buckets].nr_obs = 0;
	hs->buckets[hs->nr_buckets].sum = 0;
	hs->nr_buckets++;
	return 0;
}

/* Add an observation to the histogram. */
void
hist_add(
	struct histogram	*hs,
	long long		len)
{
	unsigned int		i;

	hs->tot_obs++;
	hs->tot_sum += len;
	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].high >= len) {
			hs->buckets[i].nr_obs++;
			hs->buckets[i].sum += len;
			break;
		}
	}
}

static int
histbucket_cmp(
	const void		*a,
	const void		*b)
{
	const struct histbucket	*ha = a;
	const struct histbucket	*hb = b;

	if (ha->low < hb->low)
		return -1;
	if (ha->low > hb->low)
		return 1;
	return 0;
}

/* Prepare a histogram for bucket configuration. */
void
hist_init(
	struct histogram	*hs)
{
	memset(hs, 0, sizeof(struct histogram));
}

/* Prepare a histogram to receive data observations. */
void
hist_prepare(
	struct histogram	*hs,
	long long		maxlen)
{
	unsigned int		i;

	qsort(hs->buckets, hs->nr_buckets, sizeof(struct histbucket),
			histbucket_cmp);

	for (i = 0; i < hs->nr_buckets - 1; i++)
		hs->buckets[i].high = hs->buckets[i + 1].low - 1;
	hs->buckets[hs->nr_buckets - 1].high = maxlen;
}

/* Free all data associated with a histogram. */
void
hist_free(
	struct histogram	*hs)
{
	free(hs->buckets);
	memset(hs, 0, sizeof(struct histogram));
}

/* Dump a histogram to stdout. */
void
hist_print(
	const struct histogram		*hs,
	const struct histogram_strings	*hstr)
{
	unsigned int			obs_w = strlen(hstr->observations);
	unsigned int			sum_w = strlen(hstr->sum);
	unsigned int			from_w = 7, to_w = 7;
	unsigned int			i;

	for (i = 0; i < hs->nr_buckets; i++) {
		char buf[256];

		if (hs->buckets[i].nr_obs == 0)
			continue;

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].low);
		from_w = max(from_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].high);
		to_w = max(to_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].nr_obs);
		obs_w = max(obs_w, strlen(buf));

		snprintf(buf, sizeof(buf) - 1, "%lld", hs->buckets[i].sum);
		sum_w = max(sum_w, strlen(buf));
	}

	printf("%*s %*s %*s %*s %6s\n",
			from_w, _("from"), to_w, _("to"),
			obs_w, hstr->observations,
			sum_w, hstr->sum,
			_("pct"));

	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].nr_obs == 0)
			continue;

		printf("%*lld %*lld %*lld %*lld %6.2f\n",
				from_w, hs->buckets[i].low,
				to_w, hs->buckets[i].high,
				obs_w, hs->buckets[i].nr_obs,
				sum_w, hs->buckets[i].sum,
				hs->buckets[i].sum * 100.0 / hs->tot_sum);
	}
}

/* Summarize the contents of the histogram. */
void
hist_summarize(
	const struct histogram		*hs,
	const struct histogram_strings	*hstr)
{
	printf("%s %lld\n", hstr->observations, hs->tot_obs);
	printf("%s %lld\n", hstr->sum, hs->tot_sum);
	printf("%s %g\n", hstr->averages,
			(double)hs->tot_sum / (double)hs->tot_obs);
}
