/*
 * libnativewindow.so for Android 6 (M).
 *
 * Android 8+ exports ANativeWindow_* / AHardwareBuffer_* entry points from
 * libnativewindow.so; Mesa's Android EGL platform calls those. On M the same
 * operations are function pointers on the ANativeWindow / ANativeWindowBuffer
 * structs (system/window.h), whose ABI is unchanged. This shim maps one onto
 * the other. AHardwareBuffer is represented by the ANativeWindowBuffer itself.
 */
#include <errno.h>
#include <stdint.h>
#include <system/window.h>

#define EXPORT __attribute__((visibility("default")))

typedef struct AHardwareBuffer AHardwareBuffer;

EXPORT void ANativeWindow_acquire(struct ANativeWindow *w)
{
    w->common.incRef(&w->common);
}

EXPORT void ANativeWindow_release(struct ANativeWindow *w)
{
    w->common.decRef(&w->common);
}

EXPORT int ANativeWindow_query(const struct ANativeWindow *w, int what, int *value)
{
    return w->query(w, what, value);
}

EXPORT int ANativeWindow_getFormat(struct ANativeWindow *w)
{
    int v = 0;
    int r = w->query(w, NATIVE_WINDOW_FORMAT, &v);
    return r < 0 ? r : v;
}

EXPORT int ANativeWindow_setSwapInterval(struct ANativeWindow *w, int interval)
{
    return w->setSwapInterval(w, interval);
}

EXPORT int ANativeWindow_dequeueBuffer(struct ANativeWindow *w,
                                       struct ANativeWindowBuffer **buf, int *fenceFd)
{
    return w->dequeueBuffer(w, buf, fenceFd);
}

EXPORT int ANativeWindow_queueBuffer(struct ANativeWindow *w,
                                     struct ANativeWindowBuffer *buf, int fenceFd)
{
    return w->queueBuffer(w, buf, fenceFd);
}

EXPORT int ANativeWindow_cancelBuffer(struct ANativeWindow *w,
                                      struct ANativeWindowBuffer *buf, int fenceFd)
{
    return w->cancelBuffer(w, buf, fenceFd);
}

EXPORT int ANativeWindow_setUsage(struct ANativeWindow *w, uint64_t usage)
{
    /* M's gralloc usage is 32-bit */
    return w->perform(w, NATIVE_WINDOW_SET_USAGE, (int)(uint32_t)usage);
}

EXPORT int ANativeWindow_setSharedBufferMode(struct ANativeWindow *w, int mode)
{
    (void)w; (void)mode;
    return -ENOSYS; /* no such perform code on M */
}

EXPORT AHardwareBuffer *ANativeWindowBuffer_getHardwareBuffer(struct ANativeWindowBuffer *b)
{
    return (AHardwareBuffer *)b;
}

EXPORT void AHardwareBuffer_acquire(AHardwareBuffer *ahb)
{
    struct ANativeWindowBuffer *b = (struct ANativeWindowBuffer *)ahb;
    b->common.incRef(&b->common);
}

EXPORT void AHardwareBuffer_release(AHardwareBuffer *ahb)
{
    struct ANativeWindowBuffer *b = (struct ANativeWindowBuffer *)ahb;
    b->common.decRef(&b->common);
}
