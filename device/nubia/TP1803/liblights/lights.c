/*
 * Minimal lights HAL for TP1803 (CM13 port).
 * Backlight: /sys/class/backlight/panel0-backlight/brightness (0..4095)
 * LEDs: red / green only (no blue on this unit)
 * Licensed under the Apache License, Version 2.0
 */
#define LOG_TAG "lights.tp1803"

#include <cutils/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hardware/lights.h>

#define BACKLIGHT "/sys/class/backlight/panel0-backlight/brightness"
#define RED_LED   "/sys/class/leds/red/brightness"
#define GREEN_LED "/sys/class/leds/green/brightness"
#define BL_MAX    4095

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static struct light_state_t g_battery;

static int write_int(const char *path, int value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        ALOGE("open %s: %s", path, strerror(errno));
        return -errno;
    }
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t r = write(fd, buf, len);
    close(fd);
    return r == len ? 0 : -errno;
}

static int rgb_to_brightness(const struct light_state_t *s)
{
    int c = s->color & 0x00ffffff;
    return ((77 * ((c >> 16) & 0xff)) + (150 * ((c >> 8) & 0xff)) + (29 * (c & 0xff))) >> 8;
}

static int set_backlight(struct light_device_t *dev, const struct light_state_t *s)
{
    (void)dev;
    int b = rgb_to_brightness(s);
    /* Kernel node is linear 0..4095 and nothing visible happens below ~5 */
    int v = b ? (b * BL_MAX + 127) / 255 : 0;
    if (b && v < 5) v = 5;
    pthread_mutex_lock(&g_lock);
    int err = write_int(BACKLIGHT, v);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static void update_leds_locked(void)
{
    const struct light_state_t *s =
        (g_notification.color & 0x00ffffff) ? &g_notification : &g_battery;
    int c = s->color & 0x00ffffff;
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    /* Fold blue into green; only red and green exist */
    if (b > g) g = b;
    write_int(RED_LED, r);
    write_int(GREEN_LED, g);
}

static int set_notifications(struct light_device_t *dev, const struct light_state_t *s)
{
    (void)dev;
    pthread_mutex_lock(&g_lock);
    g_notification = *s;
    update_leds_locked();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int set_battery(struct light_device_t *dev, const struct light_state_t *s)
{
    (void)dev;
    pthread_mutex_lock(&g_lock);
    g_battery = *s;
    update_leds_locked();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int close_lights(struct light_device_t *dev)
{
    free(dev);
    return 0;
}

static int open_lights(const struct hw_module_t *module, const char *name,
                       struct hw_device_t **device)
{
    int (*set_light)(struct light_device_t *, const struct light_state_t *);

    if (!strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_backlight;
    else if (!strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_notifications;
    else if (!strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_battery;
    else
        return -EINVAL;

    struct light_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -ENOMEM;
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = (int (*)(struct hw_device_t *))close_lights;
    dev->set_light = set_light;
    *device = (struct hw_device_t *)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "TP1803 lights module",
    .author = "TP1803 legacy port",
    .methods = &lights_module_methods,
};
