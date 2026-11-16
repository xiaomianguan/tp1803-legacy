/*
 * Sensors HAL for the Nubia Mini 5G (TP1803) on Android 6.
 *
 *  - accelerometer / gyroscope / magnetometer come from the SLPI's Snapdragon
 *    Sensor Core (SEE) over QRTR (see ssc.c). The SLPI only publishes them
 *    once hexagonrpcd has served it the sensor registry over FastRPC.
 *  - light and proximity are Nubia's kernel tmd2725 driver: sysfs enable
 *    switches + input devices "light" (REL_X = lux + 1) and "proximity"
 *    (REL_DIAL = 3 near / 10 far).
 */
#define _GNU_SOURCE
#define LOG_TAG "sensors.msmnile"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/sensors.h>

#include "ssc.h"

enum {
    H_ACCEL = 1, H_MAG, H_GYRO, H_LIGHT, H_PROX,
    H_GYRO_UNCAL, H_MAG_UNCAL, H_GRAVITY, H_LINACC, H_GAME_RV, H_ROTV, H_GEOMAG_RV,
    H_STEP_COUNTER, H_STEP_DETECTOR, H_SIG_MOTION, H_TILT,
    H_MAX
};

#define LIGHT_SYSFS "/sys/class/light/light/"
#define PROX_SYSFS  "/sys/class/proximity/proximity/"
#define PROX_NEAR_VALUE 3
#define PROX_FAR_CM 5.0f

/* How a SEE stream maps onto an Android event */
enum kind {
    K_VEC3,         /* data - cal bias (accel/gyro/mag) */
    K_UNCAL,        /* raw data + cal bias */
    K_GRAVITY,      /* "gravity" floats 0-2 */
    K_LINACC,       /* "gravity" floats 3-5 */
    K_QUAT,         /* x, y, z, w (+ heading accuracy) */
    K_STEP_COUNT,
    K_STEP_DETECT,
    K_TRIGGER,      /* tilt, significant motion */
};

/* Every Android sensor gets its own SEE client (QRTR socket), so two Android
 * sensors can share one SEE sensor at different rates. */
struct see_sensor {
    int handle, type;
    const char *see_type, *cal_type;
    enum kind kind;
    int on_change, wakeup, one_shot;
    struct ssc *ssc, *cal;
    struct ssc_suid suid, cal_suid;
    float bias[3];
    int cal_status;
    int enabled;
    int64_t period_ns;
    uint64_t step_base, step_last;   /* SEE restarts its count on every enable */
};

#define SEE(h, t, st, ct, k, oc, wu, os) \
    { h, t, st, ct, k, oc, wu, os, NULL, NULL, {0,0}, {0,0}, {0,0,0}, 3, 0, 200000000, 0, 0 }
static struct see_sensor see[] = {
    SEE(H_ACCEL,         SENSOR_TYPE_ACCELEROMETER,              "accel",     NULL,       K_VEC3,        0, 0, 0),
    SEE(H_MAG,           SENSOR_TYPE_MAGNETIC_FIELD,             "mag",       "mag_cal",  K_VEC3,        0, 0, 0),
    SEE(H_GYRO,          SENSOR_TYPE_GYROSCOPE,                  "gyro",      "gyro_cal", K_VEC3,        0, 0, 0),
    SEE(H_GYRO_UNCAL,    SENSOR_TYPE_GYROSCOPE_UNCALIBRATED,     "gyro",      "gyro_cal", K_UNCAL,       0, 0, 0),
    SEE(H_MAG_UNCAL,     SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED,"mag",       "mag_cal",  K_UNCAL,       0, 0, 0),
    SEE(H_GRAVITY,       SENSOR_TYPE_GRAVITY,                    "gravity",   NULL,       K_GRAVITY,     0, 0, 0),
    SEE(H_LINACC,        SENSOR_TYPE_LINEAR_ACCELERATION,        "gravity",   NULL,       K_LINACC,      0, 0, 0),
    SEE(H_GAME_RV,       SENSOR_TYPE_GAME_ROTATION_VECTOR,       "game_rv",   NULL,       K_QUAT,        0, 0, 0),
    SEE(H_ROTV,          SENSOR_TYPE_ROTATION_VECTOR,            "rotv",      NULL,       K_QUAT,        0, 0, 0),
    SEE(H_GEOMAG_RV,     SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR,"geomag_rv", NULL,       K_QUAT,        0, 0, 0),
    SEE(H_STEP_COUNTER,  SENSOR_TYPE_STEP_COUNTER,               "pedometer", NULL,       K_STEP_COUNT,  1, 0, 0),
    SEE(H_STEP_DETECTOR, SENSOR_TYPE_STEP_DETECTOR,              "pedometer", NULL,       K_STEP_DETECT, 1, 0, 0),
    SEE(H_SIG_MOTION,    SENSOR_TYPE_SIGNIFICANT_MOTION,         "sig_motion",NULL,       K_TRIGGER,     1, 1, 1),
    SEE(H_TILT,          SENSOR_TYPE_TILT_DETECTOR,              "tilt",      NULL,       K_TRIGGER,     1, 1, 0),
};
#define N_SEE (int)(sizeof(see) / sizeof(see[0]))

#define CONT(n, v, h, t, st, range, res, pw, mind, maxd) \
    { .name = n, .vendor = v, .version = 1, .handle = h, .type = t, .maxRange = range, \
      .resolution = res, .power = pw, .minDelay = mind, .maxDelay = maxd, \
      .stringType = st, .flags = SENSOR_FLAG_CONTINUOUS_MODE }
#define EVENT(n, v, h, t, st, range, res, pw, fl) \
    { .name = n, .vendor = v, .version = 1, .handle = h, .type = t, .maxRange = range, \
      .resolution = res, .power = pw, .minDelay = 0, .maxDelay = 0, .stringType = st, .flags = fl }
static const struct sensor_t all_sensors[] = {
    CONT("LSM6DSO Accelerometer", "STMicroelectronics", H_ACCEL, SENSOR_TYPE_ACCELEROMETER,
         SENSOR_STRING_TYPE_ACCELEROMETER, 78.4532f, 0.0023942f, 0.17f, 5000, 1000000),
    CONT("AK0991x Magnetometer", "AKM", H_MAG, SENSOR_TYPE_MAGNETIC_FIELD,
         SENSOR_STRING_TYPE_MAGNETIC_FIELD, 4912.0f, 0.15f, 1.1f, 20000, 1000000),
    CONT("LSM6DSO Gyroscope", "STMicroelectronics", H_GYRO, SENSOR_TYPE_GYROSCOPE,
         SENSOR_STRING_TYPE_GYROSCOPE, 34.906586f, 0.0012217f, 0.55f, 5000, 1000000),
    EVENT("TMD2725 Ambient Light", "ams", H_LIGHT, SENSOR_TYPE_LIGHT,
          SENSOR_STRING_TYPE_LIGHT, 30000.0f, 1.0f, 0.1f, SENSOR_FLAG_ON_CHANGE_MODE),
    EVENT("TMD2725 Proximity", "ams", H_PROX, SENSOR_TYPE_PROXIMITY,
          SENSOR_STRING_TYPE_PROXIMITY, PROX_FAR_CM, PROX_FAR_CM, 0.1f,
          SENSOR_FLAG_ON_CHANGE_MODE | SENSOR_FLAG_WAKE_UP),
    CONT("LSM6DSO Gyroscope Uncalibrated", "STMicroelectronics", H_GYRO_UNCAL,
         SENSOR_TYPE_GYROSCOPE_UNCALIBRATED, SENSOR_STRING_TYPE_GYROSCOPE_UNCALIBRATED,
         34.906586f, 0.0012217f, 0.55f, 5000, 1000000),
    CONT("AK0991x Magnetometer Uncalibrated", "AKM", H_MAG_UNCAL,
         SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, SENSOR_STRING_TYPE_MAGNETIC_FIELD_UNCALIBRATED,
         4912.0f, 0.15f, 1.1f, 20000, 1000000),
    CONT("Gravity", "Qualcomm", H_GRAVITY, SENSOR_TYPE_GRAVITY, SENSOR_STRING_TYPE_GRAVITY,
         19.6133f, 0.0023942f, 0.17f, 5000, 1000000),
    CONT("Linear Acceleration", "Qualcomm", H_LINACC, SENSOR_TYPE_LINEAR_ACCELERATION,
         SENSOR_STRING_TYPE_LINEAR_ACCELERATION, 78.4532f, 0.0023942f, 0.17f, 5000, 1000000),
    CONT("Game Rotation Vector", "Qualcomm", H_GAME_RV, SENSOR_TYPE_GAME_ROTATION_VECTOR,
         SENSOR_STRING_TYPE_GAME_ROTATION_VECTOR, 1.0f, 1.0f / 65536, 0.72f, 5000, 1000000),
    CONT("Rotation Vector", "Qualcomm", H_ROTV, SENSOR_TYPE_ROTATION_VECTOR,
         SENSOR_STRING_TYPE_ROTATION_VECTOR, 1.0f, 1.0f / 65536, 1.82f, 5000, 1000000),
    CONT("Geomagnetic Rotation Vector", "Qualcomm", H_GEOMAG_RV,
         SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR, SENSOR_STRING_TYPE_GEOMAGNETIC_ROTATION_VECTOR,
         1.0f, 1.0f / 65536, 1.27f, 20000, 1000000),
    EVENT("Step Counter", "Qualcomm", H_STEP_COUNTER, SENSOR_TYPE_STEP_COUNTER,
          SENSOR_STRING_TYPE_STEP_COUNTER, 4294967295.0f, 1.0f, 0.17f, SENSOR_FLAG_ON_CHANGE_MODE),
    EVENT("Step Detector", "Qualcomm", H_STEP_DETECTOR, SENSOR_TYPE_STEP_DETECTOR,
          SENSOR_STRING_TYPE_STEP_DETECTOR, 1.0f, 1.0f, 0.17f, SENSOR_FLAG_SPECIAL_REPORTING_MODE),
    EVENT("Significant Motion", "Qualcomm", H_SIG_MOTION, SENSOR_TYPE_SIGNIFICANT_MOTION,
          SENSOR_STRING_TYPE_SIGNIFICANT_MOTION, 1.0f, 1.0f, 0.17f,
          SENSOR_FLAG_ONE_SHOT_MODE | SENSOR_FLAG_WAKE_UP),
    EVENT("Tilt Detector", "Qualcomm", H_TILT, SENSOR_TYPE_TILT_DETECTOR,
          SENSOR_STRING_TYPE_TILT_DETECTOR, 1.0f, 1.0f, 0.17f,
          SENSOR_FLAG_SPECIAL_REPORTING_MODE | SENSOR_FLAG_WAKE_UP),
};
static struct sensor_t avail[H_MAX];
static int n_avail;

/* ------------------------------------------------------------ event queue */

#define QLEN 256
static sensors_event_t queue[QLEN];
static int q_head, q_count;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int wake_pipe[2] = { -1, -1 };

static void push(const sensors_event_t *ev)
{
    pthread_mutex_lock(&q_lock);
    if (q_count == QLEN) {               /* drop the oldest */
        q_head = (q_head + 1) % QLEN;
        q_count--;
    }
    queue[(q_head + q_count) % QLEN] = *ev;
    q_count++;
    pthread_mutex_unlock(&q_lock);
}

static void wake(void)
{
    char c = 1;
    write(wake_pipe[1], &c, 1);
}

static int64_t boottime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* SEE timestamps are QTimer ticks, i.e. the ARM generic timer. */
static int64_t qtimer_to_boottime(uint64_t ticks)
{
    uint64_t now, freq;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    int64_t boot = boottime_ns();
    if (!freq || ticks > now || now - ticks > freq)
        return boot;
    return boot - (int64_t)((now - ticks) * 1000000000ULL / freq);
}

/* ------------------------------------------------------------ SEE sensors */

static void see_init_event(sensors_event_t *ev, const struct see_sensor *s, uint64_t ts)
{
    memset(ev, 0, sizeof(*ev));
    ev->version = sizeof(*ev);
    ev->sensor = s->handle;
    ev->type = s->type;
    ev->timestamp = ts ? qtimer_to_boottime(ts) : boottime_ns();
}

static void see_event(void *ctx, const struct ssc_suid *suid, uint32_t msg_id,
                      uint64_t ts, const uint8_t *p, size_t len)
{
    struct see_sensor *s = ctx;
    sensors_event_t ev;
    float v[16];
    uint64_t status = 3, count;
    int n;

    if (s->cal_type && suid->low == s->cal_suid.low && suid->high == s->cal_suid.high) {
        if (msg_id == SNS_CAL_MSGID_SNS_CAL_EVENT && pb_floats(p, len, 1, v, 3) == 3) {
            memcpy(s->bias, v, sizeof(s->bias));
            if (!pb_varint_field(p, len, 4, &status))   /* sns_cal_event.status */
                s->cal_status = (int)status;
        }
        return;
    }
    if (!s->enabled)
        return;

    switch (s->kind) {
    case K_STEP_COUNT:
    case K_STEP_DETECT:
        if ((msg_id != SNS_PEDOMETER_MSGID_SNS_STEP_EVENT &&
             msg_id != SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_INITIAL_EVENT) ||
            pb_varint_field(p, len, 1, &count))
            return;
        see_init_event(&ev, s, ts);
        if (s->kind == K_STEP_COUNT) {
            s->step_last = count;
            ev.u64.step_counter = s->step_base + count;
            push(&ev);
        } else if (count > s->step_last) {
            s->step_last = count;
            ev.data[0] = 1.0f;
            push(&ev);
        }
        return;

    case K_TRIGGER:
        if (msg_id != SNS_TRIGGER_EVENT)
            return;
        see_init_event(&ev, s, ts);
        ev.data[0] = 1.0f;
        push(&ev);
        if (s->one_shot) {                      /* disarm after firing */
            s->enabled = 0;
            ssc_disable(s->ssc, &s->suid);
        }
        return;

    default:
        break;
    }

    if (msg_id != SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT)
        return;
    n = pb_floats(p, len, 1, v, 16);
    pb_varint_field(p, len, 2, &status);
    if (s->cal_type && s->cal_status < (int)status)
        status = s->cal_status;
    if (status > 3)
        status = 3;
    see_init_event(&ev, s, ts);

    switch (s->kind) {
    case K_VEC3:
        if (n < 3) return;
        for (int i = 0; i < 3; i++)
            ev.data[i] = v[i] - s->bias[i];
        if (s->type == SENSOR_TYPE_MAGNETIC_FIELD)
            ev.magnetic.status = (int8_t)status;
        else
            ev.acceleration.status = SENSOR_STATUS_ACCURACY_HIGH;
        break;
    case K_UNCAL:
        if (n < 3) return;
        for (int i = 0; i < 3; i++) {
            ev.uncalibrated_gyro.uncalib[i] = v[i];
            ev.uncalibrated_gyro.bias[i] = s->bias[i];
        }
        break;
    case K_GRAVITY:
    case K_LINACC: {
        int o = s->kind == K_LINACC ? 3 : 0;
        if (n < o + 3) return;
        for (int i = 0; i < 3; i++)
            ev.data[i] = v[o + i];
        ev.acceleration.status = (int8_t)status;
        break;
    }
    case K_QUAT:
        if (n < 4) return;
        for (int i = 0; i < 4; i++)
            ev.data[i] = v[i];
        if (s->type != SENSOR_TYPE_GAME_ROTATION_VECTOR) {
            /* estimated heading accuracy in radians; -1 when unknown */
            static const float acc[4] = { -1.0f, 0.70f, 0.35f, 0.10f };
            ev.data[4] = acc[status];
        }
        break;
    default:
        return;
    }
    push(&ev);
}

static void see_discover(void)
{
    /* hexagonrpcd may still be feeding the SLPI its registry: wait (up to 15 s)
     * for the first sensor to show up, then the rest are there too. */
    int64_t deadline = boottime_ns() + 15000000000LL;

    for (int i = 0; i < N_SEE; i++) {
        struct see_sensor *s = &see[i];
        do {
            s->ssc = ssc_open(see_event, s);
            if (s->ssc && ssc_lookup(s->ssc, s->see_type, &s->suid, 1000)) {
                ssc_close(s->ssc);
                s->ssc = NULL;
            }
            if (!s->ssc && i == 0)
                usleep(500000);
        } while (!s->ssc && i == 0 && boottime_ns() < deadline);
        if (!s->ssc) {
            ALOGE("SEE sensor '%s' not available", s->see_type);
            continue;
        }
        ssc_set_wakeup(s->ssc, s->wakeup);
        if (s->cal_type) {
            s->cal = ssc_open(see_event, s);
            if (s->cal && ssc_lookup(s->cal, s->cal_type, &s->cal_suid, 1000)) {
                ssc_close(s->cal);
                s->cal = NULL;
            }
        }
        ALOGI("SEE %s (handle %d): suid %016llx%016llx%s", s->see_type, s->handle,
              (unsigned long long)s->suid.high, (unsigned long long)s->suid.low,
              s->cal ? " (+cal)" : "");
    }
}

static struct see_sensor *see_by_handle(int handle)
{
    for (int i = 0; i < N_SEE; i++)
        if (see[i].handle == handle && see[i].ssc)
            return &see[i];
    return NULL;
}

static int see_apply(struct see_sensor *s)
{
    if (!s->enabled) {
        ssc_disable(s->ssc, &s->suid);
        if (s->cal)
            ssc_disable(s->cal, &s->cal_suid);
        /* the next enable restarts SEE's step count at 0 */
        s->step_base += s->step_last;
        s->step_last = 0;
        return 0;
    }
    if (s->cal)
        ssc_enable(s->cal, &s->cal_suid, 0);
    if (s->on_change)
        return ssc_enable(s->ssc, &s->suid, 0);
    float hz = 1e9f / (float)(s->period_ns > 0 ? s->period_ns : 200000000);
    if (hz > 200.0f) hz = 200.0f;
    if (hz < 1.0f) hz = 1.0f;
    return ssc_enable(s->ssc, &s->suid, hz);
}

/* ------------------------------------------------------------ light/prox */

static int light_fd = -1, prox_fd = -1;
static int light_enabled, prox_enabled;

static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    int r = write(fd, val, strlen(val));
    close(fd);
    return r < 0 ? -errno : 0;
}

static int sysfs_read_int(const char *path, int *out)
{
    char buf[32];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    *out = atoi(buf);
    return 0;
}

static int open_input(const char *want)
{
    DIR *d = opendir("/dev/input");
    struct dirent *de;
    int found = -1;
    if (!d) return -1;
    while ((de = readdir(d)) && found < 0) {
        char path[64], name[64] = "";
        if (strncmp(de->d_name, "event", 5)) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0 && !strcmp(name, want))
            found = fd;
        else
            close(fd);
    }
    closedir(d);
    return found;
}

static void push_prox(float cm)
{
    sensors_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = sizeof(ev);
    ev.sensor = H_PROX;
    ev.type = SENSOR_TYPE_PROXIMITY;
    ev.timestamp = boottime_ns();
    ev.distance = cm;
    push(&ev);
}

static void read_input(int fd, int handle)
{
    struct input_event ie[32];
    int n;
    while ((n = read(fd, ie, sizeof(ie))) > 0) {
        for (int i = 0; i < n / (int)sizeof(ie[0]); i++) {
            if (ie[i].type != EV_REL) continue;
            if (handle == H_LIGHT && ie[i].code == REL_X && light_enabled) {
                sensors_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.version = sizeof(ev);
                ev.sensor = H_LIGHT;
                ev.type = SENSOR_TYPE_LIGHT;
                ev.timestamp = boottime_ns();
                ev.light = ie[i].value > 0 ? ie[i].value - 1 : 0;
                push(&ev);
            } else if (handle == H_PROX && ie[i].code == REL_DIAL && prox_enabled) {
                push_prox(ie[i].value <= PROX_NEAR_VALUE ? 0.0f : PROX_FAR_CM);
            }
        }
    }
}

static int light_prox_activate(int handle, int en)
{
    if (handle == H_LIGHT) {
        if (light_fd < 0) return -ENODEV;
        light_enabled = en;
        return sysfs_write(LIGHT_SYSFS "enable", en ? "1" : "0");
    }
    if (prox_fd < 0) return -ENODEV;
    prox_enabled = en;
    int r = sysfs_write(PROX_SYSFS "enable", en ? "1" : "0");
    if (en) {
        /* The driver only reports changes; publish the current state now. */
        int det;
        if (sysfs_read_int(PROX_SYSFS "prox_detect", &det) == 0)
            push_prox(det <= PROX_NEAR_VALUE ? 0.0f : PROX_FAR_CM);
        wake();
    }
    return r;
}

/* ------------------------------------------------------------ HAL glue */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void init(void)
{
    pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK);
    see_discover();

    light_fd = open_input("light");
    prox_fd = open_input("proximity");
    if (light_fd >= 0) sysfs_write(LIGHT_SYSFS "dev_init", "1");
    if (prox_fd >= 0) sysfs_write(PROX_SYSFS "prox_init", "1");

    for (size_t i = 0; i < sizeof(all_sensors) / sizeof(all_sensors[0]); i++) {
        int h = all_sensors[i].handle, ok;
        if (h == H_LIGHT) ok = light_fd >= 0;
        else if (h == H_PROX) ok = prox_fd >= 0;
        else ok = see_by_handle(h) != NULL;
        if (ok)
            avail[n_avail++] = all_sensors[i];
    }
    ALOGI("%d sensors available", n_avail);
}

static int get_sensors_list(struct sensors_module_t *m, struct sensor_t const **list)
{
    (void)m;
    pthread_once(&init_once, init);
    *list = avail;
    return n_avail;
}

static int dev_activate(struct sensors_poll_device_t *d, int handle, int en)
{
    (void)d;
    int r;
    pthread_mutex_lock(&state_lock);
    if (handle == H_LIGHT || handle == H_PROX) {
        r = light_prox_activate(handle, en);
    } else {
        struct see_sensor *s = see_by_handle(handle);
        if (!s) {
            r = -EINVAL;
        } else {
            s->enabled = en;
            r = see_apply(s);
        }
    }
    pthread_mutex_unlock(&state_lock);
    wake();
    return r;
}

static int dev_batch(struct sensors_poll_device_1 *d, int handle, int flags,
                     int64_t period_ns, int64_t timeout)
{
    (void)d; (void)flags; (void)timeout;
    int r = 0;
    pthread_mutex_lock(&state_lock);
    if (handle == H_LIGHT) {
        char ms[16];
        snprintf(ms, sizeof(ms), "%lld", (long long)(period_ns / 1000000 < 200 ? 200 : period_ns / 1000000));
        sysfs_write(LIGHT_SYSFS "delay", ms);
    } else if (handle != H_PROX) {
        struct see_sensor *s = see_by_handle(handle);
        if (!s) {
            r = -EINVAL;
        } else {
            s->period_ns = period_ns;
            if (s->enabled)
                r = see_apply(s);
        }
    }
    pthread_mutex_unlock(&state_lock);
    return r;
}

static int dev_set_delay(struct sensors_poll_device_t *d, int handle, int64_t ns)
{
    return dev_batch((struct sensors_poll_device_1 *)d, handle, 0, ns, 0);
}

static int dev_flush(struct sensors_poll_device_1 *d, int handle)
{
    (void)d;
    int known = handle == H_LIGHT ? light_fd >= 0 : handle == H_PROX ? prox_fd >= 0 :
                see_by_handle(handle) != NULL;
    if (!known || handle == H_SIG_MOTION)     /* one-shot sensors can't flush */
        return -EINVAL;
    sensors_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = META_DATA_VERSION;
    ev.type = SENSOR_TYPE_META_DATA;
    ev.meta_data.what = META_DATA_FLUSH_COMPLETE;
    ev.meta_data.sensor = handle;
    push(&ev);
    wake();
    return 0;
}

static int dev_poll(struct sensors_poll_device_t *d, sensors_event_t *data, int count)
{
    (void)d;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        int n = 0;
        while (q_count && n < count) {
            data[n++] = queue[q_head];
            q_head = (q_head + 1) % QLEN;
            q_count--;
        }
        pthread_mutex_unlock(&q_lock);
        if (n)
            return n;

        struct pollfd pfd[3 + 2 * N_SEE];
        int np = 0;
        pfd[np++] = (struct pollfd){ wake_pipe[0], POLLIN, 0 };
        if (light_fd >= 0) pfd[np++] = (struct pollfd){ light_fd, POLLIN, 0 };
        if (prox_fd >= 0) pfd[np++] = (struct pollfd){ prox_fd, POLLIN, 0 };
        for (int i = 0; i < N_SEE; i++) {
            if (see[i].ssc) pfd[np++] = (struct pollfd){ ssc_fd(see[i].ssc), POLLIN, 0 };
            if (see[i].cal) pfd[np++] = (struct pollfd){ ssc_fd(see[i].cal), POLLIN, 0 };
        }
        if (poll(pfd, np, -1) < 0 && errno != EINTR)
            return -errno;

        char buf[64];
        while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
            ;
        if (light_fd >= 0) read_input(light_fd, H_LIGHT);
        if (prox_fd >= 0) read_input(prox_fd, H_PROX);
        pthread_mutex_lock(&state_lock);    /* see_event() updates sensor state */
        for (int i = 0; i < N_SEE; i++) {
            if (see[i].ssc) ssc_poll(see[i].ssc, 0);
            if (see[i].cal) ssc_poll(see[i].cal, 0);
        }
        pthread_mutex_unlock(&state_lock);
    }
}

static int dev_close(struct hw_device_t *dev)
{
    free(dev);
    return 0;
}

static int open_sensors(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    if (strcmp(id, SENSORS_HARDWARE_POLL))
        return -EINVAL;
    pthread_once(&init_once, init);

    struct sensors_poll_device_1 *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -ENOMEM;
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = SENSORS_DEVICE_API_VERSION_1_3;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = dev_close;
    dev->activate = dev_activate;
    dev->setDelay = dev_set_delay;
    dev->poll = dev_poll;
    dev->batch = dev_batch;
    dev->flush = dev_flush;
    *device = &dev->common;
    return 0;
}

static struct hw_module_methods_t sensors_methods = { .open = open_sensors };

struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = SENSORS_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = SENSORS_HARDWARE_MODULE_ID,
        .name = "TP1803 sensors (SEE + tmd2725)",
        .author = "TP1803 CM13 port",
        .methods = &sensors_methods,
    },
    .get_sensors_list = get_sensors_list,
};
