// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2004-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

#include "command.h"
#include "input.h"
#include <sys/mman.h>
#include "init.h"
#include "io.h"
#include <asm/mman.h>

static cmdinfo_t madvise_cmd;

static void
madvise_help(void)
{
	printf(_(
"\n"
" advise the page cache about access patterns expected for a mapping\n"
"\n"
" Modifies page cache behavior when operating on the current mapping.\n"
" The range arguments are required by some advise commands ([*] below).\n"
" With no arguments, the POSIX_MADV_NORMAL advice is implied.\n"
" -d -- don't need these pages (POSIX_MADV_DONTNEED) [*]\n"
" -r -- expect random page references (POSIX_MADV_RANDOM)\n"
" -s -- expect sequential page references (POSIX_MADV_SEQUENTIAL)\n"
" -w -- will need these pages (POSIX_MADV_WILLNEED) [*]\n"
"\n"
"The following Linux-specific advise values are available:\n"
#ifdef MADV_COLLAPSE
" -c -- try to collapse range into transparent hugepages (MADV_COLLAPSE)\n"
#endif
#ifdef MADV_COLD
" -D -- deactivate the range (MADV_COLD)\n"
#endif
" -f -- free the range (MADV_FREE)\n"
" -h -- disable transparent hugepages (MADV_NOHUGEPAGE)\n"
" -H -- enable transparent hugepages (MADV_HUGEPAGE)\n"
" -m -- mark the range mergeable (MADV_MERGEABLE)\n"
" -M -- mark the range unmergeable (MADV_UNMERGEABLE)\n"
#ifdef MADV_SOFT_OFFLINE
" -o -- mark the range offline (MADV_SOFT_OFFLINE)\n"
#endif
" -p -- punch a hole in the file (MADV_REMOVE)\n"
" -P -- poison the page cache (MADV_HWPOISON)\n"
#ifdef MADV_POPULATE_READ
" -R -- prefault in the range for read (MADV_POPULATE_READ)\n"
#endif
#ifdef MADV_POPULATE_WRITE
" -W -- prefault in the range for write (MADV_POPULATE_WRITE)\n"
#endif
#ifdef MADV_PAGEOUT
" -X -- reclaim the range (MADV_PAGEOUT)\n"
#endif
" Notes:\n"
"   NORMAL sets the default readahead setting on the file.\n"
"   RANDOM sets the readahead setting on the file to zero.\n"
"   SEQUENTIAL sets double the default readahead setting on the file.\n"
"   WILLNEED forces the maximum readahead.\n"
"\n"));
}

static int
madvise_f(
	int		argc,
	char		**argv)
{
	off_t		offset, llength;
	size_t		length;
	void		*start;
	int		advise = MADV_NORMAL, c;
	size_t		blocksize, sectsize;

	while ((c = getopt(argc, argv, "cdDfhHmMopPrRswWX")) != EOF) {
		switch (c) {
#ifdef MADV_COLLAPSE
		case 'c':	/* collapse to thp */
			advise = MADV_COLLAPSE;
			break;
#endif
		case 'd':	/* Don't need these pages */
			advise = MADV_DONTNEED;
			break;
#ifdef MADV_COLD
		case 'D':	/* make more likely to be reclaimed */
			advise = MADV_COLD;
			break;
#endif
		case 'f':	/* page range out of memory */
			advise = MADV_FREE;
			break;
		case 'h':	/* enable thp memory */
			advise = MADV_HUGEPAGE;
			break;
		case 'H':	/* disable thp memory */
			advise = MADV_NOHUGEPAGE;
			break;
		case 'm':	/* enable merging */
			advise = MADV_MERGEABLE;
			break;
		case 'M':	/* disable merging */
			advise = MADV_UNMERGEABLE;
			break;
#ifdef MADV_SOFT_OFFLINE
/* MADV_SOFT_OFFLINE is undefined on mips as if Linux 6.9 */
		case 'o':	/* offline */
			advise = MADV_SOFT_OFFLINE;
			break;
#endif
		case 'p':	/* punch hole */
			advise = MADV_REMOVE;
			break;
		case 'P':	/* poison */
			advise = MADV_HWPOISON;
			break;
		case 'r':	/* Expect random page references */
			advise = MADV_RANDOM;
			break;
#ifdef MADV_POPULATE_READ
		case 'R':	/* fault in pages for read */
			advise = MADV_POPULATE_READ;
			break;
#endif
		case 's':	/* Expect sequential page references */
			advise = MADV_SEQUENTIAL;
			break;
		case 'w':	/* Will need these pages */
			advise = MADV_WILLNEED;
			break;
#ifdef MADV_POPULATE_WRITE
		case 'W':	/* fault in pages for write */
			advise = MADV_POPULATE_WRITE;
			break;
#endif
#ifdef MADV_PAGEOUT
		case 'X':	/* reclaim memory */
			advise = MADV_PAGEOUT;
			break;
#endif
		default:
			exitcode = 1;
			return command_usage(&madvise_cmd);
		}
	}

	if (optind == argc) {
		offset = mapping->offset;
		length = mapping->length;
	} else if (optind == argc - 2) {
		init_cvtnum(&blocksize, &sectsize);
		offset = cvtnum(blocksize, sectsize, argv[optind]);
		if (offset < 0) {
			printf(_("non-numeric offset argument -- %s\n"),
				argv[optind]);
			exitcode = 1;
			return 0;
		}
		optind++;
		llength = cvtnum(blocksize, sectsize, argv[optind]);
		if (llength < 0) {
			printf(_("non-numeric length argument -- %s\n"),
				argv[optind]);
			exitcode = 1;
			return 0;
		} else if (llength > (size_t)llength) {
			printf(_("length argument too large -- %lld\n"),
				(long long)llength);
			exitcode = 1;
			return 0;
		} else
			length = (size_t)llength;
	} else {
		exitcode = 1;
		return command_usage(&madvise_cmd);
	}

	start = check_mapping_range(mapping, offset, length, 1);
	if (!start) {
		exitcode = 1;
		return 0;
	}

	if (madvise(start, length, advise) < 0) {
		perror("madvise");
		exitcode = 1;
		return 0;
	}
	return 0;
}

void
madvise_init(void)
{
	madvise_cmd.name = "madvise";
	madvise_cmd.altname = "ma";
	madvise_cmd.cfunc = madvise_f;
	madvise_cmd.argmin = 0;
	madvise_cmd.argmax = -1;
	madvise_cmd.flags = CMD_NOFILE_OK | CMD_FOREIGN_OK;
	madvise_cmd.args = _("[-drsw] [off len]");
	madvise_cmd.oneline = _("give advice about use of memory");
	madvise_cmd.help = madvise_help;

	add_command(&madvise_cmd);
}
