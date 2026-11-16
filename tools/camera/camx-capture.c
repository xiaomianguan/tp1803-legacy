/*
 * Phase-2 check: drive the CamX HAL directly (no framework), with buffers
 * from our gralloc, and save a frame.
 *
 * usage: camx-capture [camera_id] [width] [height] [frames] [out.yuv]
 *
 * CamX implements camera device HAL 3.5, whose structs extend Android 6's
 * camera3.h; the extended layouts are declared here.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <system/camera_metadata.h>

/* HAL 3.5 additions */
typedef struct {
    uint32_t num_streams;
    camera3_stream_t **streams;
    uint32_t operation_mode;
    const camera_metadata_t *session_parameters;
} stream_config_35_t;

typedef struct {
    uint32_t frame_number;
    const camera_metadata_t *settings;
    camera3_stream_buffer_t *input_buffer;
    uint32_t num_output_buffers;
    const camera3_stream_buffer_t *output_buffers;
    uint32_t num_physcam_settings;
    const char **physcam_id;
    const camera_metadata_t **physcam_settings;
} capture_request_35_t;

typedef struct {
    void (*process_capture_result)(const struct camera3_callback_ops *, const camera3_capture_result_t *);
    void (*notify)(const struct camera3_callback_ops *, const camera3_notify_msg_t *);
    void *request_stream_buffers;
    void *return_stream_buffers;
} callback_ops_35_t;

#define NBUF 8
static gralloc_module_t *gr;
static buffer_handle_t bufs[NBUF];
static int buf_busy[NBUF];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int results, errors, shutters, saved, want_save;
static int W = 1280, H = 720;
static const char *out_path = "/data/local/tmp/frame.yuv";
static int (*sync_wait_fn)(int, int);

static void save_frame(buffer_handle_t h)
{
    /* Dump the whole buffer: the plane layout CamX writes is what we want to learn. */
    const native_handle_t *nh = h;
    size_t size = nh->numInts > 2 ? (size_t)nh->data[nh->numFds + 2] : (size_t)W * H * 3 / 2;
    void *va = NULL;
    if (gr->lock(gr, h, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, W, H, &va) == 0 && va) {
        FILE *f = fopen(out_path, "wb");
        if (f) { fwrite(va, 1, size, f); fclose(f); }
        gr->unlock(gr, h);
        printf("saved %s (%zu bytes raw)\n", out_path, size);
        saved = 1;
    } else {
        printf("lock failed\n");
    }
}

static void on_result(const struct camera3_callback_ops *ops, const camera3_capture_result_t *r)
{
    (void)ops;
    pthread_mutex_lock(&lock);
    results++;
    for (uint32_t i = 0; i < r->num_output_buffers; i++) {
        const camera3_stream_buffer_t *b = &r->output_buffers[i];
        if (b->release_fence >= 0) {
            if (sync_wait_fn) sync_wait_fn(b->release_fence, 1000);
            close(b->release_fence);
        }
        for (int k = 0; k < NBUF; k++)
            if (b->buffer && *b->buffer == bufs[k]) {
                buf_busy[k] = 0;
                if (b->status != CAMERA3_BUFFER_STATUS_OK) errors++;
                else if (want_save && !saved) save_frame(bufs[k]);
            }
    }
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
}

static void on_notify(const struct camera3_callback_ops *ops, const camera3_notify_msg_t *m)
{
    (void)ops;
    if (m->type == CAMERA3_MSG_SHUTTER) shutters++;
    else if (m->type == CAMERA3_MSG_ERROR)
        printf("notify ERROR frame %u code %d\n", m->message.error.frame_number, m->message.error.error_code);
}

static callback_ops_35_t cb = { on_result, on_notify, NULL, NULL };

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *cam = argc > 1 ? argv[1] : "0";
    if (argc > 3) { W = atoi(argv[2]); H = atoi(argv[3]); }
    int frames = argc > 4 ? atoi(argv[4]) : 60;
    if (argc > 5) out_path = argv[5];
    int impl = argc > 6 && !strcmp(argv[6], "preview");   /* IMPLEMENTATION_DEFINED like SF preview */
    sync_wait_fn = dlsym(dlopen("libsync.so", RTLD_NOW), "sync_wait");

    int (*get_module)(const char *, const hw_module_t **) =
        dlsym(dlopen("libhardware.so", RTLD_NOW), "hw_get_module");
    if (!get_module || get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t **)&gr)) { printf("no gralloc\n"); return 1; }
    alloc_device_t *alloc;
    if (gralloc_open(&gr->common, &alloc)) { printf("gralloc_open failed\n"); return 1; }

    void *h = dlopen("/system/vendor/lib64/hw/camera.qcom.so", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    camera_module_t *m = dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
    if (m->init) m->init();
    printf("cameras: %d\n", m->get_number_of_cameras());

    camera3_device_t *dev;
    int r = m->common.methods->open(&m->common, cam, (hw_device_t **)&dev);
    printf("open %s: %d (device version 0x%x)\n", cam, r, r ? 0 : dev->common.version);
    if (r) return 1;
    r = dev->ops->initialize(dev, (const camera3_callback_ops_t *)&cb);
    printf("initialize: %d\n", r);

    camera3_stream_t st;
    memset(&st, 0, sizeof(st));
    st.stream_type = CAMERA3_STREAM_OUTPUT;
    st.width = W;
    st.height = H;
    st.format = impl ? HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED : HAL_PIXEL_FORMAT_YCbCr_420_888;
    st.usage = impl ? (GRALLOC_USAGE_HW_TEXTURE | (getenv("PREVIEW_SWREAD") ? GRALLOC_USAGE_SW_READ_RARELY : 0)) : GRALLOC_USAGE_SW_READ_OFTEN;
    st.data_space = HAL_DATASPACE_JFIF;
    camera3_stream_t *sl[1] = { &st };
    stream_config_35_t sc = { 1, sl, 0, NULL };
    r = dev->ops->configure_streams(dev, (camera3_stream_configuration_t *)&sc);
    printf("configure_streams: %d -> format 0x%x usage 0x%x max_buffers %u\n", r, st.format, st.usage, st.max_buffers);
    if (r) return 1;

    for (int i = 0; i < NBUF; i++) {
        int stride;
        r = alloc->alloc(alloc, W, H, st.format, st.usage | GRALLOC_USAGE_SW_READ_OFTEN, &bufs[i], &stride);
        if (r) { printf("alloc %d failed: %d\n", i, r); return 1; }
        if (i == 0) {
            const native_handle_t *nh = bufs[0];
            printf("buffer: stride %d, handle fds %d ints %d:", stride, nh->numFds, nh->numInts);
            for (int k = 0; k < nh->numFds + nh->numInts; k++) printf(" %d", nh->data[k]);
            printf("\n");
        }
    }

    const camera_metadata_t *settings = dev->ops->construct_default_request_settings(dev, CAMERA3_TEMPLATE_PREVIEW);
    printf("default preview settings: %p\n", settings);

    int inflight_max = st.max_buffers < NBUF ? (int)st.max_buffers : NBUF;
    for (int f = 0; f < frames; f++) {
        int slot = -1;
        pthread_mutex_lock(&lock);
        for (;;) {
            int busy = 0;
            for (int k = 0; k < NBUF; k++) busy += buf_busy[k];
            for (int k = 0; k < NBUF && slot < 0; k++) if (!buf_busy[k]) slot = k;
            if (slot >= 0 && busy < inflight_max) break;
            slot = -1;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 3;
            if (pthread_cond_timedwait(&cond, &lock, &ts) == ETIMEDOUT) {
                printf("timeout waiting for buffers at frame %d (results %d)\n", f, results);
                pthread_mutex_unlock(&lock);
                goto done;
            }
        }
        buf_busy[slot] = 1;
        want_save = f >= frames - 10;
        pthread_mutex_unlock(&lock);

        camera3_stream_buffer_t ob = { &st, &bufs[slot], 0, -1, -1 };
        capture_request_35_t rq = { (uint32_t)f, settings, NULL, 1, &ob, 0, NULL, NULL };
        r = dev->ops->process_capture_request(dev, (camera3_capture_request_t *)&rq);
        if (r) { printf("process_capture_request %d: %d\n", f, r); break; }
    }
    sleep(1);
done:
    printf("results %d shutters %d buffer errors %d saved %d\n", results, shutters, errors, saved);
    if (dev->ops->flush) dev->ops->flush(dev);
    dev->common.close(&dev->common);
    return 0;
}
