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
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "gr.h"
#include "ion_alloc.h"

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
};

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle);

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

extern int gralloc_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

extern int gralloc_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
        .open = gralloc_device_open
};

extern int gralloc_lock_ycbcr(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        struct android_ycbcr *ycbcr);

struct private_module_t HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = GRALLOC_MODULE_API_VERSION_0_2,
            .hal_api_version = 0,
            .id = GRALLOC_HARDWARE_MODULE_ID,
            .name = "TP1803 KMS/ION gralloc",
            .author = "TP1803 legacy port",
            .methods = &gralloc_module_methods
        },
        .registerBuffer = gralloc_register_buffer,
        .unregisterBuffer = gralloc_unregister_buffer,
        .lock = gralloc_lock,
        .unlock = gralloc_unlock,
        .perform = NULL,
        .lock_ycbcr = gralloc_lock_ycbcr,
    },
    /* Remaining fields are zero: drm_fd == 0 means "not opened yet" (fd 0 is
     * never the DRM node), and bionic's PTHREAD_MUTEX_INITIALIZER is all zeros. */
};

/*****************************************************************************/

/* ION: one /dev/ion fd and the system heap's id, discovered once per process. */
static pthread_mutex_t sIonLock = PTHREAD_MUTEX_INITIALIZER;
static int sIonFd = -1;
static uint32_t sIonHeapMask = 0;

static int ion_init_locked()
{
    if (sIonFd >= 0)
        return 0;
    int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct ion414_heap_data heaps[32];
    struct ion414_heap_query q;
    memset(&q, 0, sizeof(q));
    q.cnt = 32;
    q.heaps = (uint64_t)(uintptr_t)heaps;
    uint32_t mask = 0;
    if (ioctl(fd, ION414_IOC_HEAP_QUERY, &q) == 0) {
        for (uint32_t i = 0; i < q.cnt; i++)
            if (heaps[i].type == ION414_HEAP_TYPE_SYSTEM) {
                mask = 1u << heaps[i].heap_id;
                ALOGI("ION system heap '%s' id %u", heaps[i].name, heaps[i].heap_id);
                break;
            }
    }
    if (!mask) {
        ALOGW("ION heap query failed (%s); assuming msm system heap id 25", strerror(errno));
        mask = 1u << 25;
    }
    sIonFd = fd;
    sIonHeapMask = mask;
    return 0;
}

int ion_alloc_dmabuf(size_t size, bool cached)
{
    pthread_mutex_lock(&sIonLock);
    int err = ion_init_locked();
    pthread_mutex_unlock(&sIonLock);
    if (err)
        return err;
    struct ion414_allocation_data a;
    memset(&a, 0, sizeof(a));
    a.len = size;
    a.heap_id_mask = sIonHeapMask;
    a.flags = cached ? ION414_FLAG_CACHED : 0;
    if (ioctl(sIonFd, ION414_IOC_ALLOC, &a))
        return -errno;
    return (int)a.fd;
}

static int gralloc_alloc_buffer(alloc_device_t* /*dev*/,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    size = roundUpToPageSize(size);

    /* GPU/display buffers live in ION so KGSL (Mesa) and DRM can import the
     * dma-buf. CPU-read-heavy buffers are cached and synced around locks;
     * everything else is uncached (write-combined), which is what the GPU and
     * scanout want. */
    const bool cached = (usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN;
    int flags = 0;
    int fd = ion_alloc_dmabuf(size, cached);
    if (fd >= 0) {
        flags = private_handle_t::PRIV_FLAGS_ION |
                (cached ? private_handle_t::PRIV_FLAGS_CACHED : 0);
    } else {
        ALOGW("ION alloc of %zu failed (%s), falling back to ashmem", size, strerror(-fd));
        fd = ashmem_create_region("gralloc-buffer", size);
        if (fd < 0) {
            ALOGE("couldn't create ashmem (%s)", strerror(errno));
            return -errno;
        }
    }

    private_handle_t* hnd = new private_handle_t(fd, size, flags);
    /* Map lazily on lock/register. Allocations happen in SF, and mapping every
     * buffer there would waste address space. */
    *pHandle = hnd;
    return 0;
}

/*****************************************************************************/

static inline size_t align(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

static int gralloc_alloc(alloc_device_t* dev,
        int w, int h, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    if (!pHandle || !pStride || w < 0 || h < 0)
        return -EINVAL;

    size_t size = 0, stride = 0;

    size_t vstride = h;
    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        /* Camera output uses the layouts CamX writes (as QTI gralloc maps
         * them): ZSL -> NV21_ZSL, preview/video -> NV12 Venus. Other video
         * producers get flexible YUV, everything else RGBX. */
        if ((usage & GRALLOC_USAGE_HW_CAMERA_MASK) == GRALLOC_USAGE_HW_CAMERA_ZSL)
            format = HAL_PIXEL_FORMAT_NV21_ZSL;
        else if (usage & GRALLOC_USAGE_HW_CAMERA_WRITE)
            format = HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS;
        else if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER)
            format = HAL_PIXEL_FORMAT_YCbCr_420_888;
        else
            format = HAL_PIXEL_FORMAT_RGBX_8888;
    } else if (format == HAL_PIXEL_FORMAT_YCbCr_420_888 && (usage & GRALLOC_USAGE_HW_CAMERA_WRITE)) {
        format = HAL_PIXEL_FORMAT_NV21_ZSL;
    }

    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            /* freedreno imports need pitch aligned to gmem_align_w (<= 32 px).
             * Adreno GMEM resolves write whole tiles, so pad the height to the
             * tile alignment too (plus a spare page), as Qualcomm's gralloc
             * does; otherwise the CCU writes past the end of the buffer. */
            stride = align(w, 32);
            size = stride * 4 * align(h, 32) + PAGE_SIZE;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            stride = align(w, 32);
            size = stride * 3 * align(h, 32) + PAGE_SIZE;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RAW16:
        case HAL_PIXEL_FORMAT_Y16:
            stride = align(w, 32);
            size = stride * 2 * align(h, 32) + PAGE_SIZE;
            break;
        case HAL_PIXEL_FORMAT_Y8:
            stride = align(w, 16);
            size = stride * h;
            break;
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCbCr_420_888: {
            /* Laid out as YV12: Y plane, then Cr, then Cb; chroma stride = align(stride/2,16) */
            stride = align(w, 16);
            size_t cstride = align(stride / 2, 16);
            size = stride * h + cstride * ((h + 1) / 2) * 2;
            break;
        }
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS: {
            stride = align(w, 128);
            vstride = align(h, 32);
            size_t uv_rows = align((h + 1) / 2, 16);
            size = align(stride * vstride + stride * uv_rows + 4096 + 8 * stride, 4096);
            break;
        }
        case HAL_PIXEL_FORMAT_NV21_ZSL:
            stride = align(w, 64);
            vstride = align(h, 64);
            size = align(stride * vstride * 3 / 2, 4096);
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            stride = align(w, 16);
            size = stride * h * 2;
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            /* w is the byte size, h == 1 */
            stride = w;
            size = (size_t)w * (h ? h : 1);
            break;
        default:
            ALOGE("unsupported format %d (%dx%d usage=0x%x)", format, w, h, usage);
            return -EINVAL;
    }
    if (size == 0)
        size = PAGE_SIZE;

    int err = gralloc_alloc_buffer(dev, size, usage, pHandle);
    if (err < 0)
        return err;

    private_handle_t* hnd = (private_handle_t*)*pHandle;
    hnd->format = format;
    hnd->width = w;
    hnd->height = h;
    hnd->stride = stride;
    hnd->vstride = vstride;

    *pStride = stride;
    return 0;
}

static int gralloc_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(dev->common.module);
    terminateBuffer(module, const_cast<private_handle_t*>(hnd));

    close(hnd->fd);
    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc   = gralloc_alloc;
        dev->device.free    = gralloc_free;

        *device = &dev->device.common;
        status = 0;
    } else {
        status = fb_device_open(module, name, device);
    }
    return status;
}
