// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "libxfs.h"
#include "command.h"
#include "attrset.h"
#include "io.h"
#include "output.h"
#include "type.h"
#include "init.h"
#include "fprint.h"
#include "faddr.h"
#include "field.h"
#include "inode.h"
#include "malloc.h"
#include <sys/xattr.h>
#include "libfrog/fsproperties.h"
#include "libxfs/listxattr.h"

static int		attr_list_f(int argc, char **argv);
static int		attr_get_f(int argc, char **argv);
static int		attr_set_f(int argc, char **argv);
static int		attr_remove_f(int argc, char **argv);

static void		attrlist_help(void);
static void		attrget_help(void);
static void		attrset_help(void);

static const cmdinfo_t	attr_list_cmd =
	{ "attr_list", "alist", attr_list_f, 0, -1, 0,
	  N_("[-r|-s|-u|-p|-Z] [-v]"),
	  N_("list attributes on the current inode"), attrlist_help };
static const cmdinfo_t	attr_get_cmd =
	{ "attr_get", "aget", attr_get_f, 1, -1, 0,
	  N_("[-r|-s|-u|-p|-Z] name"),
	  N_("get the named attribute on the current inode"), attrget_help };
static const cmdinfo_t	attr_set_cmd =
	{ "attr_set", "aset", attr_set_f, 1, -1, 0,
	  N_("[-r|-s|-u|-p|-Z] [-n] [-R|-C] [-v n] name"),
	  N_("set the named attribute on the current inode"), attrset_help };
static const cmdinfo_t	attr_remove_cmd =
	{ "attr_remove", "aremove", attr_remove_f, 1, -1, 0,
	  N_("[-r|-s|-u|-p|-Z] [-n] name"),
	  N_("remove the named attribute from the current inode"), attrset_help };

static void
attrlist_help(void)
{
	dbprintf(_(
"\n"
" The attr_list command provide interfaces for listing all extended attributes\n"
" attached to an inode.\n"
" There are 4 namespace flags:\n"
"  -r -- 'root'\n"
"  -u -- 'user'		(default)\n"
"  -s -- 'secure'\n"
"  -p -- 'parent'\n"
"  -Z -- fs property\n"
"\n"
"  -v -- print the value of the attributes\n"
"\n"));
}

static void
attrget_help(void)
{
	dbprintf(_(
"\n"
" The attr_get command provide interfaces for retrieving the values of extended\n"
" attributes of a file.  This command requires attribute names to be specified.\n"
" There are 4 namespace flags:\n"
"  -r -- 'root'\n"
"  -u -- 'user'		(default)\n"
"  -s -- 'secure'\n"
"  -p -- 'parent'\n"
"  -Z -- fs property\n"
"\n"));
}

static void
attrset_help(void)
{
	dbprintf(_(
"\n"
" The 'attr_set' and 'attr_remove' commands provide interfaces for debugging\n"
" the extended attribute allocation and removal code.\n"
" Both commands require an attribute name to be specified, and the attr_set\n"
" command allows an optional value length (-v) to be provided as well.\n"
" There are 4 namespace flags:\n"
"  -r -- 'root'\n"
"  -u -- 'user'		(default)\n"
"  -s -- 'secure'\n"
"  -p -- 'parent'\n"
"  -Z -- fs property\n"
"\n"
" For attr_set, these options further define the type of set operation:\n"
"  -C -- 'create'    - create attribute, fail if it already exists\n"
"  -R -- 'replace'   - replace attribute, fail if it does not exist\n"
"\n"
" If the attribute value is a string, it can be specified after the\n"
" attribute name.\n"
"\n"
" The backward compatibility mode 'noattr2' can be emulated (-n) also.\n"
"\n"));
}

void
attrset_init(void)
{
	if (!expert_mode)
		return;

	add_command(&attr_list_cmd);
	add_command(&attr_get_cmd);
	add_command(&attr_set_cmd);
	add_command(&attr_remove_cmd);
}

static unsigned char *
get_buf_from_file(
	const char	*fname,
	size_t		bufsize,
	int		*namelen)
{
	FILE		*fp;
	unsigned char	*buf;
	size_t		sz;

	buf = malloc(bufsize + 1);
	if (!buf) {
		perror("malloc");
		return NULL;
	}

	fp = fopen(fname, "r");
	if (!fp) {
		perror(fname);
		goto out_free;
	}

	sz = fread(buf, sizeof(char), bufsize, fp);
	if (sz == 0) {
		printf("%s: Could not read anything from file\n", fname);
		goto out_fp;
	}

	fclose(fp);

	*namelen = sz;
	return buf;
out_fp:
	fclose(fp);
out_free:
	free(buf);
	return NULL;
}

#define LIBXFS_ATTR_NS		(LIBXFS_ATTR_SECURE | \
				 LIBXFS_ATTR_ROOT | \
				 LIBXFS_ATTR_PARENT)

static bool
adjust_fsprop_attr_name(
	struct xfs_da_args	*args,
	bool			*free_name)
{
	const char		*o = (const char *)args->name;
	char			*p;
	int			ret;

	if ((args->attr_filter & LIBXFS_ATTR_NS) != LIBXFS_ATTR_ROOT) {
		dbprintf(_("fs properties must be ATTR_ROOT\n"));
		return false;
	}

	ret = fsprop_name_to_attr_name(o, &p);
	if (ret < 0) {
		dbprintf(_("could not allocate fs property name string\n"));
		return false;
	}
	args->namelen = ret;
	args->name = (const uint8_t *)p;

	if (*free_name)
		free((void *)o);
	*free_name = true;

	if (args->namelen > MAXNAMELEN) {
		dbprintf(_("%s: name too long\n"), args->name);
		return false;
	}

	if (args->valuelen > ATTR_MAX_VALUELEN) {
		dbprintf(_("%s: value too long\n"), args->name);
		return false;
	}

	return true;
}

static void
print_fsprop(
	struct xfs_da_args	*args)
{
	const char		*p =
		attr_name_to_fsprop_name((const char *)args->name);

	if (p)
		printf("%s=%.*s\n", p, args->valuelen, (char *)args->value);
	else
		fprintf(stderr, _("%s: not a fs property?\n"), args->name);
}

static int
attr_set_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = {
		.geo		= mp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
	};
	char			*sp;
	char			*name_from_file = NULL;
	char			*value_from_file = NULL;
	bool			free_name = false;
	enum xfs_attr_update	op = XFS_ATTRUPDATE_UPSERT;
	bool			fsprop = false;
	int			c;
	int			error;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspCRnN:v:V:Z")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'Z':
			fsprop = true;
			fallthrough;
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		/* modifiers */
		case 'C':
			op = XFS_ATTRUPDATE_CREATE;
			break;
		case 'R':
			op = XFS_ATTRUPDATE_REPLACE;
			break;

		case 'N':
			name_from_file = optarg;
			break;

		case 'n':
			/*
			 * We never touch attr2 these days; leave this here to
			 * avoid breaking scripts.
			 */
			break;

		/* value length */
		case 'v':
			if (value_from_file) {
				dbprintf(_("already set value file\n"));
				return 0;
			}

			args.valuelen = strtol(optarg, &sp, 0);
			if (*sp != '\0' ||
			    args.valuelen < 0 || args.valuelen > 64 * 1024) {
				dbprintf(_("bad attr_set valuelen %s\n"), optarg);
				return 0;
			}
			break;

		case 'V':
			if (args.valuelen != 0) {
				dbprintf(_("already set valuelen\n"));
				return 0;
			}

			value_from_file = optarg;
			break;

		default:
			dbprintf(_("bad option for attr_set command\n"));
			return 0;
		}
	}

	if (name_from_file) {
		int namelen;

		if (optind != argc) {
			dbprintf(_("too many options for attr_set (no name needed)\n"));
			return 0;
		}

		args.name = get_buf_from_file(name_from_file, MAXNAMELEN,
				&namelen);
		if (!args.name)
			return 0;

		free_name = true;
		args.namelen = namelen;
	} else {
		if (optind != argc - 1 && optind != argc - 2) {
			dbprintf(_("too few options for attr_set (no name given)\n"));
			return 0;
		}

		args.name = (const unsigned char *)argv[optind];
		if (!args.name) {
			dbprintf(_("invalid name\n"));
			return 0;
		}

		args.namelen = strlen(argv[optind]);
		if (args.namelen >= MAXNAMELEN) {
			dbprintf(_("name too long\n"));
			goto out;
		}
	}

	if (value_from_file) {
		int valuelen;

		args.value = get_buf_from_file(value_from_file,
				XFS_XATTR_SIZE_MAX, &valuelen);
		if (!args.value)
			goto out;

		args.valuelen = valuelen;
	} else if (args.valuelen) {
		args.value = memalign(getpagesize(), args.valuelen);
		if (!args.value) {
			dbprintf(_("cannot allocate buffer (%d)\n"),
				args.valuelen);
			goto out;
		}
		memset(args.value, 'v', args.valuelen);
	} else if (optind == argc - 2) {
		args.valuelen = strlen(argv[optind + 1]);
		args.value = strdup(argv[optind + 1]);
		if (!args.value) {
			dbprintf(_("cannot allocate buffer (%d)\n"),
					args.valuelen);
			goto out;
		}
	}

	if (fsprop) {
		if (!fsprop_validate((const char *)args.name, args.value)) {
			dbprintf(_("%s: invalid value \"%s\"\n"),
					args.name, args.value);
			goto out;
		}

		if (!adjust_fsprop_attr_name(&args, &free_name))
			goto out;
	}

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	args.owner = iocur_top->ino;
	libxfs_attr_sethash(&args);

	error = -libxfs_attr_set(&args, op, false);
	if (error) {
		dbprintf(_("failed to set attr %s on inode %llu: %s\n"),
			args.name, (unsigned long long)iocur_top->ino,
			strerror(error));
		goto out;
	}

	if (fsprop)
		print_fsprop(&args);

	/* refresh with updated inode contents */
	set_cur_inode(iocur_top->ino);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	if (args.value)
		free(args.value);
	if (free_name)
		free((void *)args.name);
	return 0;
}

static int
attr_remove_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = {
		.geo		= mp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
	};
	char			*name_from_file = NULL;
	bool			free_name = false;
	bool			fsprop = false;
	int			c;
	int			error;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspnN:Z")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'Z':
			fsprop = true;
			fallthrough;
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		case 'N':
			name_from_file = optarg;
			break;

		case 'n':
			/*
			 * We never touch attr2 these days; leave this here to
			 * avoid breaking scripts.
			 */
			break;

		default:
			dbprintf(_("bad option for attr_remove command\n"));
			return 0;
		}
	}

	if (name_from_file) {
		int namelen;

		if (optind != argc) {
			dbprintf(_("too many options for attr_set (no name needed)\n"));
			return 0;
		}

		args.name = get_buf_from_file(name_from_file, MAXNAMELEN,
				&namelen);
		if (!args.name)
			return 0;

		free_name = true;
		args.namelen = namelen;
	} else {
		if (optind != argc - 1) {
			dbprintf(_("too few options for attr_remove (no name given)\n"));
			return 0;
		}

		args.name = (const unsigned char *)argv[optind];
		if (!args.name) {
			dbprintf(_("invalid name\n"));
			return 0;
		}

		args.namelen = strlen(argv[optind]);
		if (args.namelen >= MAXNAMELEN) {
			dbprintf(_("name too long\n"));
			return 0;
		}
	}

	if (fsprop && !adjust_fsprop_attr_name(&args, &free_name))
		goto out;

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	args.owner = iocur_top->ino;
	libxfs_attr_sethash(&args);

	error = -libxfs_attr_set(&args, XFS_ATTRUPDATE_REMOVE, false);
	if (error) {
		dbprintf(_("failed to remove attr %s from inode %llu: %s\n"),
			(unsigned char *)args.name,
			(unsigned long long)iocur_top->ino,
			strerror(error));
		goto out;
	}

	/* refresh with updated inode contents */
	set_cur_inode(iocur_top->ino);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	if (free_name)
		free((void *)args.name);
	return 0;
}

static int
attr_get_f(
	int			argc,
	char			**argv)
{
	struct xfs_da_args	args = {
		.geo		= mp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
	};
	char			*name_from_file = NULL;
	bool			free_name = false;
	bool			fsprop = false;
	int			c;
	int			error;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspN:Z")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'Z':
			fsprop = true;
			fallthrough;
		case 'r':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			args.attr_filter &= ~LIBXFS_ATTR_NS;
			args.attr_filter |= XFS_ATTR_PARENT;
			break;

		case 'N':
			name_from_file = optarg;
			break;
		default:
			dbprintf(_("bad option for attr_get command\n"));
			return 0;
		}
	}

	if (name_from_file) {
		int namelen;

		if (optind != argc) {
			dbprintf(_("too many options for attr_get (no name needed)\n"));
			return 0;
		}

		args.name = get_buf_from_file(name_from_file, MAXNAMELEN,
				&namelen);
		if (!args.name)
			return 0;

		free_name = true;
		args.namelen = namelen;
	} else {
		if (optind != argc - 1) {
			dbprintf(_("too few options for attr_get (no name given)\n"));
			return 0;
		}

		args.name = (const unsigned char *)argv[optind];
		if (!args.name) {
			dbprintf(_("invalid name\n"));
			return 0;
		}

		args.namelen = strlen(argv[optind]);
		if (args.namelen >= MAXNAMELEN) {
			dbprintf(_("name too long\n"));
			goto out;
		}
	}

	if (libxfs_iget(mp, NULL, iocur_top->ino, 0, &args.dp)) {
		dbprintf(_("failed to iget inode %llu\n"),
			(unsigned long long)iocur_top->ino);
		goto out;
	}

	if (fsprop && !adjust_fsprop_attr_name(&args, &free_name))
		goto out;

	args.owner = iocur_top->ino;
	libxfs_attr_sethash(&args);

	/*
	 * Look up attr value with a maximally long length and a null buffer
	 * to return the value and the correct length.
	 */
	args.valuelen = XATTR_SIZE_MAX;
	error = -libxfs_attr_get(&args);
	if (error) {
		dbprintf(_("failed to get attr %s on inode %llu: %s\n"),
			args.name, (unsigned long long)iocur_top->ino,
			strerror(error));
		goto out;
	}

	if (fsprop)
		print_fsprop(&args);
	else
		printf("%.*s\n", args.valuelen, (char *)args.value);

out:
	if (args.dp)
		libxfs_irele(args.dp);
	if (args.value)
		free(args.value);
	if (free_name)
		free((void *)args.name);
	return 0;
}

struct attrlist_ctx {
	unsigned int		attr_filter;
	bool			print_values;
	bool			fsprop;
};

static int
attrlist_print(
	struct xfs_trans	*tp,
	struct xfs_inode	*ip,
	unsigned int		attr_flags,
	const unsigned char	*name,
	unsigned int		namelen,
	const void		*value,
	unsigned int		valuelen,
	void			*priv)
{
	struct attrlist_ctx	*ctx = priv;
	struct xfs_da_args	args = {
		.geo		= mp->m_attr_geo,
		.whichfork	= XFS_ATTR_FORK,
		.op_flags	= XFS_DA_OP_OKNOENT,
		.dp		= ip,
		.owner		= ip->i_ino,
		.trans		= tp,
		.attr_filter	= attr_flags & XFS_ATTR_NSP_ONDISK_MASK,
		.name		= name,
		.namelen	= namelen,
	};
	char			namebuf[MAXNAMELEN + 1];
	const char		*print_name = namebuf;
	int			error;

	if ((attr_flags & XFS_ATTR_NSP_ONDISK_MASK) != ctx->attr_filter)
		return 0;

	/* Make sure the name is null terminated. */
	memcpy(namebuf, name, namelen);
	namebuf[MAXNAMELEN] = 0;

	if (ctx->fsprop) {
		const char	*p = attr_name_to_fsprop_name(namebuf);

		if (!p)
			return 0;

		namelen -= (p - namebuf);
		print_name = p;
	}

	if (!ctx->print_values) {
		printf("%.*s\n", namelen, print_name);
		return 0;
	}

	if (value) {
		printf("%.*s=%.*s\n", namelen, print_name, valuelen,
				(char *)value);
		return 0;
	}

	libxfs_attr_sethash(&args);

	/*
	 * Look up attr value with a maximally long length and a null buffer
	 * to return the value and the correct length.
	 */
	args.valuelen = XATTR_SIZE_MAX;
	error = -libxfs_attr_get(&args);
	if (error) {
		dbprintf(_("failed to get attr %s on inode %llu: %s\n"),
				args.name, (unsigned long long)iocur_top->ino,
				strerror(error));
		return error;
	}

	printf("%.*s=%.*s\n", namelen, print_name, args.valuelen,
			(char *)args.value);
	kfree(args.value);

	return 0;
}

static int
attr_list_f(
	int			argc,
	char			**argv)
{
	struct attrlist_ctx	ctx = { };
	struct xfs_trans	*tp;
	struct xfs_inode	*ip;
	int			c;
	int			error;

	if (cur_typ == NULL) {
		dbprintf(_("no current type\n"));
		return 0;
	}
	if (cur_typ->typnm != TYP_INODE) {
		dbprintf(_("current type is not inode\n"));
		return 0;
	}

	while ((c = getopt(argc, argv, "ruspvZ")) != EOF) {
		switch (c) {
		/* namespaces */
		case 'Z':
			ctx.fsprop = true;
			fallthrough;
		case 'r':
			ctx.attr_filter &= ~LIBXFS_ATTR_NS;
			ctx.attr_filter |= LIBXFS_ATTR_ROOT;
			break;
		case 'u':
			ctx.attr_filter &= ~LIBXFS_ATTR_NS;
			break;
		case 's':
			ctx.attr_filter &= ~LIBXFS_ATTR_NS;
			ctx.attr_filter |= LIBXFS_ATTR_SECURE;
			break;
		case 'p':
			ctx.attr_filter &= ~LIBXFS_ATTR_NS;
			ctx.attr_filter |= XFS_ATTR_PARENT;
			break;

		case 'v':
			ctx.print_values = true;
			break;
		default:
			dbprintf(_("bad option for attr_list command\n"));
			return 0;
		}
	}

	if (ctx.fsprop &&
	    (ctx.attr_filter & LIBXFS_ATTR_NS) != LIBXFS_ATTR_ROOT) {
		dbprintf(_("fs properties must be ATTR_ROOT\n"));
		return false;
	}

	if (optind != argc) {
		dbprintf(_("too many options for attr_list (no name needed)\n"));
		return 0;
	}

	error = -libxfs_trans_alloc_empty(mp, &tp);
	if (error) {
		dbprintf(_("failed to allocate empty transaction\n"));
		return 0;
	}

	error = -libxfs_iget(mp, NULL, iocur_top->ino, 0, &ip);
	if (error) {
		dbprintf(_("failed to iget inode %llu: %s\n"),
				(unsigned long long)iocur_top->ino,
				strerror(error));
		goto out_trans;
	}

	error = xattr_walk(tp, ip, attrlist_print, &ctx);
	if (error) {
		dbprintf(_("walking inode %llu xattrs: %s\n"),
				(unsigned long long)iocur_top->ino,
				strerror(error));
		goto out_inode;
	}

out_inode:
	libxfs_irele(ip);
out_trans:
	libxfs_trans_cancel(tp);
	return 0;
}
