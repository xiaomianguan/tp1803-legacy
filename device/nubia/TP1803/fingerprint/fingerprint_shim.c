/*
 * Fingerprint HAL 2.0 (Android 6 fingerprintd) on top of the stock Sunwave
 * HAL 2.1 blob (libsunwave_fp.so, runs the sw_fp QSEE trustlet).
 *
 * The two APIs share the device struct layout; only enumerate() differs
 * (2.1 reports templates through FINGERPRINT_TEMPLATE_ENUMERATING messages).
 */
#define LOG_TAG "fingerprint.tp1803"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/fingerprint.h>

#define BLOB "/system/vendor/lib64/libsunwave_fp.so"
#define MSG_ENUMERATING 6           /* FINGERPRINT_TEMPLATE_ENUMERATING (2.1) */

/* 2.1 device: same as 2.0 except enumerate(). */
struct fp21_device {
    hw_device_t common;
    fingerprint_notify_t notify;
    int (*set_notify)(struct fp21_device *dev, fingerprint_notify_t notify);
    uint64_t (*pre_enroll)(struct fp21_device *dev);
    int (*enroll)(struct fp21_device *dev, const hw_auth_token_t *hat, uint32_t gid,
                  uint32_t timeout_sec);
    int (*post_enroll)(struct fp21_device *dev);
    uint64_t (*get_authenticator_id)(struct fp21_device *dev);
    int (*cancel)(struct fp21_device *dev);
    int (*enumerate)(struct fp21_device *dev);
    int (*remove)(struct fp21_device *dev, uint32_t gid, uint32_t fid);
    int (*set_active_group)(struct fp21_device *dev, uint32_t gid, const char *store_path);
    int (*authenticate)(struct fp21_device *dev, uint64_t operation_id, uint32_t gid);
    void *reserved[4];
};

struct shim {
    fingerprint_device_t dev;           /* must be first */
    struct fp21_device *real;
    void *lib;
};

static fingerprint_notify_t client_notify;

/* enumerate(): collect ENUMERATING messages until the last one. */
static pthread_mutex_t enum_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t enum_cond = PTHREAD_COND_INITIALIZER;
static int enum_active, enum_done;
static uint32_t enum_count;
static fingerprint_finger_id_t enum_ids[32];

static void shim_notify(const fingerprint_msg_t *msg)
{
    if (msg->type == MSG_ENUMERATING) {
        /* layout: finger id, then u32 remaining templates */
        const uint32_t *remaining = (const uint32_t *)(&msg->data.removed.finger + 1);
        pthread_mutex_lock(&enum_lock);
        if (enum_active) {
            if (msg->data.removed.finger.fid && enum_count < 32)
                enum_ids[enum_count++] = msg->data.removed.finger;
            if (*remaining == 0) {
                enum_done = 1;
                pthread_cond_broadcast(&enum_cond);
            }
        }
        pthread_mutex_unlock(&enum_lock);
        return;
    }
    if (msg->type == FINGERPRINT_TEMPLATE_REMOVED && msg->data.removed.finger.fid == 0)
        return;     /* 2.1 "remove all" end marker; 2.0 clients don't know it */
    if (client_notify)
        client_notify(msg);
}

#define REAL(d) (((struct shim *)(d))->real)

static int shim_set_notify(struct fingerprint_device *d, fingerprint_notify_t notify)
{
    client_notify = notify;
    d->notify = notify;
    return REAL(d)->set_notify(REAL(d), shim_notify);
}

static uint64_t shim_pre_enroll(struct fingerprint_device *d)
{
    return REAL(d)->pre_enroll(REAL(d));
}

static int shim_enroll(struct fingerprint_device *d, const hw_auth_token_t *hat, uint32_t gid,
                       uint32_t timeout_sec)
{
    int r = REAL(d)->enroll(REAL(d), hat, gid, timeout_sec);
    ALOGI("enroll gid %u -> %d", gid, r);
    return r;
}

static int shim_post_enroll(struct fingerprint_device *d)
{
    return REAL(d)->post_enroll(REAL(d));
}

static uint64_t shim_get_authenticator_id(struct fingerprint_device *d)
{
    return REAL(d)->get_authenticator_id(REAL(d));
}

static int shim_cancel(struct fingerprint_device *d)
{
    return REAL(d)->cancel(REAL(d));
}

static int shim_enumerate(struct fingerprint_device *d, fingerprint_finger_id_t *results,
                          uint32_t *max_size)
{
    pthread_mutex_lock(&enum_lock);
    enum_active = 1;
    enum_done = 0;
    enum_count = 0;
    pthread_mutex_unlock(&enum_lock);
    int r = REAL(d)->enumerate(REAL(d));
    pthread_mutex_lock(&enum_lock);
    if (r == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        while (!enum_done && pthread_cond_timedwait(&enum_cond, &enum_lock, &ts) == 0)
            ;
    }
    enum_active = 0;
    uint32_t n = enum_count < *max_size ? enum_count : *max_size;
    memcpy(results, enum_ids, n * sizeof(*results));
    *max_size = n;
    pthread_mutex_unlock(&enum_lock);
    return r < 0 ? r : (int)n;
}

static int shim_remove(struct fingerprint_device *d, uint32_t gid, uint32_t fid)
{
    return REAL(d)->remove(REAL(d), gid, fid);
}

static int shim_set_active_group(struct fingerprint_device *d, uint32_t gid,
                                 const char *store_path)
{
    int r = REAL(d)->set_active_group(REAL(d), gid, store_path);
    ALOGI("set_active_group %u %s -> %d", gid, store_path, r);
    return r;
}

static int shim_authenticate(struct fingerprint_device *d, uint64_t operation_id, uint32_t gid)
{
    return REAL(d)->authenticate(REAL(d), operation_id, gid);
}

static int shim_close(hw_device_t *dev)
{
    struct shim *s = (struct shim *)dev;
    if (s->real)
        s->real->common.close(&s->real->common);
    free(s);
    return 0;
}

static int shim_open(const hw_module_t *module, const char *id, hw_device_t **device)
{
    void *lib = dlopen(BLOB, RTLD_NOW);
    if (!lib) {
        ALOGE("dlopen %s: %s", BLOB, dlerror());
        return -ENODEV;
    }
    hw_module_t *hmi = dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR);
    if (!hmi) {
        ALOGE("no HMI in %s", BLOB);
        return -ENODEV;
    }
    hw_device_t *rd = NULL;
    int r = hmi->methods->open(hmi, id, &rd);
    if (r || !rd) {
        ALOGE("stock HAL open failed: %d", r);
        return r ? r : -ENODEV;
    }
    ALOGI("stock HAL open: device version 0x%x", rd->version);

    struct shim *s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->real = (struct fp21_device *)rd;
    s->lib = lib;
    fingerprint_device_t *d = &s->dev;
    d->common.tag = HARDWARE_DEVICE_TAG;
    d->common.version = FINGERPRINT_MODULE_API_VERSION_2_0;
    d->common.module = (hw_module_t *)module;
    d->common.close = shim_close;
    d->set_notify = shim_set_notify;
    d->pre_enroll = shim_pre_enroll;
    d->enroll = shim_enroll;
    d->post_enroll = shim_post_enroll;
    d->get_authenticator_id = shim_get_authenticator_id;
    d->cancel = shim_cancel;
    d->enumerate = shim_enumerate;
    d->remove = shim_remove;
    d->set_active_group = shim_set_active_group;
    d->authenticate = shim_authenticate;
    *device = &d->common;
    return 0;
}

static struct hw_module_methods_t methods = { .open = shim_open };

fingerprint_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = FINGERPRINT_MODULE_API_VERSION_2_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = FINGERPRINT_HARDWARE_MODULE_ID,
        .name = "TP1803 Sunwave fingerprint shim",
        .author = "TP1803 CM13 port",
        .methods = &methods,
    },
};
