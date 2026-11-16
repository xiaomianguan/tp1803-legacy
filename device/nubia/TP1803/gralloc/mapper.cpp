/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>
#include <system/graphics.h>

#include "gralloc_priv.h"
#include "ion_alloc.h"

#include <sys/ioctl.h>

static void dmabuf_sync(const private_handle_t* hnd, uint64_t flags)
{
    if ((hnd->flags & private_handle_t::PRIV_FLAGS_CACHED) == 0)
        return;
    struct dma_buf_sync_414 s = { flags | DMA_BUF_SYNC_RW_ };
    ioctl(hnd->fd, DMA_BUF_IOCTL_SYNC_, &s);
}


/*****************************************************************************/

static int gralloc_map(gralloc_module_t const* /*module*/,
        buffer_handle_t handle,
        void** vaddr)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    if (!hnd->base) {
        size_t size = hnd->size;
        void* mappedAddress = mmap(0, size,
                PROT_READ|PROT_WRITE, MAP_SHARED, hnd->fd, 0);
        if (mappedAddress == MAP_FAILED) {
            ALOGE("Could not mmap %s", strerror(errno));
            return -errno;
        }
        hnd->base = uintptr_t(mappedAddress) + hnd->offset;
        //ALOGD("gralloc_map() succeeded fd=%d, off=%d, size=%d, vaddr=%p",
        //        hnd->fd, hnd->offset, hnd->size, mappedAddress);
    }
    *vaddr = (void*)hnd->base;
    return 0;
}

static int gralloc_unmap(gralloc_module_t const* /*module*/,
        buffer_handle_t handle)
{
    private_handle_t* hnd = (private_handle_t*)handle;
    if (hnd->base) {
        void* base = (void*)hnd->base;
        size_t size = hnd->size;
        //ALOGD("unmapping from %p, size=%d", base, size);
        if (munmap(base, size) < 0) {
            ALOGE("Could not unmap %s", strerror(errno));
        }
    }
    hnd->base = 0;
    return 0;
}

/*****************************************************************************/

int gralloc_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    // *** WARNING WARNING WARNING ***
    //
    // If a buffer handle is passed from the process that allocated it to a
    // different process, and then back to the allocator process, we will
    // create a second mapping of the buffer. If the process reads and writes
    // through both mappings, normal memory ordering guarantees may be
    // violated, depending on the processor cache implementation*.
    //
    // If you are deriving a new gralloc implementation from this code, don't
    // do this. A "real" gralloc should provide a single reference-counted
    // mapping for each buffer in a process.
    //
    // In the current system, there is one case that needs a buffer to be
    // registered in the same process that allocated it. The SurfaceFlinger
    // process acts as the IGraphicBufferAlloc Binder provider, so all gralloc
    // allocations happen in its process. After returning the buffer handle to
    // the IGraphicBufferAlloc client, SurfaceFlinger free's its handle to the
    // buffer (unmapping it from the SurfaceFlinger process). If
    // SurfaceFlinger later acts as the producer end of the buffer queue the
    // buffer belongs to, it will get a new handle to the buffer in response
    // to IGraphicBufferProducer::requestBuffer(). Like any buffer handle
    // received through Binder, the SurfaceFlinger process will register it.
    // Since it already freed its original handle, it will only end up with
    // one mapping to the buffer and there will be no problem.
    //
    // Currently SurfaceFlinger only acts as a buffer producer for a remote
    // consumer when taking screenshots and when using virtual displays.
    //
    // Eventually, each application should be allowed to make its own gralloc
    // allocations, solving the problem. Also, this ashmem-based gralloc
    // should go away, replaced with a real ion-based gralloc.
    //
    // * Specifically, associative virtually-indexed caches are likely to have
    //   problems. Most modern L1 caches fit that description.

    private_handle_t* hnd = (private_handle_t*)handle;
    ALOGD_IF(hnd->pid == getpid(),
            "Registering a buffer in the process that created it. "
            "This may cause memory ordering problems.");

    /* base travels with the handle through Binder: it's the sender's mapping,
     * meaningless here. Map our own (unmapping the sender's address later
     * would tear down unrelated memory in this process). */
    hnd->base = 0;

    void *vaddr;
    return gralloc_map(module, handle, &vaddr);
}

int gralloc_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    if (hnd->base)
        gralloc_unmap(module, handle);

    return 0;
}

int mapBuffer(gralloc_module_t const* module,
        private_handle_t* hnd)
{
    void* vaddr;
    return gralloc_map(module, hnd, &vaddr);
}

int terminateBuffer(gralloc_module_t const* module,
        private_handle_t* hnd)
{
    if (hnd->base) {
        // this buffer was mapped, unmap it now
        gralloc_unmap(module, hnd);
    }

    return 0;
}

static pthread_mutex_t sMapLock = PTHREAD_MUTEX_INITIALIZER;

int gralloc_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int /*usage*/,
        int /*l*/, int /*t*/, int /*w*/, int /*h*/,
        void** vaddr)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    if (!hnd->base) {
        /* Buffers aren't mapped at allocation time; map on first CPU access. */
        pthread_mutex_lock(&sMapLock);
        int err = hnd->base ? 0 : gralloc_map(module, handle, vaddr);
        pthread_mutex_unlock(&sMapLock);
        if (err)
            return err;
    }
    dmabuf_sync(hnd, DMA_BUF_SYNC_START_);
    *vaddr = (void*)hnd->base;
    return 0;
}

int gralloc_lock_ycbcr(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        struct android_ycbcr *ycbcr)
{
    if (private_handle_t::validate(handle) < 0 || !ycbcr)
        return -EINVAL;

    private_handle_t* hnd = (private_handle_t*)handle;
    void* base = NULL;
    /* Without CPU usage, callers (Mesa's buffer import) want plane offsets:
     * the pointers are returned relative to NULL and nothing is mapped. */
    if (usage & (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK)) {
        int err = gralloc_lock(module, handle, usage, l, t, w, h, &base);
        if (err)
            return err;
    }

    const size_t ystride = hnd->stride;
    const size_t h_ = hnd->height;
    memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));
    switch (hnd->format) {
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCbCr_420_888: {
            const size_t cstride = (ystride / 2 + 15) & ~15;
            char* y = (char*)base;
            char* cr = y + ystride * h_;
            char* cb = cr + cstride * ((h_ + 1) / 2);
            ycbcr->y = y;
            ycbcr->cb = cb;
            ycbcr->cr = cr;
            ycbcr->ystride = ystride;
            ycbcr->cstride = cstride;
            ycbcr->chroma_step = 1;
            return 0;
        }
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS: { /* NV12 */
            char* y = (char*)base;
            ycbcr->y = y;
            ycbcr->cb = y + ystride * hnd->vstride;
            ycbcr->cr = (char*)ycbcr->cb + 1;
            ycbcr->ystride = ystride;
            ycbcr->cstride = ystride;
            ycbcr->chroma_step = 2;
            return 0;
        }
        case HAL_PIXEL_FORMAT_NV21_ZSL: { /* NV21 */
            char* y = (char*)base;
            ycbcr->y = y;
            ycbcr->cr = y + ystride * hnd->vstride;
            ycbcr->cb = (char*)ycbcr->cr + 1;
            ycbcr->ystride = ystride;
            ycbcr->cstride = ystride;
            ycbcr->chroma_step = 2;
            return 0;
        }
        case HAL_PIXEL_FORMAT_YCrCb_420_SP: { /* NV21 */
            char* y = (char*)base;
            ycbcr->y = y;
            ycbcr->cr = y + ystride * h_;
            ycbcr->cb = (char*)ycbcr->cr + 1;
            ycbcr->ystride = ystride;
            ycbcr->cstride = ystride;
            ycbcr->chroma_step = 2;
            return 0;
        }
        default:
            ALOGE("lock_ycbcr: unsupported format %d", hnd->format);
            return -EINVAL;
    }
}

int gralloc_unlock(gralloc_module_t const* /*module*/,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;
    dmabuf_sync((const private_handle_t*)handle, DMA_BUF_SYNC_END_);
    return 0;
}
