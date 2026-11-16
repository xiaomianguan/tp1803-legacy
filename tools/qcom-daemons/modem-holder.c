/*
 * Keep a Qualcomm PIL subsystem (default: the modem) booted.
 *
 * On downstream kernels a subsystem is powered up when its /dev/subsys_<name>
 * node is opened and shut down when the last reference is closed. Qualcomm's
 * per_mgr does this on stock; this is the minimal equivalent for Android 6.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "modem-holder"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "modem";
    signal(SIGHUP, SIG_IGN);
    if (argc > 2 && !strcmp(argv[2], "-d")) {
        /* detach from the launching shell/session (for manual testing) */
        if (fork() > 0) _exit(0);
        setsid();
        if (fork() > 0) _exit(0);
    }
    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/subsys_%s", name);

    int fd;
    for (int tries = 0;; tries++) {
        fd = open(dev, O_RDONLY);   /* blocks until the subsystem has booted */
        if (fd >= 0)
            break;
        LOG("open %s failed: %s (retrying)", dev, strerror(errno));
        sleep(tries < 10 ? 1 : 5);
    }
    LOG("%s is up; holding it", dev);
    for (;;)
        pause();
}
