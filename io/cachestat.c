// SPDX-License-Identifier: GPL-2.0

#include "command.h"
#include "input.h"
#include "init.h"
#include "io.h"
#include <unistd.h>
#include <linux/mman.h>
#include <asm/unistd.h>

static cmdinfo_t cachestat_cmd;

static void print_cachestat(struct cachestat *cs)
{
	printf(_("Cached: %llu, Dirty: %llu, Writeback: %llu, Evicted: %llu, Recently Evicted: %llu\n"),
			cs->nr_cache, cs->nr_dirty, cs->nr_writeback,
			cs->nr_evicted, cs->nr_recently_evicted);
}

static int
cachestat_f(int argc, char **argv)
{
	off_t offset = 0, length = 0;
	size_t blocksize, sectsize;
	struct cachestat_range cs_range;
	struct cachestat cs;

	if (argc != 3) {
		exitcode = 1;
		return command_usage(&cachestat_cmd);
	}

	init_cvtnum(&blocksize, &sectsize);
	offset = cvtnum(blocksize, sectsize, argv[1]);
	if (offset < 0) {
		printf(_("invalid offset argument -- %s\n"), argv[1]);
		exitcode = 1;
		return 0;
	}

	length = cvtnum(blocksize, sectsize, argv[2]);
	if (length < 0) {
		printf(_("invalid length argument -- %s\n"), argv[2]);
		exitcode = 1;
		return 0;
	}

	cs_range.off = offset;
	cs_range.len = length;

	if (syscall(__NR_cachestat, file->fd, &cs_range, &cs, 0)) {
		perror("cachestat");
		exitcode = 1;
		return 0;
	}

	print_cachestat(&cs);

	return 0;
}

static cmdinfo_t cachestat_cmd = {
	.name		= "cachestat",
	.altname	= "cs",
	.cfunc		= cachestat_f,
	.argmin		= 2,
	.argmax		= 2,
	.flags		= CMD_NOMAP_OK | CMD_FOREIGN_OK,
	.args		= "off len",
	.oneline	= "find page cache pages for a given file",
};

void cachestat_init(void)
{
	add_command(&cachestat_cmd);
}

