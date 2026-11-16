/*
 * mdm-ctl: issue esoc commands to the external modem (X50) via the esoc
 * command engine. Used to hard power-cycle an X50 that is stuck in its crash
 * (ramdump) state, which survives warm AP reboots.
 *   mdm-ctl off | on | reset | status
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#define ESOC_CODE        0xCC
#define ESOC_CMD_EXE     _IOW(ESOC_CODE, 1, unsigned int)
#define ESOC_GET_STATUS  _IOR(ESOC_CODE, 4, unsigned int)
#define ESOC_GET_ERR_FATAL _IOR(ESOC_CODE, 5, unsigned int)
#define ESOC_REG_CMD_ENG _IO(ESOC_CODE, 8)
enum { ESOC_PWR_ON = 1, ESOC_PWR_OFF, ESOC_FORCE_PWR_OFF, ESOC_RESET };

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mdm-ctl off|on|reset|status\n"); return 2; }
    int fd = open("/dev/esoc-0", O_RDONLY);
    if (fd < 0) { perror("open /dev/esoc-0"); return 1; }
    unsigned st = 0, ef = 0;
    ioctl(fd, ESOC_GET_STATUS, &st);
    ioctl(fd, ESOC_GET_ERR_FATAL, &ef);
    printf("status=%u err_fatal=%u\n", st, ef);
    if (!strcmp(argv[1], "status")) return 0;
    if (ioctl(fd, ESOC_REG_CMD_ENG) < 0) { perror("ESOC_REG_CMD_ENG"); return 1; }
    unsigned cmd = !strcmp(argv[1], "off") ? ESOC_FORCE_PWR_OFF :
                   !strcmp(argv[1], "on") ? ESOC_PWR_ON : ESOC_RESET;
    int r = ioctl(fd, ESOC_CMD_EXE, &cmd);
    printf("cmd %u -> %d (%s)\n", cmd, r, r < 0 ? strerror(errno) : "ok");
    ioctl(fd, ESOC_GET_STATUS, &st);
    printf("status=%u\n", st);
    return r < 0;
}
