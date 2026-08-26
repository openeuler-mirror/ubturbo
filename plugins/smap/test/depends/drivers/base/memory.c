/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Description: linux memory.c stub
 */
#include <linux/notifier.h>

int register_memory_notifier(struct notifier_block *nb)
{
        return 0;
}

void unregister_memory_notifier(struct notifier_block *nb)
{}
