/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __LIBFROG_FSPROPERTIES_H__
#define __LIBFROG_FSPROPERTIES_H__

/* Name space for filesystem properties. */
#define FSPROP_NAMESPACE	"trusted."

/*
 * All filesystem property xattr names must have this string after the
 * namespace.  For example, the VFS xattr calls should use the name
 * "trusted.xfs:fubar".  The xfs xattr ioctls would set ATTR_ROOT and use the
 * name "xfs:fubar".
 */
#define FSPROP_NAME_PREFIX	"xfs:"

/* Maximum size the value of a filesystem property. */
#define FSPROP_MAX_VALUELEN	(65536)

static inline int
fsprop_name_to_attr_name(
	const char		*prop_name,
	char			**attr_name)
{
	return asprintf(attr_name, FSPROP_NAME_PREFIX "%s", prop_name);
}

static inline const char *
attr_name_to_fsprop_name(
	const char		*attr_name)
{
	const size_t		bytes = sizeof(FSPROP_NAME_PREFIX) - 1;
	unsigned int		i;

	for (i = 0; i < bytes; i++) {
		if (attr_name[i] == 0)
			return NULL;
	}

	if (memcmp(attr_name, FSPROP_NAME_PREFIX, bytes) != 0)
		return NULL;

	return attr_name + bytes;
}

bool fsprop_validate(const char *name, const char *value);

/* Specific Filesystem Properties */

#endif /* __LIBFROG_FSPROPERTIES_H__ */
