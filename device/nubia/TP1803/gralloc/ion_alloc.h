/*
 * ION allocation for the 4.14 kernel (staging ION ABI >= 4.12: ALLOC returns a
 * dma-buf fd directly; heaps are discovered with HEAP_QUERY). Defined locally
 * because M's bionic kernel headers carry the pre-4.12 ABI.
 */
#pragma once
#include <stdint.h>
#include <sys/ioctl.h>

struct ion414_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
struct ion414_heap_data {
    char name[32];
    uint32_t type;
    uint32_t heap_id;
    uint32_t reserved0, reserved1, reserved2;
};
struct ion414_heap_query {
    uint32_t cnt;
    uint32_t reserved0;
    uint64_t heaps;
    uint32_t reserved1, reserved2;
};
#define ION414_IOC_ALLOC      _IOWR('I', 0, struct ion414_allocation_data)
#define ION414_IOC_HEAP_QUERY _IOWR('I', 8, struct ion414_heap_query)
#define ION414_HEAP_TYPE_SYSTEM 0
#define ION414_FLAG_CACHED    1

struct dma_buf_sync_414 { uint64_t flags; };
#define DMA_BUF_SYNC_READ_  (1 << 0)
#define DMA_BUF_SYNC_WRITE_ (2 << 0)
#define DMA_BUF_SYNC_RW_    (DMA_BUF_SYNC_READ_ | DMA_BUF_SYNC_WRITE_)
#define DMA_BUF_SYNC_START_ (0 << 2)
#define DMA_BUF_SYNC_END_   (1 << 2)
#define DMA_BUF_IOCTL_SYNC_ _IOW('b', 0, struct dma_buf_sync_414)

/* Returns a dma-buf fd, or -errno. */
int ion_alloc_dmabuf(size_t size, bool cached);
