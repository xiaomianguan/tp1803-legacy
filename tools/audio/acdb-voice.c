/* 32-bit: init the stock ACDB loader like the HAL and send voice cal.
 * acdb-voice <rx_id> <tx_id> */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct ln { struct ln *next, *prev; };
struct init4 { char *cvd; char *card; struct ln *keys; bool *inst; };

int main(int argc, char **argv)
{
    int rx = argc > 1 ? atoi(argv[1]) : 7, tx = argc > 2 ? atoi(argv[2]) : 4;
    void *h = dlopen("libacdbloader.so", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    int (*init)(struct init4 *, int) = dlsym(h, "acdb_loader_init_v4");
    int (*top)(void) = dlsym(h, "acdb_loader_send_common_custom_topology");
    void (*voice)(int, int) = dlsym(h, "acdb_loader_send_voice_cal");
    static struct ln keys = { &keys, &keys };
    static bool inst;
    struct init4 d = { "2.4", "sm8150-tavil-snd-card", &keys, &inst };
    printf("init %d\n", init(&d, 4));
    if (top) printf("topology %d\n", top());
    voice(rx, tx);
    printf("voice cal %d/%d sent\n", rx, tx);
    return 0;
}
