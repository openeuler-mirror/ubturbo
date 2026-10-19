/*
* Description: SMAP3.0 Tiering Memory Solution: tracking_dev
*/
#ifndef _LINUX_DMA_MAP_OPS_H
#define _LINUX_DMA_MAP_OPS_H
#include <linux/types.h>
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_NONE
};
struct device;
struct page;
struct dma_map_ops {
    dma_addr_t (*map_page)(struct device *dev, struct page *page, unsigned long offset, size_t size,
                           enum dma_data_direction dir, unsigned long attrs);
    void (*unmap_page)(struct device *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir,
                       unsigned long attrs);
};
#endif