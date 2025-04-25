// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Christoph Hellwig.
 */
#include <ctype.h>
#include <linux/blkzoned.h>
#include "libxfs_priv.h"
#include "libxfs.h"
#include "xfs_zones.h"
#include "err_protos.h"
#include "zoned.h"

/* random size that allows efficient processing */
#define ZONES_PER_IOCTL			16384

static void
report_zones_cb(
	struct xfs_mount	*mp,
	struct blk_zone		*zone)
{
	xfs_rtblock_t		zsbno = xfs_daddr_to_rtb(mp, zone->start);
	xfs_rgblock_t		write_pointer;
	xfs_rgnumber_t		rgno;
	struct xfs_rtgroup	*rtg;

	if (xfs_rtb_to_rgbno(mp, zsbno) != 0) {
		do_error(_("mismatched zone start 0x%llx."),
				(unsigned long long)zsbno);
		return;
	}

	rgno = xfs_rtb_to_rgno(mp, zsbno);
	rtg = libxfs_rtgroup_grab(mp, rgno);
	if (!rtg) {
		do_error(_("realtime group not found for zone %u."), rgno);
		return;
	}

	if (!rtg_rmap(rtg))
		do_warn(_("no rmap inode for zone %u."), rgno);
	else
		libxfs_zone_validate(zone, rtg, &write_pointer);
	libxfs_rtgroup_rele(rtg);
}

void
check_zones(
	struct xfs_mount	*mp)
{
	int			fd = mp->m_rtdev_targp->bt_bdev_fd;
	uint64_t		sector = XFS_FSB_TO_BB(mp, mp->m_sb.sb_rtstart);
	unsigned int		zone_size, zone_capacity;
	uint64_t		device_size;
	size_t			rep_size;
	struct blk_zone_report	*rep;
	unsigned int		i, n = 0;

	if (ioctl(fd, BLKGETSIZE64, &device_size))
		return; /* not a block device */
	if (ioctl(fd, BLKGETZONESZ, &zone_size) || !zone_size)
		return;	/* not zoned */

	/* BLKGETSIZE64 reports a byte value */
	device_size = BTOBB(device_size);
	if (device_size / zone_size < mp->m_sb.sb_rgcount) {
		do_error(_("rt device too small\n"));
		return;
	}

	rep_size = sizeof(struct blk_zone_report) +
		   sizeof(struct blk_zone) * ZONES_PER_IOCTL;
	rep = malloc(rep_size);
	if (!rep) {
		do_warn(_("malloc failed for zone report\n"));
		return;
	}

	while (n < mp->m_sb.sb_rgcount) {
		struct blk_zone *zones = (struct blk_zone *)(rep + 1);
		int ret;

		memset(rep, 0, rep_size);
		rep->sector = sector;
		rep->nr_zones = ZONES_PER_IOCTL;

		ret = ioctl(fd, BLKREPORTZONE, rep);
		if (ret) {
			do_error(_("ioctl(BLKREPORTZONE) failed: %d!\n"), ret);
			goto out_free;
		}
		if (!rep->nr_zones)
			break;

		for (i = 0; i < rep->nr_zones; i++) {
			if (n >= mp->m_sb.sb_rgcount)
				break;

			if (zones[i].len != zone_size) {
				do_error(_("Inconsistent zone size!\n"));
				goto out_free;
			}

			switch (zones[i].type) {
			case BLK_ZONE_TYPE_CONVENTIONAL:
			case BLK_ZONE_TYPE_SEQWRITE_REQ:
				break;
			case BLK_ZONE_TYPE_SEQWRITE_PREF:
				do_error(
_("Found sequential write preferred zone\n"));
				goto out_free;
			default:
				do_error(
_("Found unknown zone type (0x%x)\n"), zones[i].type);
				goto out_free;
			}

			if (!n) {
				zone_capacity = zones[i].capacity;
				if (zone_capacity > zone_size) {
					do_error(
_("Zone capacity larger than zone size!\n"));
					goto out_free;
				}
			} else if (zones[i].capacity != zone_capacity) {
				do_error(
_("Inconsistent zone capacity!\n"));
				goto out_free;
			}

			report_zones_cb(mp, &zones[i]);
			n++;
		}
		sector = zones[rep->nr_zones - 1].start +
			 zones[rep->nr_zones - 1].len;
	}

out_free:
	free(rep);
}
