// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "libxfs.h"
#include "libxlog.h"

#include "logprint.h"

/*
 * Start is defined to be the block pointing to the oldest valid log record.
 */
int
xlog_print_find_oldest(
	struct xlog	*log,
	xfs_daddr_t	*last_blk)
{
	struct xfs_buf	*bp;
	xfs_daddr_t	first_blk;
	uint		first_half_cycle, last_half_cycle;
	int		error = 0;

	if (xlog_find_zeroed(log, &first_blk))
		return 0;

	first_blk = 0;		/* read first block */
	bp = xlog_get_bp(log, 1);
	xlog_bread_noalign(log, 0, 1, bp);
	first_half_cycle = xlog_get_cycle(bp->b_addr);
	*last_blk = log->l_logBBsize-1;	/* read last block */
	xlog_bread_noalign(log, *last_blk, 1, bp);
	last_half_cycle = xlog_get_cycle(bp->b_addr);
	ASSERT(last_half_cycle != 0);

	if (first_half_cycle == last_half_cycle) /* all cycle nos are same */
		*last_blk = 0;
	else		/* have 1st and last; look for middle cycle */
		error = xlog_find_cycle_start(log, bp, first_blk,
					      last_blk, last_half_cycle);

	libxfs_buf_relse(bp);
	return error;
}

void
xlog_recover_print_data(
	char		*p,
	int		len)
{
	if (print_data) {
		uint *dp  = (uint *)p;
		int  nums = len >> 2;
		int  j = 0;

		while (j < nums) {
			if ((j % 8) == 0)
				printf("%2x ", j);
			printf("%8x ", *dp);
			dp++;
			j++;
			if ((j % 8) == 0)
				printf("\n");
		}
		printf("\n");
	}
}

STATIC void
xlog_recover_print_buffer(
	struct xlog_recover_item *item)
{
	xfs_agi_t		*agi;
	xfs_agf_t		*agf;
	xfs_buf_log_format_t	*f;
	char			*p;
	int			len, num, i;
	xfs_daddr_t		blkno;
	struct xfs_disk_dquot	*ddq;

	f = (xfs_buf_log_format_t *)item->ri_buf[0].i_addr;
	printf("	");
	ASSERT(f->blf_type == XFS_LI_BUF);
	printf(_("BUF:  #regs:%d   start blkno:0x%llx   len:%d   bmap size:%d   flags:0x%x\n"),
		f->blf_size, (long long)f->blf_blkno, f->blf_len, f->blf_map_size, f->blf_flags);
	blkno = (xfs_daddr_t)f->blf_blkno;
	num = f->blf_size-1;
	i = 1;
	while (num-- > 0) {
		p = item->ri_buf[i].i_addr;
		len = item->ri_buf[i].i_len;
		i++;
		if (blkno == 0) { /* super block */
			struct xfs_dsb  *dsb = (struct xfs_dsb *)p;

			printf(_("	SUPER Block Buffer:\n"));
			if (!print_buffer)
				continue;
			printf(_("              icount:%llu ifree:%llu  "),
				(unsigned long long) get_unaligned_be64(&dsb->sb_icount),
				(unsigned long long) get_unaligned_be64(&dsb->sb_ifree));
			printf(_("fdblks:%llu  frext:%llu\n"),
				(unsigned long long) get_unaligned_be64(&dsb->sb_fdblocks),
				(unsigned long long) get_unaligned_be64(&dsb->sb_frextents));
			printf(_("		sunit:%u  swidth:%u\n"),
				get_unaligned_be32(&dsb->sb_unit),
				get_unaligned_be32(&dsb->sb_width));
		} else if (be32_to_cpu(*(__be32 *)p) == XFS_AGI_MAGIC) {
			int bucket, buckets;
			agi = (xfs_agi_t *)p;
			printf(_("	AGI Buffer: (XAGI)\n"));
			if (!print_buffer)
				continue;
			printf(_("		ver:%d  "),
				be32_to_cpu(agi->agi_versionnum));
			printf(_("seq#:%d  len:%d  cnt:%d  root:%d\n"),
				be32_to_cpu(agi->agi_seqno),
				be32_to_cpu(agi->agi_length),
				be32_to_cpu(agi->agi_count),
				be32_to_cpu(agi->agi_root));
			printf(_("		level:%d  free#:0x%x  newino:0x%x\n"),
				be32_to_cpu(agi->agi_level),
				be32_to_cpu(agi->agi_freecount),
				be32_to_cpu(agi->agi_newino));
			if (len == 128) {
				buckets = 17;
			} else if (len == 256) {
				buckets = 32 + 17;
			} else {
				buckets = XFS_AGI_UNLINKED_BUCKETS;
			}
			for (bucket = 0; bucket < buckets;) {
				int col;
				printf(_("bucket[%d - %d]: "), bucket, bucket+3);
				for (col = 0; col < 4; col++, bucket++) {
					if (bucket < buckets) {
						printf("0x%x ",
			be32_to_cpu(agi->agi_unlinked[bucket]));
					}
				}
				printf("\n");
			}
		} else if (be32_to_cpu(*(__be32 *)p) == XFS_AGF_MAGIC) {
			agf = (xfs_agf_t *)p;
			printf(_("	AGF Buffer: (XAGF)\n"));
			if (!print_buffer)
				continue;
			printf(_("		ver:%d  seq#:%d  len:%d  \n"),
				be32_to_cpu(agf->agf_versionnum),
				be32_to_cpu(agf->agf_seqno),
				be32_to_cpu(agf->agf_length));
			printf(_("		root BNO:%d  CNT:%d\n"),
				be32_to_cpu(agf->agf_bno_root),
				be32_to_cpu(agf->agf_cnt_root));
			printf(_("		level BNO:%d  CNT:%d\n"),
				be32_to_cpu(agf->agf_bno_level),
				be32_to_cpu(agf->agf_cnt_level));
			printf(_("		1st:%d  last:%d  cnt:%d  "
				"freeblks:%d  longest:%d\n"),
				be32_to_cpu(agf->agf_flfirst),
				be32_to_cpu(agf->agf_fllast),
				be32_to_cpu(agf->agf_flcount),
				be32_to_cpu(agf->agf_freeblks),
				be32_to_cpu(agf->agf_longest));
		} else if (*(uint *)p == XFS_DQUOT_MAGIC) {
			ddq = (struct xfs_disk_dquot *)p;
			printf(_("	DQUOT Buffer:\n"));
			if (!print_buffer)
				continue;
			printf(_("		UIDs 0x%lx-0x%lx\n"),
			       (unsigned long)be32_to_cpu(ddq->d_id),
			       (unsigned long)be32_to_cpu(ddq->d_id) +
			       (BBTOB(f->blf_len) / sizeof(struct xfs_dqblk)) - 1);
		} else {
			printf(_("	BUF DATA\n"));
			if (!print_buffer) continue;
			xlog_recover_print_data(p, len);
		}
	}
}

STATIC void
xlog_recover_print_quotaoff(
	struct xlog_recover_item *item)
{
	xfs_qoff_logformat_t	*qoff_f;

	qoff_f = (xfs_qoff_logformat_t *)item->ri_buf[0].i_addr;

	ASSERT(qoff_f);
	printf(_("\tQUOTAOFF: #regs:%d   type:"), qoff_f->qf_size);
	if (qoff_f->qf_flags & XFS_UQUOTA_ACCT)
		printf(" USER");
	if (qoff_f->qf_flags & XFS_GQUOTA_ACCT)
		printf(" GROUP");
	if (qoff_f->qf_flags & XFS_PQUOTA_ACCT)
		printf(" PROJECT");
	printf("\n");
}

STATIC void
xlog_recover_print_dquot(
	struct xlog_recover_item *item)
{
	xfs_dq_logformat_t	*f;
	struct xfs_disk_dquot	*d;

	f = (xfs_dq_logformat_t *)item->ri_buf[0].i_addr;
	ASSERT(f);
	ASSERT(f->qlf_len == 1);
	d = (struct xfs_disk_dquot *)item->ri_buf[1].i_addr;
	printf(_("\tDQUOT: #regs:%d  blkno:%lld  boffset:%u id: %d\n"),
	       f->qlf_size, (long long)f->qlf_blkno, f->qlf_boffset, f->qlf_id);
	if (!print_quota)
		return;
	printf(_("\t\tmagic 0x%x\tversion 0x%x\tID 0x%x (%d)\t\n"),
	       be16_to_cpu(d->d_magic),
	       d->d_version,
	       be32_to_cpu(d->d_id),
	       be32_to_cpu(d->d_id));
	printf(_("\t\tblk_hard 0x%x\tblk_soft 0x%x\tino_hard 0x%x"
	       "\tino_soft 0x%x\n"),
	       (int)be64_to_cpu(d->d_blk_hardlimit),
	       (int)be64_to_cpu(d->d_blk_softlimit),
	       (int)be64_to_cpu(d->d_ino_hardlimit),
	       (int)be64_to_cpu(d->d_ino_softlimit));
	printf(_("\t\tbcount 0x%x (%d) icount 0x%x (%d)\n"),
	       (int)be64_to_cpu(d->d_bcount),
	       (int)be64_to_cpu(d->d_bcount),
	       (int)be64_to_cpu(d->d_icount),
	       (int)be64_to_cpu(d->d_icount));
	printf(_("\t\tbtimer 0x%x itimer 0x%x \n"),
	       (int)be32_to_cpu(d->d_btimer),
	       (int)be32_to_cpu(d->d_itimer));
}

STATIC void
xlog_recover_print_inode_core(
	struct xfs_log_dinode	*di)
{
	xfs_extnum_t		nextents;
	xfs_aextnum_t		anextents;

	printf(_("	CORE inode:\n"));
	if (!print_inode)
		return;

	if (di->di_flags2 & XFS_DIFLAG2_NREXT64) {
		nextents = di->di_big_nextents;
		anextents = di->di_big_anextents;
	} else {
		nextents = di->di_nextents;
		anextents = di->di_anextents;
	}

	printf(_("		magic:%c%c  mode:0x%x  ver:%d  format:%d\n"),
	       (di->di_magic>>8) & 0xff, di->di_magic & 0xff,
	       di->di_mode, di->di_version, di->di_format);
	printf(_("		uid:%d  gid:%d  nlink:%d projid:0x%04x%04x\n"),
	       di->di_uid, di->di_gid, di->di_nlink,
	       di->di_projid_hi, di->di_projid_lo);
	printf(_("		atime:%lld  mtime:%lld  ctime:%lld\n"),
			xlog_extract_dinode_ts(di->di_atime),
			xlog_extract_dinode_ts(di->di_mtime),
			xlog_extract_dinode_ts(di->di_ctime));
	printf(_("		flushiter:%d\n"), di->di_flushiter);
	printf(_("		size:0x%llx  nblks:0x%llx  exsize:%d  "
	     "nextents:%" PRIu64 "  anextents:%" PRIu32 "\n"), (unsigned long long)
	       di->di_size, (unsigned long long)di->di_nblocks,
	       di->di_extsize, nextents, anextents);
	printf(_("		forkoff:%d  dmevmask:0x%x  dmstate:%d  flags:0x%x  "
	     "gen:%u\n"),
	       (int)di->di_forkoff, di->di_dmevmask, (int)di->di_dmstate,
	       (int)di->di_flags, di->di_gen);
	if (di->di_version == 3) {
		printf(_("		flags2 0x%llx cowextsize 0x%x\n"),
			(unsigned long long)di->di_flags2, di->di_cowextsize);
	}
}

STATIC void
xlog_recover_print_inode(
	struct xlog_recover_item *item)
{
	struct xfs_inode_log_format	f_buf;
	struct xfs_inode_log_format	*f;
	int			attr_index;
	int			hasdata;
	int			hasattr;

	ASSERT(item->ri_buf[0].i_len == sizeof(struct xfs_inode_log_format_32) ||
	       item->ri_buf[0].i_len == sizeof(struct xfs_inode_log_format));
	f = xfs_inode_item_format_convert(item->ri_buf[0].i_addr, item->ri_buf[0].i_len, &f_buf);

	printf(_("	INODE: #regs:%d   ino:0x%llx  flags:0x%x   dsize:%d\n"),
	       f->ilf_size, (unsigned long long)f->ilf_ino, f->ilf_fields,
	       f->ilf_dsize);

	/* core inode comes 2nd */
	/* ASSERT len vs xfs_log_dinode_size() for V3 or V2 inodes */
	ASSERT(item->ri_buf[1].i_len ==
			offsetof(struct xfs_log_dinode, di_next_unlinked) ||
	       item->ri_buf[1].i_len == sizeof(struct xfs_log_dinode));
	xlog_recover_print_inode_core((struct xfs_log_dinode *)
				      item->ri_buf[1].i_addr);

	hasdata = (f->ilf_fields & XFS_ILOG_DFORK) != 0;
	hasattr = (f->ilf_fields & XFS_ILOG_AFORK) != 0;
	/* does anything come next */
	switch (f->ilf_fields & (XFS_ILOG_DFORK|XFS_ILOG_DEV|XFS_ILOG_UUID)) {
	case XFS_ILOG_DEXT:
		ASSERT(f->ilf_size == 3 + hasattr);
		printf(_("		DATA FORK EXTENTS inode data:\n"));
		if (print_inode && print_data)
			xlog_recover_print_data(item->ri_buf[2].i_addr,
						item->ri_buf[2].i_len);
		break;
	case XFS_ILOG_DBROOT:
		ASSERT(f->ilf_size == 3 + hasattr);
		printf(_("		DATA FORK BTREE inode data:\n"));
		if (print_inode && print_data)
			xlog_recover_print_data(item->ri_buf[2].i_addr,
						item->ri_buf[2].i_len);
		break;
	case XFS_ILOG_DDATA:
		ASSERT(f->ilf_size == 3 + hasattr);
		printf(_("		DATA FORK LOCAL inode data:\n"));
		if (print_inode && print_data)
			xlog_recover_print_data(item->ri_buf[2].i_addr,
						item->ri_buf[2].i_len);
		break;
	case XFS_ILOG_DEV:
		ASSERT(f->ilf_size == 2 + hasattr);
		printf(_("		DEV inode: no extra region\n"));
		break;
	case XFS_ILOG_UUID:
		ASSERT(f->ilf_size == 2 + hasattr);
		printf(_("		UUID inode: no extra region\n"));
		break;

	case 0:
		ASSERT(f->ilf_size == 2 + hasattr);
		break;
	default:
		xlog_panic("xlog_print_trans_inode: illegal inode type");
	}

	if (hasattr) {
		attr_index = 2 + hasdata;
		switch (f->ilf_fields & XFS_ILOG_AFORK) {
		case XFS_ILOG_AEXT:
			ASSERT(f->ilf_size == 3 + hasdata);
			printf(_("		ATTR FORK EXTENTS inode data:\n"));
			if (print_inode && print_data)
				xlog_recover_print_data(
					item->ri_buf[attr_index].i_addr,
					item->ri_buf[attr_index].i_len);
			break;
		case XFS_ILOG_ABROOT:
			ASSERT(f->ilf_size == 3 + hasdata);
			printf(_("		ATTR FORK BTREE inode data:\n"));
			if (print_inode && print_data)
				xlog_recover_print_data(
					item->ri_buf[attr_index].i_addr,
					item->ri_buf[attr_index].i_len);
			break;
		case XFS_ILOG_ADATA:
			ASSERT(f->ilf_size == 3 + hasdata);
			printf(_("		ATTR FORK LOCAL inode data:\n"));
			if (print_inode && print_data)
				xlog_recover_print_data(
					item->ri_buf[attr_index].i_addr,
					item->ri_buf[attr_index].i_len);
			break;
		default:
			xlog_panic("%s: illegal inode log flag", __FUNCTION__);
		}
	}
}


STATIC void
xlog_recover_print_icreate(
	struct xlog_recover_item	*item)
{
	struct xfs_icreate_log	*icl;

	icl = (struct xfs_icreate_log *)item->ri_buf[0].i_addr;

	printf(_("	ICR:  #ag: %d  agbno: 0x%x  len: %d\n"
		 "	      cnt: %d  isize: %d    gen: 0x%x\n"),
		be32_to_cpu(icl->icl_ag), be32_to_cpu(icl->icl_agbno),
		be32_to_cpu(icl->icl_length), be32_to_cpu(icl->icl_count),
		be32_to_cpu(icl->icl_isize), be32_to_cpu(icl->icl_gen));
}

void
xlog_recover_print_logitem(
	struct xlog_recover_item	*item)
{
	switch (ITEM_TYPE(item)) {
	case XFS_LI_BUF:
		xlog_recover_print_buffer(item);
		break;
	case XFS_LI_ICREATE:
		xlog_recover_print_icreate(item);
		break;
	case XFS_LI_INODE:
		xlog_recover_print_inode(item);
		break;
	case XFS_LI_EFD_RT:
	case XFS_LI_EFD:
		xlog_recover_print_efd(item);
		break;
	case XFS_LI_EFI_RT:
	case XFS_LI_EFI:
		xlog_recover_print_efi(item);
		break;
	case XFS_LI_ATTRD:
		xlog_recover_print_attrd(item);
		break;
	case XFS_LI_ATTRI:
		xlog_recover_print_attri(item);
		break;
	case XFS_LI_RUD_RT:
	case XFS_LI_RUD:
		xlog_recover_print_rud(item);
		break;
	case XFS_LI_RUI_RT:
	case XFS_LI_RUI:
		xlog_recover_print_rui(item);
		break;
	case XFS_LI_CUD_RT:
	case XFS_LI_CUD:
		xlog_recover_print_cud(item);
		break;
	case XFS_LI_CUI_RT:
	case XFS_LI_CUI:
		xlog_recover_print_cui(item);
		break;
	case XFS_LI_BUD:
		xlog_recover_print_bud(item);
		break;
	case XFS_LI_BUI:
		xlog_recover_print_bui(item);
		break;
	case XFS_LI_XMD:
		xlog_recover_print_xmd(item);
		break;
	case XFS_LI_XMI:
		xlog_recover_print_xmi(item);
		break;
	case XFS_LI_DQUOT:
		xlog_recover_print_dquot(item);
		break;
	case XFS_LI_QUOTAOFF:
		xlog_recover_print_quotaoff(item);
		break;
	default:
		printf(_("xlog_recover_print_logitem: illegal type\n"));
		break;
	}
}

static void
xlog_recover_print_item(
	struct xlog_recover_item *item)
{
	int			i;

	switch (ITEM_TYPE(item)) {
	case XFS_LI_BUF:
		printf("BUF");
		break;
	case XFS_LI_ICREATE:
		printf("ICR");
		break;
	case XFS_LI_INODE:
		printf("INO");
		break;
	case XFS_LI_EFD_RT:
		printf("EFD_RT");
		break;
	case XFS_LI_EFI_RT:
		printf("EFI_RT");
		break;
	case XFS_LI_EFD:
		printf("EFD");
		break;
	case XFS_LI_EFI:
		printf("EFI");
		break;
	case XFS_LI_ATTRD:
		printf("ATTRD");
		break;
	case XFS_LI_ATTRI:
		printf("ATTRI");
		break;
	case XFS_LI_RUD:
		printf("RUD");
		break;
	case XFS_LI_RUI:
		printf("RUI");
		break;
	case XFS_LI_RUD_RT:
		printf("RUD_RT");
		break;
	case XFS_LI_RUI_RT:
		printf("RUI_RT");
		break;
	case XFS_LI_CUD:
		printf("CUD");
		break;
	case XFS_LI_CUI:
		printf("CUI");
		break;
	case XFS_LI_CUD_RT:
		printf("CUD_RT");
		break;
	case XFS_LI_CUI_RT:
		printf("CUI_RT");
		break;
	case XFS_LI_BUD:
		printf("BUD");
		break;
	case XFS_LI_BUI:
		printf("BUI");
		break;
	case XFS_LI_XMD:
		printf("XMD");
		break;
	case XFS_LI_XMI:
		printf("XMI");
		break;
	case XFS_LI_DQUOT:
		printf("DQ ");
		break;
	case XFS_LI_QUOTAOFF:
		printf("QOFF");
		break;
	default:
		cmn_err(CE_PANIC, _("%s: illegal type"), __FUNCTION__);
		break;
	}

/*	type isn't filled in yet
	printf(_("ITEM: type: %d cnt: %d total: %d "),
	       item->ri_type, item->ri_cnt, item->ri_total);
*/
	printf(_(": cnt:%d total:%d "), item->ri_cnt, item->ri_total);
	for (i=0; i<item->ri_cnt; i++) {
		printf(_("a:0x%lx len:%d "),
		       (long)item->ri_buf[i].i_addr, item->ri_buf[i].i_len);
	}
	printf("\n");
	xlog_recover_print_logitem(item);
}

void
xlog_recover_print_trans(
	struct xlog_recover	*trans,
	struct list_head	*itemq,
	int			print)
{
	struct xlog_recover_item *item;

	if (print < 3)
		return;

	print_xlog_record_line();
	xlog_recover_print_trans_head(trans);
	list_for_each_entry(item, itemq, ri_list)
		xlog_recover_print_item(item);
}
