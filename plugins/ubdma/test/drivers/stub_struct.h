/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UBDMA 驱动模块打桩文件
 */
#ifndef __STUB_STRUCT_H
#define __STUB_STRUCT_H
struct mem_nf_work_info {
    struct work_struct work;
    u64 start_pa;
    u64 end_pa;
};

struct migrate_msg {
    uint64_t src_start;
    uint64_t src_end;
    uint64_t dst_start;
    uint64_t dst_end;
    uint32_t src_nid;
    uint32_t dst_nid;
    uint64_t pfn;
};

struct ub_virt_dma_desc {
    struct dma_async_tx_descriptor tx;
    struct dmaengine_result tx_result;
    struct list_head node;
};

struct ub_dma_desc {
    struct ub_virt_dma_desc vd;
    uint64_t src_addr;
    uint64_t dest_addr;
    size_t len;
};
struct ub_virt_dma_chan {
    struct dma_chan chan;
    struct tasklet_struct task;
    void (*desc_free)(struct ub_virt_dma_desc *);

    spinlock_t lock;

    struct list_head desc_allocated;
    struct list_head desc_submitted;
    struct list_head desc_issued;
    struct list_head desc_completed;
    struct list_head desc_terminated;

    struct ub_virt_dma_desc *cyclic;
};

struct ub_dma_vchan {
    struct ub_virt_dma_chan vc;
    enum dma_status status;
    struct list_head node;
};

struct swait_queue_head {
    raw_spinlock_t lock;
    struct list_head task_list;
};

struct completion {
    unsigned int done;
    struct swait_queue_head wait;
};

struct dma_channel_work {
    struct dma_chan *chan;
    enum dma_status status;
    struct completion done;
};

struct ub_dma_dev {
    struct dma_device slave;
    struct ub_dma_vchan *vchans;
    uint32_t vchan_num;
    struct device_driver driver;
};

#endif
