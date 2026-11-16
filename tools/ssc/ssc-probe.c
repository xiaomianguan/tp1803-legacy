/* List which SEE data types resolve to a SUID. usage: ssc-probe [type...] */
#include <stdio.h>
#include "ssc.h"
static void on_event(void *c, const struct ssc_suid *s, uint32_t m, uint64_t t, const uint8_t *p, size_t l) {}
int main(int argc, char **argv)
{
    static const char *def[] = { "accel", "gyro", "mag", "ambient_light", "proximity", "prox",
        "als", "rgb", "hall", "pressure", "humidity", "ambient_temperature", "sensor_temperature",
        "gravity", "linear_acceleration", "game_rv", "rotv", "geomag_rv", "sig_motion",
        "step_detect", "pedometer", "device_orient", "tilt", "amd", "gyro_cal", "mag_cal",
        "accel_cal", "uv", "gas", "motion_detect", "oem1", "ambient_light_rgb", NULL };
    const char **types = argc > 1 ? (const char **)argv + 1 : def;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct ssc *s = ssc_open(on_event, NULL);
    if (!s) return 1;
    for (int i = 0; types[i]; i++) {
        struct ssc_suid su;
        if (ssc_lookup(s, types[i], &su, 700) == 0)
            printf("%-22s %016llx%016llx\n", types[i], (unsigned long long)su.high, (unsigned long long)su.low);
    }
    ssc_close(s);
    return 0;
}
