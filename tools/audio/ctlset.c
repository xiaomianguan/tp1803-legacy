/* Write a multi-value INT mixer control with raw ioctls (no range check):
 * ctlset "<name>" v1 [v2 ...]  (values parsed with strtoul, any base) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: ctlset name v...\n"); return 2; }
    int fd = open("/dev/snd/controlC0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct snd_ctl_elem_list l = {0};
    ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l);
    l.space = l.count;
    l.pids = calloc(l.count, sizeof(*l.pids));
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0) { perror("list"); return 1; }
    for (unsigned i = 0; i < l.used; i++) {
        if (strcmp((char *)l.pids[i].name, argv[1]))
            continue;
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof(v));
        v.id.numid = l.pids[i].numid;
        for (int k = 0; k + 2 < argc && k < 128; k++)
            v.value.integer.value[k] = (long)(int)strtoul(argv[k + 2], NULL, 0);
        int r = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
        printf("set %s (numid %u): %d\n", argv[1], v.id.numid, r);
        return r < 0;
    }
    fprintf(stderr, "no ctl\n");
    return 1;
}
