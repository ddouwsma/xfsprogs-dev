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

/*
 * Compute the CDF of the histogram in decreasing order of value.
 *
 * For a free space histogram, callers can determine how much free space is not
 * in the long tail of small extents, e.g. 98% of the free space extents are
 * larger than 31 blocks.
 */
struct histogram_cdf *
hist_cdf(
	const struct histogram	*hs)
{
	struct histogram_cdf	*cdf;
	int			i = hs->nr_buckets - 1;

	ASSERT(hs->nr_buckets < INT_MAX);

	cdf = malloc(sizeof(struct histogram_cdf));
	if (!cdf)
		return NULL;
	cdf->histogram = hs;

	if (hs->nr_buckets == 0) {
		cdf->buckets = NULL;
		return cdf;
	}

	cdf->buckets = calloc(hs->nr_buckets, sizeof(struct histbucket));
	if (!cdf->buckets) {
		free(cdf);
		return NULL;
	}

	cdf->buckets[i].nr_obs = hs->buckets[i].nr_obs;
	cdf->buckets[i].sum = hs->buckets[i].sum;
	i--;

	while (i >= 0) {
		cdf->buckets[i].nr_obs = hs->buckets[i].nr_obs +
					cdf->buckets[i + 1].nr_obs;

		cdf->buckets[i].sum =    hs->buckets[i].sum +
					cdf->buckets[i + 1].sum;
		i--;
	}

	return cdf;
}

/* Free all data associated with a histogram cdf. */
void
histcdf_free(
	struct histogram_cdf	*cdf)
{
	free(cdf->buckets);
	free(cdf);
}

/* Dump a histogram to stdout. */
void
hist_print(
	const struct histogram		*hs,
	const struct histogram_strings	*hstr)
{
	struct histogram_cdf		*cdf;
	unsigned int			obs_w = strlen(hstr->observations);
	unsigned int			sum_w = strlen(hstr->sum);
	unsigned int			from_w = 7, to_w = 7;
	unsigned int			i;

	cdf = hist_cdf(hs);
	if (!cdf) {
		perror(_("histogram cdf"));
		return;
	}

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

	printf("%*s %*s %*s %*s %6s %6s %6s\n",
			from_w, _("from"), to_w, _("to"),
			obs_w, hstr->observations,
			sum_w, hstr->sum,
			_("pct"), _("blkcdf"), _("extcdf"));

	for (i = 0; i < hs->nr_buckets; i++) {
		if (hs->buckets[i].nr_obs == 0)
			continue;

		printf("%*lld %*lld %*lld %*lld %6.2f %6.2f %6.2f\n",
				from_w, hs->buckets[i].low,
				to_w, hs->buckets[i].high,
				obs_w, hs->buckets[i].nr_obs,
				sum_w, hs->buckets[i].sum,
				hs->buckets[i].sum * 100.0 / hs->tot_sum,
				cdf->buckets[i].sum * 100.0 / hs->tot_sum,
				cdf->buckets[i].nr_obs * 100.0 / hs->tot_obs);
	}

	histcdf_free(cdf);
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

/* Copy the contents of src to dest. */
void
hist_import(
	struct histogram	*dest,
	const struct histogram	*src)
{
	unsigned int		i;

	ASSERT(dest->nr_buckets == src->nr_buckets);

	dest->tot_sum += src->tot_sum;
	dest->tot_obs += src->tot_obs;

	for (i = 0; i < dest->nr_buckets; i++) {
		ASSERT(dest->buckets[i].low == src->buckets[i].low);
		ASSERT(dest->buckets[i].high == src->buckets[i].high);

		dest->buckets[i].nr_obs += src->buckets[i].nr_obs;
		dest->buckets[i].sum += src->buckets[i].sum;
	}
}

/*
 * Move the contents of src to dest and reinitialize src.  dst must not
 * contain any observations or buckets.
 */
void
hist_move(
	struct histogram	*dest,
	struct histogram	*src)
{
	ASSERT(dest->nr_buckets == 0);
	ASSERT(dest->tot_obs == 0);

	memcpy(dest, src, sizeof(struct histogram));
	hist_init(src);
}
