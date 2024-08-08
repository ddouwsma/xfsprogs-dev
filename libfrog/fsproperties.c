// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include <string.h>
#include "xfs.h"
#include "libfrog/fsgeom.h"
#include "libfrog/fsproperties.h"
#include "list.h"

/* Return the offset of a string in a string table, or -1 if not found. */
static inline int
__fsprops_lookup(
	const char	*values[],
	unsigned int	nr_values,
	const char	*value)
{
	unsigned int	i;

	for (i = 0; i < nr_values; i++) {
		if (values[i] && !strcmp(value, values[i]))
			return i;
	}

	return -1;
}

#define fsprops_lookup(values, value) \
	__fsprops_lookup((values), ARRAY_SIZE(values), (value))

/* Automatic background fsck fs property */

static const char *fsprop_autofsck_values[] = {
	[FSPROP_AUTOFSCK_UNSET]		= NULL,
	[FSPROP_AUTOFSCK_NONE]		= "none",
	[FSPROP_AUTOFSCK_CHECK]		= "check",
	[FSPROP_AUTOFSCK_OPTIMIZE]	= "optimize",
	[FSPROP_AUTOFSCK_REPAIR]	= "repair",
};

/* Convert the autofsck property enum to a string. */
const char *
fsprop_autofsck_write(
	enum fsprop_autofsck	x)
{
	if (x <= FSPROP_AUTOFSCK_UNSET ||
	    x >= ARRAY_SIZE(fsprop_autofsck_values))
		return NULL;
	return fsprop_autofsck_values[x];
}

/*
 * Turn a autofsck value string into an enumerated value, or _UNSET if it's
 * not recognized.
 */
enum fsprop_autofsck
fsprop_autofsck_read(
	const char	*value)
{
	int ret = fsprops_lookup(fsprop_autofsck_values, value);
	if (ret < 0)
		return FSPROP_AUTOFSCK_UNSET;
	return ret;
}

/* Return true if a fs property name=value tuple is allowed. */
bool
fsprop_validate(
	const char	*name,
	const char	*value)
{
	if (!strcmp(name, FSPROP_AUTOFSCK_NAME))
		return fsprops_lookup(fsprop_autofsck_values, value) >= 0;

	return true;
}
