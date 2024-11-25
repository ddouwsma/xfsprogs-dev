// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef _XFS_REPAIR_RT_H_
#define _XFS_REPAIR_RT_H_

void generate_rtinfo(struct xfs_mount *mp);
void check_rtbitmap(struct xfs_mount *mp);
void check_rtsummary(struct xfs_mount *mp);

void fill_rtbitmap(struct xfs_rtgroup *rtg);
void fill_rtsummary(struct xfs_rtgroup *rtg);

#endif /* _XFS_REPAIR_RT_H_ */
