// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_DB_BLOCK_H
#define __XFS_DB_BLOCK_H

struct field;

extern void	block_init(void);
extern void	print_block(const struct field *fields, int argc, char **argv);

#endif /* __XFS_DB_BLOCK_H */
