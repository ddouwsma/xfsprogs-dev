// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */

struct field;

#define	szof(x,y)	sizeof(((x *)0)->y)
#define	szcount(x,y)	(szof(x,y) / szof(x,y[0]))

typedef enum typnm
{
	TYP_AGF,
	TYP_AGFL,
	TYP_AGI,
	TYP_ATTR,
	TYP_BMAPBTA,
	TYP_BMAPBTD,
	TYP_BNOBT,
	TYP_CNTBT,
	TYP_RMAPBT,
	TYP_RTRMAPBT,
	TYP_REFCBT,
	TYP_RTREFCBT,
	TYP_DATA,
	TYP_DIR2,
	TYP_DQBLK,
	TYP_INOBT,
	TYP_INODATA,
	TYP_INODE,
	TYP_LOG,
	TYP_RTBITMAP,
	TYP_RTSUMMARY,
	TYP_RTSB,
	TYP_SB,
	TYP_SYMLINK,
	TYP_TEXT,
	TYP_FINOBT,
	TYP_RGBITMAP,
	TYP_RGSUMMARY,
	TYP_NONE
} typnm_t;

#define DB_FUZZ  2
#define DB_WRITE 1
#define DB_READ  0

typedef void (*opfunc_t)(const struct field *fld, int argc, char **argv);
typedef void (*pfunc_t)(int action, const struct field *fld, int argc, char **argv);

typedef struct typ
{
	typnm_t			typnm;
	char			*name;
	pfunc_t			pfunc;
	const struct field	*fields;
	const struct xfs_buf_ops *bops;
	unsigned long		crc_off;
#define TYP_F_NO_CRC_OFF	(-1UL)
#define TYP_F_CRC_FUNC		(-2UL)
	void			(*set_crc)(struct xfs_buf *);
} typ_t;
extern const typ_t	*typtab, *cur_typ;

extern void	type_init(void);
extern void	type_set_tab_crc(void);
extern void	type_set_tab_spcrc(void);
extern void	handle_block(int action, const struct field *fields, int argc,
			     char **argv);
extern void	handle_string(int action, const struct field *fields, int argc,
			      char **argv);
extern void	handle_struct(int action, const struct field *fields, int argc,
			      char **argv);
extern void	handle_text(int action, const struct field *fields, int argc,
			    char **argv);
