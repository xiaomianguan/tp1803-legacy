/* Look up a few sensors over SEE and stream samples. */
#include <stdio.h>
#include <string.h>
#include "ssc.h"

static int count;
static void on_event(void *ctx, const struct ssc_suid *suid, uint32_t msg_id,
                     uint64_t ts, const uint8_t *p, size_t len)
{
    (void)ctx;
    float f[8];
    int n = pb_floats(p, len, 1, f, 8);
    uint64_t prox = 0;
    if (msg_id == SNS_PROXIMITY_MSGID_SNS_PROXIMITY_EVENT && !pb_varint_field(p, len, 1, &prox))
        printf("[%016llx] proximity %s\n", (unsigned long long)suid->low, prox ? "NEAR" : "FAR");
    else if (n > 0 && count < 40)
        printf("[%016llx] msg %u ts %llu: %d values %.3f %.3f %.3f\n", (unsigned long long)suid->low,
               msg_id, (unsigned long long)ts, n, f[0], n > 1 ? f[1] : 0, n > 2 ? f[2] : 0);
    else if (count < 40)
        printf("[%016llx] msg %u (%zu bytes)\n", (unsigned long long)suid->low, msg_id, len);
    count++;
}

int main(int argc, char **argv)
{
    const char *types[] = { "accel", "gyro", "mag", "ambient_light", "proximity", "mag_cal", "gyro_cal", NULL };
    setvbuf(stdout, NULL, _IONBF, 0);
    struct ssc *s = ssc_open(on_event, NULL);
    if (!s) { printf("no SSC service\n"); return 1; }
    struct ssc_suid su[8]; int have[8] = {0};
    for (int i = 0; types[i]; i++) {
        if (ssc_lookup(s, types[i], &su[i], 2000) == 0) {
            have[i] = 1;
            printf("%-14s suid %016llx%016llx\n", types[i], (unsigned long long)su[i].high, (unsigned long long)su[i].low);
        } else printf("%-14s not found\n", types[i]);
    }
    const char *which = argc > 1 ? argv[1] : "accel";
    for (int i = 0; types[i]; i++)
        if (have[i] && !strcmp(types[i], which)) {
            float rate = (!strcmp(which, "ambient_light") || !strcmp(which, "proximity") || strstr(which, "_cal")) ? 0 : 10;
            printf("enabling %s (rate %.0f): %d\n", which, rate, ssc_enable(s, &su[i], rate));
            for (int t = 0; t < 40 && count < 30; t++) ssc_poll(s, 100);
            ssc_disable(s, &su[i]);
        }
    printf("events: %d\n", count);
    ssc_close(s);
    return 0;
}
