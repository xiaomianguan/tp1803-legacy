/*
 * Camera HAL wrapper: presents the stock Qualcomm CamX HAL (camera device
 * HAL 3.5) to Android 6's camera service, which speaks device HAL 3.3.
 *
 * Selected with ro.hardware.camera=tp1803 (camera.tp1803.so); CamX itself is
 * loaded from /system/vendor/lib64/hw/camera.qcom.so.
 *
 * What it translates:
 *  - device_version is reported as 3.3;
 *  - camera3_stream_t.reserved[0] is physical_camera_id in HAL 3.4+; Android
 *    6 leaves it uninitialised, so it's cleared before configure_streams;
 *  - stream configurations and capture requests gained trailing fields
 *    (session parameters, physical camera settings) that CamX reads, so they
 *    are passed as zero-extended copies;
 *  - the callback table gained request/return_stream_buffers, which CamX
 *    gets as NULL (HAL buffer management stays off).
 *
 * Timestamps: CamX stamps frames with CLOCK_BOOTTIME; Android 6 expects
 * CLOCK_MONOTONIC (preview presentation, A/V sync in recordings). They
 * differ by the time spent suspended, so the shutter timestamp and the
 * result's ANDROID_SENSOR_TIMESTAMP are both shifted (M requires them equal).
 *
 * It also holds a CPU latency PM QoS request while any camera is open: with
 * the msmnile deep idle states (cluster/LLCC power collapse, ~6.5 ms exit)
 * enabled, the IFE never completes the first frame and CamX loops in
 * recovery. Stock does the same through a perf lock (perfd), absent here.
 */
#define LOG_TAG "CameraWrapper"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cutils/log.h>
#include <hardware/camera3.h>
#include <system/camera_metadata.h>
#include <hardware/hardware.h>

#define CAMX_PATH "/system/vendor/lib64/hw/camera.qcom.so"

typedef struct {
    uint32_t num_streams;
    camera3_stream_t **streams;
    uint32_t operation_mode;
    const camera_metadata_t *session_parameters;      /* 3.5 */
} stream_config_35_t;

typedef struct {
    uint32_t frame_number;
    const camera_metadata_t *settings;
    camera3_stream_buffer_t *input_buffer;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    uint32_t num_physcam_settings;                    /* 3.4 */
    const char **physcam_id;
    const camera_metadata_t **physcam_settings;
} capture_request_35_t;

typedef struct {
    void (*process_capture_result)(const struct camera3_callback_ops *, const camera3_capture_result_t *);
    void (*notify)(const struct camera3_callback_ops *, const camera3_notify_msg_t *);
    void *request_stream_buffers;                     /* 3.5 */
    void *return_stream_buffers;
} callback_ops_35_t;

struct wrapper {
    camera3_device_t base;          /* what the framework sees */
    camera3_device_t *real;         /* CamX's device */
    const camera3_callback_ops_t *fw_cb;
    callback_ops_35_t cb;           /* what CamX calls */
    int64_t ts_offset;              /* CLOCK_BOOTTIME - CLOCK_MONOTONIC, ns */
};

/* Allow only WFI-level idle (~43-51 us). Little-core C1 power collapse
 * (909 us exit) is enough to slow the pipeline to ~5 fps. */
#define CAMERA_CPU_LATENCY_US 100

static camera_module_t *camx;
static pthread_mutex_t qos_lock = PTHREAD_MUTEX_INITIALIZER;
static int qos_fd = -1;
static int open_cameras;

static void qos_get(void)
{
    pthread_mutex_lock(&qos_lock);
    if (open_cameras++ == 0) {
        qos_fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        int32_t us = CAMERA_CPU_LATENCY_US;
        if (qos_fd < 0 || write(qos_fd, &us, sizeof(us)) != sizeof(us))
            ALOGE("cpu_dma_latency request failed: %s", strerror(errno));
    }
    pthread_mutex_unlock(&qos_lock);
}

static void qos_put(void)
{
    pthread_mutex_lock(&qos_lock);
    if (--open_cameras == 0 && qos_fd >= 0) {
        close(qos_fd);      /* closing the fd drops the request */
        qos_fd = -1;
    }
    pthread_mutex_unlock(&qos_lock);
}
static pthread_once_t load_once = PTHREAD_ONCE_INIT;

static void load_camx(void)
{
    void *h = dlopen(CAMX_PATH, RTLD_NOW);
    if (!h) {
        ALOGE("dlopen %s: %s", CAMX_PATH, dlerror());
        return;
    }
    camx = dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
    if (!camx) {
        ALOGE("%s has no HAL module", CAMX_PATH);
        return;
    }
    if (camx->init) {
        int r = camx->init();
        ALOGI("CamX init: %d", r);
    }
}

static int ensure_camx(void)
{
    pthread_once(&load_once, load_camx);
    return camx ? 0 : -ENODEV;
}

static struct wrapper *from_dev(const camera3_device_t *d)
{
    return (struct wrapper *)d->priv;
}

static struct wrapper *from_cb(const struct camera3_callback_ops *ops)
{
    return (struct wrapper *)((char *)ops - offsetof(struct wrapper, cb));
}

/* ---- timestamps: CLOCK_BOOTTIME (CamX) -> CLOCK_MONOTONIC (framework) ---- */

static int64_t clock_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* The offset only changes across a suspend; keep one cached value so a
 * frame's shutter and result timestamps are shifted by the same amount. */
static int64_t boot_mono_offset(struct wrapper *w)
{
    int64_t now = clock_ns(CLOCK_BOOTTIME) - clock_ns(CLOCK_MONOTONIC);
    int64_t cur = __atomic_load_n(&w->ts_offset, __ATOMIC_RELAXED);
    if (now - cur > 1000000 || cur - now > 1000000) {
        __atomic_store_n(&w->ts_offset, now, __ATOMIC_RELAXED);
        cur = now;
    }
    return cur;
}

/* ---- callbacks: CamX -> framework ---- */

static void cb_result(const struct camera3_callback_ops *ops, const camera3_capture_result_t *r)
{
    struct wrapper *w = from_cb(ops);
    camera_metadata_ro_entry_t e;
    if (r->result && find_camera_metadata_ro_entry(r->result, ANDROID_SENSOR_TIMESTAMP, &e) == 0 &&
        e.count == 1) {
        camera_metadata_t *md = clone_camera_metadata(r->result);
        if (md) {
            int64_t ts = e.data.i64[0] - boot_mono_offset(w);
            camera_metadata_entry_t me;
            if (find_camera_metadata_entry(md, ANDROID_SENSOR_TIMESTAMP, &me) == 0)
                update_camera_metadata_entry(md, me.index, &ts, 1, NULL);
            camera3_capture_result_t copy = *r;
            copy.result = md;
            w->fw_cb->process_capture_result(w->fw_cb, &copy);
            free_camera_metadata(md);
            return;
        }
    }
    w->fw_cb->process_capture_result(w->fw_cb, r);
}

static void cb_notify(const struct camera3_callback_ops *ops, const camera3_notify_msg_t *m)
{
    struct wrapper *w = from_cb(ops);
    if (m->type == CAMERA3_MSG_SHUTTER) {
        camera3_notify_msg_t copy = *m;
        copy.message.shutter.timestamp -= boot_mono_offset(w);
        w->fw_cb->notify(w->fw_cb, &copy);
        return;
    }
    w->fw_cb->notify(w->fw_cb, m);
}

/* ---- device ops: framework -> CamX ---- */

static int dev_initialize(const struct camera3_device *d, const camera3_callback_ops_t *cb)
{
    struct wrapper *w = from_dev(d);
    w->fw_cb = cb;
    w->cb.process_capture_result = cb_result;
    w->cb.notify = cb_notify;
    w->cb.request_stream_buffers = NULL;
    w->cb.return_stream_buffers = NULL;
    return w->real->ops->initialize(w->real, (const camera3_callback_ops_t *)&w->cb);
}

static int dev_configure_streams(const struct camera3_device *d, camera3_stream_configuration_t *list)
{
    struct wrapper *w = from_dev(d);
    if (!list)
        return -EINVAL;
    for (uint32_t i = 0; i < list->num_streams; i++)
        memset(list->streams[i]->reserved, 0, sizeof(list->streams[i]->reserved));
    stream_config_35_t c = { list->num_streams, list->streams, list->operation_mode, NULL };
    int r = w->real->ops->configure_streams(w->real, (camera3_stream_configuration_t *)&c);
    for (uint32_t i = 0; i < list->num_streams; i++) {
        camera3_stream_t *s = list->streams[i];
        ALOGI("stream %u: %ux%u format 0x%x usage 0x%x max_buffers %u -> %d",
              i, s->width, s->height, s->format, s->usage, s->max_buffers, r);
    }
    return r;
}

static const camera_metadata_t *dev_default_settings(const struct camera3_device *d, int type)
{
    struct wrapper *w = from_dev(d);
    return w->real->ops->construct_default_request_settings(w->real, type);
}

static int dev_process_capture_request(const struct camera3_device *d, camera3_capture_request_t *rq)
{
    struct wrapper *w = from_dev(d);
    if (!rq)
        return -EINVAL;
    capture_request_35_t r = {
        rq->frame_number, rq->settings, rq->input_buffer,
        rq->num_output_buffers, rq->output_buffers, 0, NULL, NULL,
    };
    return w->real->ops->process_capture_request(w->real, (camera3_capture_request_t *)&r);
}

static void dev_get_vendor_tag_ops(const struct camera3_device *d, vendor_tag_query_ops_t *ops)
{
    struct wrapper *w = from_dev(d);
    if (w->real->ops->get_metadata_vendor_tag_ops)
        w->real->ops->get_metadata_vendor_tag_ops(w->real, ops);
}

static void dev_dump(const struct camera3_device *d, int fd)
{
    struct wrapper *w = from_dev(d);
    if (w->real->ops->dump)
        w->real->ops->dump(w->real, fd);
}

static int dev_flush(const struct camera3_device *d)
{
    struct wrapper *w = from_dev(d);
    return w->real->ops->flush ? w->real->ops->flush(w->real) : 0;
}

static camera3_device_ops_t wrapper_ops = {
    .initialize = dev_initialize,
    .configure_streams = dev_configure_streams,
    .register_stream_buffers = NULL,     /* must be NULL since HAL 3.2 */
    .construct_default_request_settings = dev_default_settings,
    .process_capture_request = dev_process_capture_request,
    .get_metadata_vendor_tag_ops = dev_get_vendor_tag_ops,
    .dump = dev_dump,
    .flush = dev_flush,
};

static int dev_close(hw_device_t *dev)
{
    struct wrapper *w = from_dev((camera3_device_t *)dev);
    int r = w->real->common.close(&w->real->common);
    free(w);
    qos_put();
    return r;
}

/* ---- module ---- */

static int mod_get_number_of_cameras(void)
{
    return ensure_camx() ? 0 : camx->get_number_of_cameras();
}

static int mod_get_camera_info(int id, struct camera_info *info)
{
    if (ensure_camx())
        return -ENODEV;
    int r = camx->get_camera_info(id, info);
    if (r == 0 && info->device_version > CAMERA_DEVICE_API_VERSION_3_3)
        info->device_version = CAMERA_DEVICE_API_VERSION_3_3;
    return r;
}

static int mod_set_callbacks(const camera_module_callbacks_t *cb)
{
    return ensure_camx() ? -ENODEV : camx->set_callbacks(cb);
}

static void mod_get_vendor_tag_ops(vendor_tag_ops_t *ops)
{
    if (!ensure_camx() && camx->get_vendor_tag_ops)
        camx->get_vendor_tag_ops(ops);
}

static int mod_set_torch_mode(const char *id, bool enabled)
{
    if (ensure_camx())
        return -ENODEV;
    return camx->set_torch_mode ? camx->set_torch_mode(id, enabled) : -ENOSYS;
}

static int mod_open_legacy(const struct hw_module_t *m, const char *id, uint32_t v, struct hw_device_t **d)
{
    (void)m; (void)id; (void)v; (void)d;
    return -ENOSYS;
}

static int mod_init(void)
{
    return ensure_camx();
}

static int mod_open(const hw_module_t *module, const char *id, hw_device_t **device)
{
    if (ensure_camx())
        return -ENODEV;
    hw_device_t *hw = NULL;
    int r = camx->common.methods->open(&camx->common, id, &hw);
    if (r || !hw)
        return r ? r : -ENODEV;

    struct wrapper *w = calloc(1, sizeof(*w));
    if (!w) {
        hw->close(hw);
        return -ENOMEM;
    }
    w->real = (camera3_device_t *)hw;
    w->base.common.tag = HARDWARE_DEVICE_TAG;
    w->base.common.version = CAMERA_DEVICE_API_VERSION_3_3;
    w->base.common.module = (hw_module_t *)module;
    w->base.common.close = dev_close;
    w->base.ops = &wrapper_ops;
    w->base.priv = w;
    *device = &w->base.common;
    qos_get();
    ALOGI("opened camera %s (CamX device version 0x%x)", id, hw->version);
    return 0;
}

static hw_module_methods_t methods = { .open = mod_open };

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "TP1803 CamX wrapper",
        .author = "TP1803 CM13 port",
        .methods = &methods,
    },
    .get_number_of_cameras = mod_get_number_of_cameras,
    .get_camera_info = mod_get_camera_info,
    .set_callbacks = mod_set_callbacks,
    .get_vendor_tag_ops = mod_get_vendor_tag_ops,
    .open_legacy = mod_open_legacy,
    .set_torch_mode = mod_set_torch_mode,
    .init = mod_init,
};
