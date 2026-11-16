/*
 * Vibrator HAL for Nubia TP1803 (Android 6 / CM13).
 *
 * The PM8150B haptics are exposed by the 4.14 kernel as the force-feedback
 * input device "qti-haptics" (no timed_output). vibrator_on(ms) uploads an
 * FF_CONSTANT effect lasting ms and plays it, which is what the QTI vibrator
 * service does on newer Android.
 */
#define LOG_TAG "vibrator.msmnile"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/vibrator.h>

#define HAPTICS_NAME "qti-haptics"

struct ff_vibrator {
    vibrator_device_t dev;
    int fd;
    int effect_id;
    pthread_mutex_t lock;
};

static int open_haptics(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    int fd = -1;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int f = open(path, O_RDWR | O_CLOEXEC);
        if (f < 0)
            continue;
        if (ioctl(f, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, HAPTICS_NAME)) {
            fd = f;
            break;
        }
        close(f);
    }
    if (d)
        closedir(d);
    return fd;
}

#define MIN_ON_MS 50

static int play(struct ff_vibrator *v, int on)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    ev.code = v->effect_id;
    ev.value = on;
    return write(v->fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -errno;
}

/* Upload (or update) the constant effect and start it. Caller holds lock. */
static int start_locked(struct ff_vibrator *v, unsigned int timeout_ms)
{
    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_CONSTANT;
    effect.id = v->effect_id;              /* -1: new effect, else update */
    effect.u.constant.level = 0x7fff;
    effect.replay.length = timeout_ms;
    effect.replay.delay = 0;
    if (ioctl(v->fd, EVIOCSFF, &effect) < 0) {
        int e = errno;
        ALOGE("upload %u ms (effect %d) failed: %s", timeout_ms, v->effect_id, strerror(e));
        return -e;
    }
    v->effect_id = effect.id;
    int ret = play(v, 1);
    if (ret)
        ALOGE("play effect %d failed: %s", v->effect_id, strerror(-ret));
    return ret;
}

/* Drop the effect slot and the fd and start over with a fresh device handle. */
static int reopen_locked(struct ff_vibrator *v)
{
    if (v->fd >= 0) {
        if (v->effect_id >= 0)
            ioctl(v->fd, EVIOCRMFF, v->effect_id);
        close(v->fd);
    }
    v->effect_id = -1;
    v->fd = open_haptics();
    ALOGW("reopened %s: fd %d", HAPTICS_NAME, v->fd);
    return v->fd >= 0 ? 0 : -ENODEV;
}

static int vibra_on(vibrator_device_t *dev, unsigned int timeout_ms)
{
    struct ff_vibrator *v = (struct ff_vibrator *)dev;
    int ret;

    if (!timeout_ms)
        return 0;
    if (timeout_ms > 0xffff)
        timeout_ms = 0xffff;
    /* The LRA doesn't spin up in under ~50 ms; stretch short haptic ticks. */
    if (timeout_ms < MIN_ON_MS)
        timeout_ms = MIN_ON_MS;

    pthread_mutex_lock(&v->lock);
    ret = v->fd >= 0 ? start_locked(v, timeout_ms) : -ENODEV;
    if (ret && reopen_locked(v) == 0)
        ret = start_locked(v, timeout_ms);
    pthread_mutex_unlock(&v->lock);
    return ret;
}

static int vibra_off(vibrator_device_t *dev)
{
    struct ff_vibrator *v = (struct ff_vibrator *)dev;
    int ret = 0;
    pthread_mutex_lock(&v->lock);
    if (v->fd >= 0 && v->effect_id >= 0) {
        ret = play(v, 0);
        if (ret)
            ALOGE("stop effect %d failed: %s", v->effect_id, strerror(-ret));
    }
    pthread_mutex_unlock(&v->lock);
    return ret;
}

static int vibra_close(hw_device_t *device)
{
    struct ff_vibrator *v = (struct ff_vibrator *)device;
    if (v->fd >= 0) {
        if (v->effect_id >= 0)
            ioctl(v->fd, EVIOCRMFF, v->effect_id);
        close(v->fd);
    }
    free(v);
    return 0;
}

static int vibra_open(const hw_module_t *module, const char *id __unused,
                      hw_device_t **device)
{
    int fd = open_haptics();
    if (fd < 0) {
        ALOGE("no %s input device", HAPTICS_NAME);
        return -ENODEV;
    }
    struct ff_vibrator *v = calloc(1, sizeof(*v));
    if (!v) {
        close(fd);
        return -ENOMEM;
    }
    v->fd = fd;
    v->effect_id = -1;
    pthread_mutex_init(&v->lock, NULL);
    v->dev.common.tag = HARDWARE_DEVICE_TAG;
    v->dev.common.module = (hw_module_t *)module;
    v->dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    v->dev.common.close = vibra_close;
    v->dev.vibrator_on = vibra_on;
    v->dev.vibrator_off = vibra_off;
    *device = (hw_device_t *)v;
    return 0;
}

static struct hw_module_methods_t vibrator_module_methods = {
    .open = vibra_open,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = VIBRATOR_API_VERSION,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = VIBRATOR_HARDWARE_MODULE_ID,
    .name = "TP1803 force-feedback vibrator HAL",
    .author = "TP1803 legacy port",
    .methods = &vibrator_module_methods,
};
