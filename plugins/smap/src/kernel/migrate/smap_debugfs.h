/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP debugfs header
 */

#ifndef SMAP_DEBUGFS_H
#define SMAP_DEBUGFS_H

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/fs.h>

int smap_debugfs_migrate_init(void);

void smap_debugfs_mod_exit(void);

#endif