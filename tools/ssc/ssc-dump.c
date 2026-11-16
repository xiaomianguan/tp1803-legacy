/* Enable one SEE data type and dump its events' protobuf fields.
 * usage: ssc-dump <data_type> <rate_hz (0 = on-change)> [seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ssc.h"

static void dump(const uint8_t *p, size_t len, int depth)
{
    const uint8_t *e = p + len;
    while (p < e) {
        uint64_t key = 0, v = 0; int s = 0;
        while (p < e) { uint8_t c = *p++; key |= (uint64_t)(c & 0x7f) << s; s += 7; if (!(c & 0x80)) break; }
        int f = key >> 3, wt = key & 7;
        printf("%*s%d:", depth * 2, "", f);
        if (wt == 0) { s = 0; while (p < e) { uint8_t c = *p++; v |= (uint64_t)(c & 0x7f) << s; s += 7; if (!(c & 0x80)) break; } printf(" varint %llu\n", (unsigned long long)v); }
        else if (wt == 5) { float fl; uint32_t u; memcpy(&u, p, 4); memcpy(&fl, p, 4); p += 4; printf(" f32 %g (0x%x)\n", fl, u); }
        else if (wt == 1) { uint64_t u; memcpy(&u, p, 8); p += 8; printf(" f64 0x%llx\n", (unsigned long long)u); }
        else if (wt == 2) {
            s = 0; while (p < e) { uint8_t c = *p++; v |= (uint64_t)(c & 0x7f) << s; s += 7; if (!(c & 0x80)) break; }
            printf(" bytes[%llu]", (unsigned long long)v);
            if (v % 4 == 0 && v <= 64) { printf(" floats:"); for (size_t i = 0; i < v; i += 4) { float fl; memcpy(&fl, p + i, 4); printf(" %g", fl); } }
            printf("\n");
            if (depth < 2) dump(p, v, depth + 1);
            p += v;
        } else { printf(" wt %d?\n", wt); return; }
    }
}

static int n;
static void on_event(void *c, const struct ssc_suid *s, uint32_t msg, uint64_t ts, const uint8_t *p, size_t len)
{
    if (n++ >= 12) return;
    printf("msg %u ts %llu len %zu\n", msg, (unsigned long long)ts, len);
    dump(p, len, 1);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct ssc *s = ssc_open(on_event, NULL);
    struct ssc_suid su;
    if (!s || ssc_lookup(s, argv[1], &su, 2000)) { printf("%s not found\n", argv[1]); return 1; }
    printf("enable %s rate %s: %d\n", argv[1], argv[2], ssc_enable(s, &su, atof(argv[2])));
    int secs = argc > 3 ? atoi(argv[3]) : 4;
    for (int i = 0; i < secs * 10; i++) ssc_poll(s, 100);
    ssc_disable(s, &su);
    printf("events: %d\n", n);
    return 0;
}
