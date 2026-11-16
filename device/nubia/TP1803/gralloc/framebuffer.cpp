/*
 * KMS "framebuffer" device for the TP1803 CM13 port.
 *
 * SurfaceFlinger (without an HWC) composes into ordinary ashmem gralloc
 * buffers, then calls post(). We copy the frame into one of two DRM dumb
 * buffers and page-flip to it. The SDE driver on the 4.14 kernel accepts
 * legacy SETCRTC/PAGE_FLIP on dumb buffers (verified with tools/kmstest).
 *
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2026 TP1803 legacy port
 * Licensed under the Apache License, Version 2.0
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

/* bionic's old drm.h names a struct member "virtual", which isn't valid C++ */
#define virtual virtual_
#include <drm/drm.h>
#include <drm/drm_mode.h>
#undef virtual

#include "gralloc_priv.h"
#include "gr.h"

#ifndef DRM_FORMAT_XBGR8888
#define DRM_FORMAT_XBGR8888 0x34324258 /* [31:0] x:B:G:R 8:8:8:8 little endian == R,G,B,X bytes */
#endif

struct fb_context_t {
    framebuffer_device_t device;
};

static_assert(sizeof(struct drm_mode_modeinfo) <= sizeof(((private_module_t*)0)->mode_blob),
              "mode_blob too small");

/*****************************************************************************/

static int fb_setSwapInterval(struct framebuffer_device_t* dev, int interval)
{
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;
    return 0;
}

/* Wait for the pending flip's completion event, if any. */
static void kms_wait_flip(private_module_t* m)
{
    while (m->flip_pending) {
        struct pollfd pfd = { m->drm_fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 100);
        if (r <= 0) {
            ALOGW("page flip event timeout (%d)", r);
            m->flip_pending = 0;
            break;
        }
        char buf[1024];
        ssize_t len = read(m->drm_fd, buf, sizeof(buf));
        ssize_t i = 0;
        while (len > 0 && i + (ssize_t)sizeof(struct drm_event) <= len) {
            struct drm_event* e = (struct drm_event*)&buf[i];
            if (e->type == DRM_EVENT_FLIP_COMPLETE)
                m->flip_pending = 0;
            i += e->length;
        }
    }
}

/* Import an ION-backed gralloc buffer into DRM and wrap it in a framebuffer.
 * Results are cached by dma-buf inode. Returns fb_id or 0 if not importable. */
static uint32_t kms_import_fb(private_module_t* m, private_handle_t const* hnd)
{
    if (!(hnd->flags & private_handle_t::PRIV_FLAGS_ION) || m->import_failed)
        return 0;
    struct stat st;
    if (fstat(hnd->fd, &st))
        return 0;
    const uint64_t ino = st.st_ino;
    m->import_clock++;

    struct kms_import* victim = NULL;
    for (int i = 0; i < KMS_IMPORT_CACHE; i++) {
        struct kms_import* e = &m->imports[i];
        if (e->fb_id && e->ino == ino) {
            e->last_use = m->import_clock;
            return e->fb_id;
        }
        if (e->fb_id == m->scanout_fb && e->fb_id)
            continue; /* never evict what's on screen */
        if (!victim || !e->fb_id || (victim->fb_id && e->last_use < victim->last_use))
            victim = e;
    }
    if (!victim)
        return 0;
    if (victim->fb_id) {
        ioctl(m->drm_fd, DRM_IOCTL_MODE_RMFB, &victim->fb_id);
        struct drm_gem_close gc = { victim->gem, 0 };
        ioctl(m->drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        memset(victim, 0, sizeof(*victim));
    }

    struct drm_prime_handle ph;
    memset(&ph, 0, sizeof(ph));
    ph.fd = hnd->fd;
    if (ioctl(m->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph)) {
        ALOGW("PRIME import failed (%s); using copy path", strerror(errno));
        m->import_failed = 1;
        return 0;
    }
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof(f));
    f.width = m->width;
    f.height = m->height;
    f.pixel_format = DRM_FORMAT_XBGR8888;
    f.handles[0] = ph.handle;
    f.pitches[0] = (uint32_t)hnd->stride * 4;
    if (ioctl(m->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f)) {
        ALOGW("ADDFB2 on imported buffer failed (%s); using copy path", strerror(errno));
        struct drm_gem_close gc = { ph.handle, 0 };
        ioctl(m->drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        m->import_failed = 1;
        return 0;
    }
    victim->ino = ino;
    victim->gem = ph.handle;
    victim->fb_id = f.fb_id;
    victim->last_use = m->import_clock;
    ALOGI("KMS: scanning out gralloc buffer ino=%llu as fb %u", (unsigned long long)ino, f.fb_id);
    return f.fb_id;
}

static int kms_show(private_module_t* m, uint32_t fb_id)
{
    if (!m->crtc_set) {
        struct drm_mode_crtc sc;
        memset(&sc, 0, sizeof(sc));
        sc.crtc_id = m->crtc_id;
        sc.fb_id = fb_id;
        sc.set_connectors_ptr = (uint64_t)(uintptr_t)&m->connector_id;
        sc.count_connectors = 1;
        sc.mode_valid = 1;
        memcpy(&sc.mode, m->mode_blob, sizeof(sc.mode));
        if (ioctl(m->drm_fd, DRM_IOCTL_MODE_SETCRTC, &sc)) {
            ALOGE("SETCRTC failed: %s", strerror(errno));
            return -errno;
        }
        m->crtc_set = 1;
    } else {
        struct drm_mode_crtc_page_flip pf;
        memset(&pf, 0, sizeof(pf));
        pf.crtc_id = m->crtc_id;
        pf.fb_id = fb_id;
        pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
        if (ioctl(m->drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
            m->flip_pending = 1;
        } else {
            ALOGW("PAGE_FLIP failed (%s), falling back to SETCRTC", strerror(errno));
            m->crtc_set = 0;
            return kms_show(m, fb_id);
        }
    }
    m->scanout_fb = fb_id;
    /* Block until the flip lands: the previous buffer goes back to
     * SurfaceFlinger as soon as we return, so it must be off screen. This also
     * paces SF to the panel refresh. */
    kms_wait_flip(m);
    return 0;
}

static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
    if (private_handle_t::validate(buffer) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(buffer);
    private_module_t* m = reinterpret_cast<private_module_t*>(dev->common.module);

    pthread_mutex_lock(&m->lock);

    int err;
    uint32_t fb_id = kms_import_fb(m, hnd);
    if (fb_id) {
        err = kms_show(m, fb_id);
    } else {
        /* Copy path: blit into the dumb buffer that isn't on screen. */
        int back = m->kms_front ^ 1;
        struct kms_buffer* kb = &m->kms[back];
        void* src = NULL;
        m->base.lock(&m->base, buffer, GRALLOC_USAGE_SW_READ_RARELY,
                     0, 0, m->width, m->height, &src);
        const size_t srcPitch = (size_t)hnd->stride * 4;
        const size_t rowBytes = (size_t)m->width * 4;
        if (srcPitch == kb->pitch) {
            memcpy(kb->map, src, (size_t)kb->pitch * m->height);
        } else {
            for (uint32_t y = 0; y < m->height; y++)
                memcpy((char*)kb->map + (size_t)y * kb->pitch,
                       (const char*)src + y * srcPitch, rowBytes);
        }
        m->base.unlock(&m->base, buffer);
        err = kms_show(m, kb->fb_id);
        if (!err)
            m->kms_front = back;
    }

    pthread_mutex_unlock(&m->lock);
    return err;
}

static int fb_compositionComplete(struct framebuffer_device_t* /*dev*/)
{
    return 0;
}

/*****************************************************************************/

static int kms_create_buffer(private_module_t* m, struct kms_buffer* b)
{
    struct drm_mode_create_dumb c;
    memset(&c, 0, sizeof(c));
    c.width = m->width;
    c.height = m->height;
    c.bpp = 32;
    if (ioctl(m->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) {
        ALOGE("CREATE_DUMB failed: %s", strerror(errno));
        return -errno;
    }
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;

    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof(f));
    f.width = m->width;
    f.height = m->height;
    f.pixel_format = DRM_FORMAT_XBGR8888;
    f.handles[0] = c.handle;
    f.pitches[0] = c.pitch;
    if (ioctl(m->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f)) {
        ALOGE("ADDFB2 failed: %s", strerror(errno));
        return -errno;
    }
    b->fb_id = f.fb_id;

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof(md));
    md.handle = c.handle;
    if (ioctl(m->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        ALOGE("MAP_DUMB failed: %s", strerror(errno));
        return -errno;
    }
    b->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, m->drm_fd, md.offset);
    if (b->map == MAP_FAILED) {
        ALOGE("mmap dumb failed: %s", strerror(errno));
        return -errno;
    }
    memset(b->map, 0, c.size);
    return 0;
}

int mapFrameBufferLocked(struct private_module_t* module)
{
    if (module->drm_fd > 0)
        return 0;

    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("can't open /dev/dri/card0: %s", strerror(errno));
        return -errno;
    }
    if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0))
        ALOGW("SET_MASTER failed: %s (continuing)", strerror(errno));

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        ALOGE("GETRESOURCES failed: %s", strerror(errno));
        close(fd);
        return -errno;
    }
    uint32_t conn_ids[16], crtc_ids[16], enc_ids[16];
    if (res.count_connectors > 16 || res.count_crtcs > 16 || res.count_encoders > 16) {
        close(fd);
        return -EINVAL;
    }
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)enc_ids;
    res.count_fbs = 0;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    struct drm_mode_modeinfo modes[32], mode;
    uint32_t encs[16];
    uint32_t conn = 0, enc = 0, mm_w = 0, mm_h = 0;
    for (uint32_t i = 0; i < res.count_connectors && !conn; i++) {
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof(c));
        c.connector_id = conn_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c))
            continue;
        /* Prefer the built-in DSI panel. */
        if (c.connector_type != DRM_MODE_CONNECTOR_DSI || c.connection != 1 || !c.count_modes)
            continue;
        if (c.count_modes > 32) c.count_modes = 32;
        if (c.count_encoders > 16) c.count_encoders = 16;
        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        c.encoders_ptr = (uint64_t)(uintptr_t)encs;
        c.count_props = 0;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c))
            continue;
        conn = conn_ids[i];
        mode = modes[0];
        for (uint32_t k = 0; k < c.count_modes; k++)
            if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { mode = modes[k]; break; }
        enc = c.encoder_id ? c.encoder_id : encs[0];
        mm_w = c.mm_width;
        mm_h = c.mm_height;
    }
    if (!conn) {
        ALOGE("no connected DSI connector");
        close(fd);
        return -ENODEV;
    }

    struct drm_mode_get_encoder e;
    memset(&e, 0, sizeof(e));
    e.encoder_id = enc;
    ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e);
    uint32_t crtc = e.crtc_id;
    for (uint32_t i = 0; !crtc && i < res.count_crtcs; i++)
        if (e.possible_crtcs & (1u << i))
            crtc = crtc_ids[i];

    module->drm_fd = fd;
    module->connector_id = conn;
    module->crtc_id = crtc;
    module->width = mode.hdisplay;
    module->height = mode.vdisplay;
    module->refresh = mode.vrefresh ? mode.vrefresh : 60;
    memcpy(module->mode_blob, &mode, sizeof(mode));

    for (int i = 0; i < 2; i++) {
        int err = kms_create_buffer(module, &module->kms[i]);
        if (err) {
            close(fd);
            module->drm_fd = 0;
            return err;
        }
    }
    module->kms_front = 1;   /* first post goes to buffer 0 */

    /* DPI: use connector physical size when present, else the 5.7" JDI panel */
    if (mm_w && mm_h) {
        module->xdpi = module->width * 25.4f / mm_w;
        module->ydpi = module->height * 25.4f / mm_h;
    } else {
        module->xdpi = module->ydpi = 424.0f;
    }
    module->fps = module->refresh;

    ALOGI("KMS: conn=%u crtc=%u %ux%u@%u pitch=%u dpi=%.1fx%.1f",
          conn, crtc, module->width, module->height, module->refresh,
          module->kms[0].pitch, module->xdpi, module->ydpi);
    return 0;
}

static int mapFrameBuffer(struct private_module_t* module)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module);
    pthread_mutex_unlock(&module->lock);
    return err;
}

/*****************************************************************************/

static int fb_close(struct hw_device_t* dev)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    free(ctx);
    return 0;
}

int fb_device_open(hw_module_t const* module, const char* name,
        hw_device_t** device)
{
    if (strcmp(name, GRALLOC_HARDWARE_FB0))
        return -EINVAL;

    fb_context_t* dev = (fb_context_t*)malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));

    dev->device.common.tag = HARDWARE_DEVICE_TAG;
    dev->device.common.version = 0;
    dev->device.common.module = const_cast<hw_module_t*>(module);
    dev->device.common.close = fb_close;
    dev->device.setSwapInterval = fb_setSwapInterval;
    dev->device.post = fb_post;
    dev->device.setUpdateRect = 0;
    dev->device.compositionComplete = fb_compositionComplete;

    private_module_t* m = (private_module_t*)module;
    int status = mapFrameBuffer(m);
    if (status < 0) {
        free(dev);
        return status;
    }

    const_cast<uint32_t&>(dev->device.flags) = 0;
    const_cast<uint32_t&>(dev->device.width) = m->width;
    const_cast<uint32_t&>(dev->device.height) = m->height;
    const_cast<int&>(dev->device.stride) = m->width;
    const_cast<int&>(dev->device.format) = HAL_PIXEL_FORMAT_RGBX_8888;
    const_cast<float&>(dev->device.xdpi) = m->xdpi;
    const_cast<float&>(dev->device.ydpi) = m->ydpi;
    const_cast<float&>(dev->device.fps) = m->fps;
    const_cast<int&>(dev->device.minSwapInterval) = 1;
    const_cast<int&>(dev->device.maxSwapInterval) = 1;
    /* One "framebuffer" buffer: SF renders into normal ashmem buffers that post() copies. */
    const_cast<int&>(dev->device.numFramebuffers) = 1;

    *device = &dev->device.common;
    return 0;
}
