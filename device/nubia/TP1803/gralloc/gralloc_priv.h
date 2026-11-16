/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2026 TP1803 legacy port
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

#ifndef GRALLOC_PRIV_H_
#define GRALLOC_PRIV_H_

#include <stdint.h>
#include <limits.h>
#include <sys/cdefs.h>
#include <hardware/gralloc.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>

#include <cutils/native_handle.h>

/*****************************************************************************/

struct private_module_t;
struct private_handle_t;

/* Qualcomm-private YUV layouts the camera HAL (CamX) writes; values match
 * QTI gralloc so CamX and our consumers agree on them. */
#define HAL_PIXEL_FORMAT_YCbCr_420_SP_VENUS 0x7FA30C04  /* NV12, stride 128, rows 32 */
#define HAL_PIXEL_FORMAT_NV21_ZSL           0x113       /* NV21, stride 64, rows 64 */

/* One KMS scanout buffer (a DRM dumb buffer). */
struct kms_buffer {
    uint32_t handle;
    uint32_t pitch;
    uint32_t fb_id;
    uint64_t size;
    void*    map;
};

/* A gralloc (ION dma-buf) buffer imported into DRM for direct scanout. */
struct kms_import {
    uint64_t ino;      /* dma-buf inode: unique per buffer */
    uint32_t gem;
    uint32_t fb_id;
    uint32_t last_use;
};
#define KMS_IMPORT_CACHE 8

struct private_module_t {
    gralloc_module_t base;

    /* KMS state (owned by the process that opened the fb device, i.e. SurfaceFlinger) */
    int      drm_fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh;
    uint32_t mode_blob[17];   /* struct drm_mode_modeinfo, kept opaque here */
    struct kms_buffer kms[2];
    int      kms_front;       /* index of the buffer currently scanned out */
    int      flip_pending;
    int      crtc_set;
    struct kms_import imports[KMS_IMPORT_CACHE];
    uint32_t import_clock;
    uint32_t scanout_fb;      /* fb_id currently on screen (never evicted) */
    int      import_failed;   /* PRIME/ADDFB2 rejected: stay on the copy path */

    uint32_t flags;
    pthread_mutex_t lock;
    float xdpi;
    float ydpi;
    float fps;
};

/*****************************************************************************/

#ifdef __cplusplus
struct private_handle_t : public native_handle {
#else
struct private_handle_t {
    struct native_handle nativeHandle;
#endif

    enum {
        PRIV_FLAGS_FRAMEBUFFER = 0x00000001,
        PRIV_FLAGS_ION         = 0x00000002,  /* fd is an ION dma-buf */
        PRIV_FLAGS_CACHED      = 0x00000004,  /* CPU-cached: sync around locks */
    };

    // file-descriptors
    int     fd;
    // ints
    int     magic;
    int     flags;
    int     size;
    int     offset;
    int     format;
    int     width;
    int     height;
    int     stride;   /* in pixels (luma pixels for YUV) */
    int     vstride;  /* YUV: luma plane height in rows (chroma follows) */

    // FIXME: the attributes below should be out-of-line
    uint64_t base __attribute__((aligned(8)));
    int     pid;

#ifdef __cplusplus
    static inline int sNumInts() {
        return (((sizeof(private_handle_t) - sizeof(native_handle_t))/sizeof(int)) - sNumFds);
    }
    static const int sNumFds = 1;
    static const int sMagic = 0x3141593;

    private_handle_t(int fd, int size, int flags) :
        fd(fd), magic(sMagic), flags(flags), size(size), offset(0),
        format(0), width(0), height(0), stride(0), vstride(0),
        base(0), pid(getpid())
    {
        version = sizeof(native_handle);
        numInts = sNumInts();
        numFds = sNumFds;
    }
    ~private_handle_t() {
        magic = 0;
    }

    static int validate(const native_handle* h) {
        const private_handle_t* hnd = (const private_handle_t*)h;
        if (!h || h->version != sizeof(native_handle) ||
                h->numInts != sNumInts() || h->numFds != sNumFds ||
                hnd->magic != sMagic)
        {
            ALOGE("invalid gralloc handle (at %p): version %d numFds %d numInts %d (want %d) magic 0x%x",
                  h, h ? h->version : -1, h ? h->numFds : -1, h ? h->numInts : -1,
                  sNumInts(), h ? hnd->magic : 0);
            return -EINVAL;
        }
        return 0;
    }
#endif
};

#endif /* GRALLOC_PRIV_H_ */
