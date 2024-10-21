// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */

/* Dummy program to test C++ compilation of user-exported xfs headers */

extern "C" {
#include "include/xfs.h"
#include "include/handle.h"
#include "include/jdm.h"
};
