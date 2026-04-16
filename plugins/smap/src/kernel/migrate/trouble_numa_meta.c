// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rwlock.h>
#include <linux/bitmap.h>

#include "trouble_numa_meta.h"

#undef pr_fmt
#define pr_fmt(fmt) "SMAP_trouble_numa: " fmt

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

    write_lock(&g_manager.lock);
    list_for_each_entry_safe(node, tmp, &g_manager.head, list) {
        list_del(&node->list);
        kfree(node);
    }
    write_unlock(&g_manager.lock);
}

int trouble_numa_list_add(u16 numa_id)
{
    struct numa_node *node, *tmp;
    int ret = 0;

    write_lock(&g_manager.lock);
    list_for_each_entry(tmp, &g_manager.head, list) {
        if (tmp->numa_id == numa_id) {
            pr_warn_ratelimited("Trouble NUMA ID %u already exists in trouble list\n", numa_id);
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
    list_add_tail(&node->list, &g_manager.head);
out:
    write_unlock(&g_manager.lock);
    return ret;
}

int is_trouble_numa(u16 numa_id)
{
    struct numa_node *node;

    read_lock(&g_manager.lock);
    if (list_empty(&g_manager.head)) {
        read_unlock(&g_manager.lock);
        return 0;
    }

    list_for_each_entry(node, &g_manager.head, list) {
        if (node->numa_id == numa_id) {
            read_unlock(&g_manager.lock);
            return 1;
        }
    }

    read_unlock(&g_manager.lock);
    return 0;
}

static int deal_trouble_numa_info_inner(struct numa_entry *info)
{
    u8 found = 0;
    struct numa_node *node;

    list_for_each_entry(node, &g_manager.head, list) { //todo safe
        if (node->numa_id == info->numa_id) {
            if (info->status == NUMA_AVAILABLE) {
                list_del(&node->list);
                kfree(node);
                return 0;
            }
            found = 1;
            break;
        }
    }

    if (!found && info->status == NUMA_UNAVAILABLE) {
        struct numa_node *new_node = kmalloc(sizeof(*new_node), GFP_ATOMIC);
        if (new_node) {
            new_node->numa_id = info->numa_id;
            list_add_tail(&new_node->list, &g_manager.head);
            pr_info("Added trouble NUMA ID %u to the trouble list\n", info->numa_id);
            return 0;
        } else {
            pr_err("Failed to allocate node for NUMA %d\n", info->numa_id);
            return -ENOMEM;
        }
    }

    return 0;
}

int deal_trouble_numa_info(struct numa_status_list *numa_info)
{
    struct numa_node *node, *tmp;
    unsigned long *keep_bits;
    int max_id = 0;
    int i;

    for (i = 0; i < numa_info->cnt; i++) {
        if (numa_info->entries[i].numa_id > max_id) {
            max_id = numa_info->entries[i].numa_id;
        }
    }

    keep_bits = bitmap_zalloc(max_id + 1, GFP_KERNEL);
    if (!keep_bits) {
        pr_err("Failed to allocate bitmap\n");
        return -ENOMEM;
    }

    for (i = 0; i < numa_info->cnt; i++) {
        u16 id = numa_info->entries[i].numa_id;
        __set_bit(id, keep_bits);
    }

    write_lock(&g_manager.lock);
    list_for_each_entry_safe(node, tmp, &g_manager.head, list) {
        if (node->numa_id > max_id || !test_bit(node->numa_id, keep_bits)) {
            list_del(&node->list);
            kfree(node);
        }
    }
    bitmap_free(keep_bits);

    for (i = 0; i < numa_info->cnt; i++) {
        int ret = deal_trouble_numa_info_inner(&numa_info->entries[i]);
        if (ret) {
            pr_err("Failed to deal with NUMA ID %u\n", numa_info->entries[i].numa_id);
            write_unlock(&g_manager.lock);
            return ret;
        }
    }

    write_unlock(&g_manager.lock);
    return 0;
}

int trouble_numa_list_get_all(u16 *buffer, size_t buf_size)
{
    struct numa_node *node;
    int count = 0;

    read_lock(&g_manager.lock);
    list_for_each_entry(node, &g_manager.head, list) {
        if (count < buf_size) {
            buffer[count++] = node->numa_id;
        } else {
            break;
        }
    }

    read_unlock(&g_manager.lock);
    return count;
}