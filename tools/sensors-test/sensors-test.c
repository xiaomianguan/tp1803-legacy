/* Load the sensors HAL directly and print events from the given handles for a few seconds. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/sensors.h>
int main(int argc, char **argv)
{
    void *h = dlopen("/system/lib64/hw/sensors.msmnile.so", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    struct sensors_module_t *m = dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
    struct sensor_t const *list;
    int n = m->get_sensors_list(m, &list);
    for (int i = 0; i < n; i++) printf("  [%d] %s (type %d)\n", list[i].handle, list[i].name, list[i].type);
    struct sensors_poll_device_1 *d;
    if (m->common.methods->open(&m->common, SENSORS_HARDWARE_POLL, (struct hw_device_t **)&d)) return 1;
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    for (int i = 2; i < argc; i++) {
        int hd = atoi(argv[i]);
        d->batch(d, hd, 0, 100000000, 0);
        printf("activate %d: %d\n", hd, d->activate(&d->v0, hd, 1));
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    long long last[32] = {0};
    sensors_event_t ev[16];
    long long t0 = 0;
    for (;;) {
        int k = d->poll(&d->v0, ev, 16);
        for (int i = 0; i < k; i++) {
            if (!t0) t0 = ev[i].timestamp;
            if (ev[i].timestamp - t0 > secs * 1000000000LL) return 0;
            int s = ev[i].sensor & 31;
            if (ev[i].timestamp / 500000000 == last[s]) continue;   /* 2 lines/s/sensor */
            last[s] = ev[i].timestamp / 500000000;
            if (ev[i].type == SENSOR_TYPE_STEP_COUNTER)
                printf("%d t=%.2f steps %llu\n", ev[i].sensor, (ev[i].timestamp - t0) / 1e9,
                       (unsigned long long)ev[i].u64.step_counter);
            else
                printf("%d t=%.2f %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f\n", ev[i].sensor,
                       (ev[i].timestamp - t0) / 1e9, ev[i].data[0], ev[i].data[1], ev[i].data[2],
                       ev[i].data[3], ev[i].data[4], ev[i].data[5]);
        }
    }
    (void)n; (void)list;
}
