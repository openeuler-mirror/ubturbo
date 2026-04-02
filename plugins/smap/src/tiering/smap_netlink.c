// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/netlink.h>
#include <net/sock.h>

#include "trouble_numa_meta.h"
#include "smap_netlink.h"

#define NETLINK_USER_PROTOCOL 31

enum nl_message_type {
    NL_MSG_REGISTER = 1,
    NL_MSG_UNREGISTER,
    NL_MSG_USER_TO_KERNEL,
    NL_MSG_KERNEL_TO_USER,
    NL_MSG_ACK
};

struct nl_msg_header {
    uint32_t type;
    uint32_t data_len;
};

static struct sock *nl_sock = NULL;
static uint32_t registered_pid = 0;

static int send_nl_message(uint32_t dst_pid, uint32_t type, 
                           const void *data, size_t data_len) {
    struct sk_buff *skb;
    struct nl_msg_header *header;
    struct nlmsghdr *nlh;
    int total_size;
    int ret;
    
    if (!dst_pid) {
        pr_err("No registered PID\n");
        return -EINVAL;
    }
    
    total_size = NLMSG_SPACE(sizeof(struct nl_msg_header) + data_len);
    
    skb = nlmsg_new(total_size, GFP_ATOMIC);
    if (!skb) {
        pr_err("Failed to allocate skb\n");
        return -ENOMEM;
    }
    
    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, total_size - sizeof(*nlh), 0);
    if (!nlh) {
        pr_err("nlmsg_put failed\n");
        kfree_skb(skb);
        return -EMSGSIZE;
    }

    header = nlmsg_data(nlh);
    header->type = type;
    header->data_len = data_len;
    
    if (data && data_len > 0) {
        memcpy((void *)(header + 1), data, data_len);
    }

    ret = nlmsg_unicast(nl_sock, skb, dst_pid);
    if (ret < 0) {
        pr_err("nlmsg_unicast failed: %d\n", ret);
    }
    
    return ret;
}

int smap_send_info_to_user(const void *data, size_t data_len)
{
    int ret;

    if (!registered_pid) {
        pr_warn("No registered PID to send message\n");
        return -EINVAL;
    }

    ret = send_nl_message(registered_pid, NL_MSG_KERNEL_TO_USER, data, data_len);
    if (ret < 0) {
        pr_err("Failed to send message to user: %d\n", ret);
    }
    
    return ret;
}

static void nl_recv_callback(struct sk_buff *skb) {
    struct nlmsghdr *nlh;
    struct nl_msg_header *header;
    void *payload;
    uint32_t sender_pid;
    
    nlh = nlmsg_hdr(skb);
    if (!nlh || !NLMSG_OK(nlh, skb->len)) {
        pr_err("Invalid netlink message\n");
        return;
    }
    
    header = nlmsg_data(nlh);
    payload = (void *)(header + 1);
    sender_pid = nlh->nlmsg_pid;

    switch (header->type) {
        case NL_MSG_REGISTER:
            registered_pid = sender_pid;
            pr_info("Kernel: User PID %d registered\n", sender_pid);
            break;
        case NL_MSG_UNREGISTER:
            if (registered_pid == sender_pid) {
                registered_pid = 0;
            }
            pr_info("Kernel: User PID %d unregistered\n", sender_pid);
            break;
        case NL_MSG_USER_TO_KERNEL:
            if (header->data_len > 0) {
                deal_trouble_numa_info(payload);
            }
            break;
        default:
            pr_warn("Unknown message type: %d\n", header->type);
    }
}

int smap_netlink_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = nl_recv_callback,
        .flags = NL_CFG_F_NONROOT_RECV | NL_CFG_F_NONROOT_SEND,
    };

    nl_sock = netlink_kernel_create(&init_net, NETLINK_USER_PROTOCOL, &cfg);
    if (!nl_sock) {
        pr_err("Failed to create netlink socket\n");
        return -ENOMEM;
    }

    return 0;
}

void smap_netlink_exit(void)
{
    if (nl_sock) {
        netlink_kernel_release(nl_sock);
        nl_sock = NULL;
    }
}