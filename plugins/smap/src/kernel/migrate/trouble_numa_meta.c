// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rwlock.h>

#include "trouble_numa_meta.h"

struct numa_node {
    u16 numa_id;
    struct list_head list;
};

struct numa_list_manager {
    struct list_head head;
    rwlock_t lock;
};

static struct numa_list_manager g_manager;

void init_trouble_numa_manager(void)
{
    INIT_LIST_HEAD(&g_manager.head);
    rwlock_init(&g_manager.lock);
}

void cleanup_trouble_numa_manager(void)
{
    struct numa_node *node, *tmp;
    unsigned long flags;

    write_lock_irqsave(&g_manager.lock, flags);
    list_for_each_entry_safe(node, tmp, &g_manager.head, list) {
        list_del(&node->list);
        kfree(node);
    }
    write_unlock_irqrestore(&g_manager.lock, flags);
}

int trouble_numa_list_add(u16 numa_id)
{
    struct numa_node *node, *tmp;
    unsigned long irq_flags;
    int ret = 0;

    write_lock_irqsave(&g_manager.lock, irq_flags);
    list_for_each_entry(tmp, &g_manager.head, list) {
        if (tmp->numa_id == numa_id) {
            pr_warn("NUMA ID %u already exists\n", numa_id);
            ret = -EEXIST;
            goto out;
        }
    }

    node = kmalloc(sizeof(*node), GFP_ATOMIC);
    if (!node) {
        pr_err("Failed to allocate memory for NUMA ID %u\n", numa_id);
        ret = -ENOMEM;
        goto out;
    }

    node->numa_id = numa_id;
    INIT_LIST_HEAD(&node->list);

    list_add_tail(&node->list, &g_manager.head);
out:
    write_unlock_irqrestore(&g_manager.lock, irq_flags);
    return ret;
}

int trouble_numa_list_del(u16 numa_id)
{
    struct numa_node *node, *tmp;
    unsigned long irq_flags;
    int ret = -ENOENT;

    write_lock_irqsave(&g_manager.lock, irq_flags);

    list_for_each_entry_safe(node, tmp, &g_manager.head, list) {
        if (node->numa_id == numa_id) {
            list_del(&node->list);
            kfree(node);
            ret = 0;
            break;
        }
    }

    write_unlock_irqrestore(&g_manager.lock, irq_flags);

    if (ret != 0) {
        pr_warn("NUMA ID %u not found for deletion\n", numa_id);
    }

    return ret;
}

void trouble_numa_list_del_all(void)
{
    struct numa_node *node, *tmp;
    unsigned long irq_flags;
    write_lock_irqsave(&g_manager.lock, irq_flags);

    list_for_each_entry_safe(node, tmp, &g_manager.head, list) {
        list_del(&node->list);
        kfree(node);
    }

    write_unlock_irqrestore(&g_manager.lock, irq_flags);
}

int is_trouble_numa_in_list(u16 numa_id)
{
    struct numa_node *node;
    unsigned long irq_flags;

    read_lock_irqsave(&g_manager.lock, irq_flags);

    list_for_each_entry(node, &g_manager.head, list) {
        if (node->numa_id == numa_id) {
            read_unlock_irqrestore(&g_manager.lock, irq_flags);
            return 1;
        }
    }

    read_unlock_irqrestore(&g_manager.lock, irq_flags);
    return 0;
}

int is_trouble_numa_list_empty(void)
{
    unsigned long irq_flags;
    int ret = 0;

    read_lock_irqsave(&g_manager.lock, irq_flags);
    if (list_empty(&g_manager.head)) {
        ret = 1;
    }
    read_unlock_irqrestore(&g_manager.lock, irq_flags);

    return ret;
}

int is_trouble_numa(u16 numa_id)
{
    if (is_trouble_numa_list_empty()) {
        return 0;
    }

    return is_trouble_numa_in_list(numa_id);
}

void deal_trouble_numa_info(void *msg)
{
    struct numa_status_list *numa_info = (struct numa_status_list *)msg;
    int i;
    int ret;

    for (i = 0; i < numa_info->cnt; i++) {
        if (numa_info->entries[i].status == NUMA_UNAVAILABLE) {
            ret = trouble_numa_list_add(numa_info->entries[i].numa_id);
            if (ret && ret != -EEXIST) {
                pr_err("Failed to add NUMA ID %u to trouble list\n", numa_info->entries[i].numa_id);
            }
        } else if (numa_info->entries[i].status == NUMA_AVAILABLE) {
            ret = trouble_numa_list_del(numa_info->entries[i].numa_id);
            if (ret) {
                pr_err("Failed to remove NUMA ID %u from trouble list\n", numa_info->entries[i].numa_id);
            }
        } else {
            pr_warn("Unknown status %u for NUMA ID %u\n", numa_info->entries[i].status, numa_info->entries[i].numa_id);
        }
    }
}

int trouble_numa_list_get_all(u16 *buffer, size_t buf_size)
{
    struct numa_node *node;
    unsigned long irq_flags;
    int count = 0;

    read_lock_irqsave(&g_manager.lock, irq_flags);

    list_for_each_entry(node, &g_manager.head, list) {
        if (count < buf_size) {
            buffer[count++] = node->numa_id;
        } else {
            break;
        }
    }

    read_unlock_irqrestore(&g_manager.lock, irq_flags);

    return count;
}