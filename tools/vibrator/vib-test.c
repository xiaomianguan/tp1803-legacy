/* Play force-feedback effects on the qti-haptics input device, like the
 * vibrator HAL does: vib-test [ms] [count] [gap_ms]
 * Each play re-uploads the same effect id (update) and starts it. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int open_haptics(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5))
            continue;
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int f = open(path, O_RDWR);
        if (f < 0)
            continue;
        if (ioctl(f, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, "qti-haptics")) {
            closedir(d);
            return f;
        }
        close(f);
    }
    return -1;
}

int main(int argc, char **argv)
{
    int ms = argc > 1 ? atoi(argv[1]) : 300;
    int count = argc > 2 ? atoi(argv[2]) : 1;
    int gap = argc > 3 ? atoi(argv[3]) : 500;
    int fd = open_haptics();
    if (fd < 0) { perror("qti-haptics"); return 1; }
    struct ff_effect eff;
    memset(&eff, 0, sizeof(eff));
    eff.id = -1;
    for (int i = 0; i < count; i++) {
        eff.type = FF_CONSTANT;
        eff.u.constant.level = 0x7fff;
        eff.replay.length = ms;
        if (ioctl(fd, EVIOCSFF, &eff) < 0) {
            printf("%d: EVIOCSFF: %s\n", i, strerror(errno));
            return 1;
        }
        struct input_event ev = { .type = EV_FF, .code = eff.id, .value = 1 };
        if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
            printf("%d: play: %s\n", i, strerror(errno));
            return 1;
        }
        usleep((ms + gap) * 1000);
    }
    printf("played %d x %d ms (effect id %d)\n", count, ms, eff.id);
    return 0;
}
