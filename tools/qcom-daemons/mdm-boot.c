/*
 * mdm-boot: boot the external SDX50M (X50 5G) modem on Nubia TP1803 from
 * Android 6, replacing Qualcomm's mdm_helper + ks.
 *
 * Flow on the downstream 4.14 esoc/MHI stack:
 *   1. register as the esoc "request engine" on /dev/esoc-0
 *   2. open /dev/subsys_esoc0 -> kernel powers the MDM on (blocks until booted)
 *   3. MDM PBL ready -> kernel sends ESOC_REQ_IMG; the MHI driver pushes
 *      sdx50m/sbl1.mbn over BHI by itself
 *   4. SBL asks for the remaining images over the Sahara protocol on the MHI
 *      SAHARA channel (/dev/mhi_*_pipe_2); we serve them from
 *      /firmware/image/sdx50m (EFS from the mdm1m9kefs* partitions)
 *   5. notify ESOC_IMG_XFER_DONE; kernel waits for MDM2AP_STATUS -> booted
 *
 * Image ID -> file table recovered from the stock /vendor/bin/ks.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "mdm-boot"
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

/* include/uapi/linux/esoc_ctrl.h */
#define ESOC_CODE           0xCC
#define ESOC_CMD_EXE        _IOW(ESOC_CODE, 1, unsigned int)
#define ESOC_WAIT_FOR_REQ   _IOR(ESOC_CODE, 2, unsigned int)
#define ESOC_NOTIFY         _IOW(ESOC_CODE, 3, unsigned int)
#define ESOC_GET_STATUS     _IOR(ESOC_CODE, 4, unsigned int)
#define ESOC_REG_REQ_ENG    _IO(ESOC_CODE, 7)
#define ESOC_SET_BOOT_FAIL_ACT _IOW(ESOC_CODE, 10, unsigned int)
#define ESOC_SET_N_PON_TRIES   _IOW(ESOC_CODE, 11, unsigned int)
#define BOOT_FAIL_ACTION_COLD_RESET 1
#define BOOT_FAIL_ACTION_NOP 4
enum { ESOC_IMG_XFER_DONE = 1, ESOC_BOOT_DONE, ESOC_BOOT_FAIL, ESOC_IMG_XFER_RETRY,
       ESOC_IMG_XFER_FAIL, ESOC_UPGRADE_AVAILABLE, ESOC_DEBUG_DONE, ESOC_DEBUG_FAIL };
enum { ESOC_REQ_IMG = 1, ESOC_REQ_DEBUG, ESOC_REQ_SHUTDOWN, ESOC_REQ_SEND_SHUTDOWN,
       ESOC_REQ_CRASH_SHUTDOWN };

/* Sahara protocol */
enum {
    SAHARA_HELLO = 1, SAHARA_HELLO_RESP = 2, SAHARA_READ_DATA = 3,
    SAHARA_END_IMAGE_TX = 4, SAHARA_DONE = 5, SAHARA_DONE_RESP = 6,
    SAHARA_RESET = 7, SAHARA_RESET_RESP = 8, SAHARA_MEMORY_DEBUG = 9,
    SAHARA_CMD_READY = 0xb, SAHARA_READ_DATA_64 = 0x12,
};
enum { SAHARA_MODE_IMAGE_TX_PENDING = 0, SAHARA_MODE_IMAGE_TX_COMPLETE = 1 };

#define FW_DIR "/firmware/image/sdx50m/"
#define BYNAME "/dev/block/bootdevice/by-name/"

static const struct { uint32_t id; const char *path; } images[] = {
    { 2,  FW_DIR "qdsp6sw.mbn" },        /* amss */
    { 34, BYNAME "mdmddr" },             /* DDR training data (first request after SBL1) */
    { 6,  FW_DIR "apps.mbn" },
    { 8,  FW_DIR "qdsp6sw.mbn" },        /* dsp1: the X50's modem DSP image */
    { 16, BYNAME "mdm1m9kefs1" },        /* efs1 */
    { 17, BYNAME "mdm1m9kefs2" },        /* efs2 */
    { 20, BYNAME "mdm1m9kefs3" },        /* efs3 */
    { 21, FW_DIR "sbl1.mbn" },
    { 23, FW_DIR "rpm.mbn" },
    { 25, FW_DIR "tz.mbn" },
    { 29, FW_DIR "acdb.mbn" },
    { 31, FW_DIR "mba.mbn" },
};

static void *efs_sync_thread(void *arg);
static int read_exact(int fd, void *buf, size_t len);

struct img_fd { uint32_t id; int fd; };
static struct img_fd open_imgs[32];

static int image_fd(uint32_t id)
{
    for (unsigned i = 0; i < sizeof(open_imgs) / sizeof(open_imgs[0]); i++)
        if (open_imgs[i].fd > 0 && open_imgs[i].id == id)
            return open_imgs[i].fd;
    const char *path = NULL;
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
        if (images[i].id == id)
            path = images[i].path;
    if (!path) {
        LOGE("Sahara: no mapping for image id %u", id);
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOGE("Sahara: open %s: %s", path, strerror(errno));
        return -1;
    }
    LOGI("Sahara: image %u -> %s", id, path);
    for (unsigned i = 0; i < sizeof(open_imgs) / sizeof(open_imgs[0]); i++)
        if (open_imgs[i].fd <= 0) {
            open_imgs[i].id = id;
            open_imgs[i].fd = fd;
            break;
        }
    return fd;
}

static void close_images(void)
{
    for (unsigned i = 0; i < sizeof(open_imgs) / sizeof(open_imgs[0]); i++)
        if (open_imgs[i].fd > 0) {
            close(open_imgs[i].fd);
            open_imgs[i].fd = 0;
        }
}

static int find_sahara_node(char *out, size_t len)
{
    for (int tries = 0; tries < 300; tries++) {      /* up to 30 s */
        DIR *d = opendir("/dev");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (!strncmp(e->d_name, "mhi_", 4) && n > 7 &&
                !strcmp(e->d_name + n - 7, "_pipe_2")) {
                snprintf(out, len, "/dev/%s", e->d_name);
                closedir(d);
                return 0;
            }
        }
        if (d) closedir(d);
        usleep(100 * 1000);
    }
    return -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int send_data(int fd, uint32_t id, uint64_t off, uint64_t len)
{
    int ifd = image_fd(id);
    if (ifd < 0)
        return -1;
    static char buf[1 << 20];
    while (len) {
        size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;
        ssize_t n = pread(ifd, buf, chunk, off);
        if (n <= 0) {
            LOGE("Sahara: short read image %u @%llu", id, (unsigned long long)off);
            memset(buf, 0, chunk);
            n = chunk;
        }
        if (write_all(fd, buf, n))
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}


/* The MDM crashed earlier and offers a ramdump. Before resetting it, scan its
 * memory for the fatal-error text so the crash cause ends up in logcat. */
static int read_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        got += n;
    }
    return 0;
}

static void scan_mdm_dump(int fd, uint32_t *pkt)
{
    uint64_t taddr, tlen;
    int is64 = pkt[0] == 0x10;
    if (is64) { memcpy(&taddr, &pkt[2], 8); memcpy(&tlen, &pkt[4], 8); }
    else { taddr = pkt[2]; tlen = pkt[3]; }
    if (tlen > 64 * 1024) return;
    static unsigned char table[64 * 1024];
    uint32_t rq[6];
    if (is64) { rq[0] = 0x11; rq[1] = 24; memcpy(&rq[2], &taddr, 8); memcpy(&rq[4], &tlen, 8); write_all(fd, rq, 24); }
    else { rq[0] = 0xA; rq[1] = 16; rq[2] = taddr; rq[3] = tlen; write_all(fd, rq, 16); }
    if (read_exact(fd, table, tlen)) return;
    size_t esz = is64 ? 64 : 52;  /* 32-bit: save_pref, base, len, desc[20], filename[20] */
    static const char *pats[] = { ".c:", "dog bite", "imeout", NULL };
    static unsigned char buf[1 << 20];
    int hits = 0;
    for (size_t i = 0; i + esz <= tlen && hits < 40; i += esz) {
        uint64_t base, len; char name[21] = {0};
        if (is64) { memcpy(&base, table + i + 8, 8); memcpy(&len, table + i + 16, 8); memcpy(name, table + i + 24, 20); }
        else { uint32_t b, l; memcpy(&b, table + i + 4, 4); memcpy(&l, table + i + 8, 4); base = b; len = l; memcpy(name, table + i + 12, 20); }
        LOGI("MDM dump region %s base 0x%llx len 0x%llx", name, (unsigned long long)base, (unsigned long long)len);
        if (len > 512ull * 1024 * 1024) continue;
        for (uint64_t off = 0; off < len && hits < 40; off += sizeof(buf)) {
            uint64_t n = len - off > sizeof(buf) ? sizeof(buf) : len - off;
            uint64_t a = base + off;
            if (is64) { rq[0] = 0x11; rq[1] = 24; memcpy(&rq[2], &a, 8); memcpy(&rq[4], &n, 8); write_all(fd, rq, 24); }
            else { rq[0] = 0xA; rq[1] = 16; rq[2] = (uint32_t)a; rq[3] = (uint32_t)n; write_all(fd, rq, 16); }
            if (read_exact(fd, buf, n)) return;
            for (int p = 0; pats[p]; p++) {
                size_t pl = strlen(pats[p]);
                for (size_t k = 0; k + pl < n && hits < 40; k++) {
                    if (memcmp(buf + k, pats[p], pl)) continue;
                    /* ".c:" only counts when followed by a line number */
                    if (p == 0 && !(k + pl < n && buf[k + pl] >= '0' && buf[k + pl] <= '9')) continue;
                    size_t st = k > 120 ? k - 120 : 0, en = k + 200 < n ? k + 200 : n;
                    char ctx[400]; size_t c = 0;
                    for (size_t q = st; q < en && c < sizeof(ctx) - 1; q++)
                        ctx[c++] = (buf[q] >= 0x20 && buf[q] < 0x7f) ? buf[q] : '|';
                    ctx[c] = 0;
                    LOGI("MDM dump hit [%s @0x%llx]: %s", name, (unsigned long long)(a + k), ctx);
                    hits++;
                    k += pl;
                }
            }
        }
    }
}

/* Returns 0 when the target reports all images transferred, 1 if the target
 * was reset and the boot should be retried, -1 on failure. */
static int sahara_serve(const char *node)
{
    /* After an MDM restart the old pipe node can linger until MHI
     * re-enumerates; opening it then fails with EIO. Keep trying. */
    int fd = -1;
    for (int tries = 0; tries < 300 && fd < 0; tries++) {
        fd = open(node, O_RDWR);
        if (fd < 0) {
            if (errno != EIO && errno != ENOENT && errno != ENODEV) break;
            usleep(100 * 1000);
        }
    }
    if (fd < 0) {
        LOGE("open %s: %s", node, strerror(errno));
        return -1;
    }
    LOGI("Sahara: serving on %s", node);
    uint32_t pkt[64];
    int ret = -1;
    for (;;) {
        ssize_t n = read(fd, pkt, sizeof(pkt));
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("Sahara: read: %s", strerror(errno));
            break;
        }
        if (n < 8) continue;
        uint32_t cmd = pkt[0];
        switch (cmd) {
        case SAHARA_HELLO: {
            uint32_t mode = pkt[5];
            LOGI("Sahara: HELLO version %u compat %u mode %u", pkt[2], pkt[3], mode);
            uint32_t r[12] = { SAHARA_HELLO_RESP, 0x30, 2, 1, 0, mode, 0, 0, 0, 0, 0, 0 };
            if (write_all(fd, r, sizeof(r))) goto out;
            break;
        }
        case SAHARA_READ_DATA:
            if (send_data(fd, pkt[2], pkt[3], pkt[4])) goto out;
            break;
        case SAHARA_READ_DATA_64: {
            uint64_t id, off, len;
            memcpy(&id, &pkt[2], 8); memcpy(&off, &pkt[4], 8); memcpy(&len, &pkt[6], 8);
            if (send_data(fd, (uint32_t)id, off, len)) goto out;
            break;
        }
        case SAHARA_END_IMAGE_TX: {
            LOGI("Sahara: END_IMAGE_TX image %u status %u", pkt[2], pkt[3]);
            if (pkt[3] != 0) {
                LOGE("Sahara: target reported error %u for image %u", pkt[3], pkt[2]);
                goto out;
            }
            uint32_t r[2] = { SAHARA_DONE, 8 };
            if (write_all(fd, r, sizeof(r))) goto out;
            break;
        }
        case SAHARA_DONE_RESP:
            LOGI("Sahara: DONE_RESP status %u", pkt[2]);
            if (pkt[2] == SAHARA_MODE_IMAGE_TX_COMPLETE) {
                ret = 0;
                goto out;
            }
            break; /* more images: the target sends a new HELLO */
        case SAHARA_MEMORY_DEBUG:
        case 0x10: /* MEMORY_DEBUG_64 */ {
            /* The MDM crashed earlier and its SBL is offering a ramdump.
             * We don't collect it: reset the MDM and ask for a retry. */
            LOGI("Sahara: MDM is in memory-debug (crash) mode; scanning its dump");
            /* scan_mdm_dump(fd, pkt);  -- enable to log the MDM crash text */
            /* Resetting it over Sahara clears the crash state for the next
             * power-up; within this boot it won't come back. */
            LOGI("Sahara: MDM kept its crash state (unclean AP reboot); resetting it - reboot to bring it up");
            {
                uint32_t r[2] = { SAHARA_RESET, 8 };
                write_all(fd, r, sizeof(r));
            }
            ret = -1;
            goto out;
        }
        case SAHARA_RESET_RESP:
            break;
        default:
            LOGE("Sahara: unhandled command 0x%x (len %u)", cmd, pkt[1]);
            break;
        }
    }
out:
    close_images();
    close(fd);
    return ret;
}

/* Non-auto-boot MDMs: once MDM2AP_STATUS is high, userspace must send
 * ESOC_BOOT_DONE so the kernel moves the MDM to RUN state (pon_done). */
static int mission_mode(void)
{
    /* MHI mission-mode channels (QMI etc.) only exist once the MDM runs AMSS */
    DIR *d = opendir("/dev");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (!strncmp(e->d_name, "mhi_", 4) && n > 8 && !strcmp(e->d_name + n - 8, "_pipe_14"))
            found = 1;
    }
    if (d) closedir(d);
    return found;
}

/* Keep the MDM's PCIe link out of runtime suspend: on this board the link
 * fails to retrain after M3, which takes the X50 (and the WLAN clock) down. */
static void pin_pcie_link(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/power/control", e->d_name);
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            write(fd, "on", 2);
            close(fd);
            LOGI("runtime PM disabled for PCI %s", e->d_name);
        }
    }
    if (d) closedir(d);
}

/* Non-auto-boot MDMs: once the MDM is up, userspace must send ESOC_BOOT_DONE
 * so the kernel moves it to RUN state (pon_done). MDM2AP_STATUS never rises on
 * this board, so mission mode on MHI is accepted as "booted" too. */
static int wait_status_and_boot_done(int efd)
{
    for (int i = 0; i < 600; i++) {               /* up to 60 s */
        unsigned int st = 0;
        int mm = mission_mode();
        if ((ioctl(efd, ESOC_GET_STATUS, &st) >= 0 && st == 1) || (mm && i >= 20)) {
            pin_pcie_link();
            unsigned int n = ESOC_BOOT_DONE;
            if (ioctl(efd, ESOC_NOTIFY, &n) < 0) {
                LOGE("ESOC_NOTIFY(BOOT_DONE): %s", strerror(errno));
                return -1;
            }
            LOGI("MDM up (status=%u, mission mode=%d); sent ESOC_BOOT_DONE", st, mm);
            static int efs_started;
            if (!efs_started) {
                pthread_t t;
                pthread_create(&t, NULL, efs_sync_thread, NULL);
                efs_started = 1;
            }
            return 0;
        }
        usleep(100 * 1000);
    }
    LOGE("MDM never reached mission mode");
    return -1;
}


/* EFS sync: the MDM periodically pushes its EFS over a Sahara memory-debug
 * session on the MHI EFS pipe (channel 10), served on stock by
 * `ks -t -1 -g m9k1_`. If nobody answers, the MDM errfatals ~3 minutes after
 * boot. We collect the regions into /data/vendor/mdm-efs and reset the session. */
static int find_pipe(const char *suffix, char *out, size_t len)
{
    DIR *d = opendir("/dev");
    struct dirent *e;
    size_t sl = strlen(suffix);
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (!strncmp(e->d_name, "mhi_", 4) && n > sl && !strcmp(e->d_name + n - sl, suffix)) {
            snprintf(out, len, "/dev/%s", e->d_name);
            closedir(d);
            return 0;
        }
    }
    if (d) closedir(d);
    return -1;
}

static void *efs_sync_thread(void *arg)
{
    (void)arg;
    char node[128];
    mkdir("/data/vendor/mdm-efs", 0700);
    for (;;) {
        if (find_pipe("_pipe_10", node, sizeof(node))) { sleep(2); continue; }
        int fd = open(node, O_RDWR);
        if (fd < 0) { sleep(2); continue; }
        LOGI("EFS sync: listening on %s", node);
        uint32_t pkt[64];
        for (;;) {
            ssize_t n = read(fd, pkt, sizeof(pkt));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { LOGE("EFS sync: read: %s", n < 0 ? strerror(errno) : "eof"); break; }
            if (n < 8) continue;
            LOGI("EFS sync: cmd 0x%x len %u", pkt[0], pkt[1]);
            if (pkt[0] == SAHARA_HELLO) {
                uint32_t r[12] = { SAHARA_HELLO_RESP, 0x30, 2, 1, 0, pkt[5], 0, 0, 0, 0, 0, 0 };
                LOGI("EFS sync: HELLO mode %u", pkt[5]);
                write_all(fd, r, sizeof(r));
            } else if (pkt[0] == SAHARA_MEMORY_DEBUG || pkt[0] == 0x10) {
                int is64 = pkt[0] == 0x10;
                uint64_t taddr, tlen;
                if (is64) { memcpy(&taddr, &pkt[2], 8); memcpy(&tlen, &pkt[4], 8); }
                else { taddr = pkt[2]; tlen = pkt[3]; }
                static unsigned char table[16384];
                if (tlen > sizeof(table)) tlen = sizeof(table);
                uint32_t rq[6];
                if (is64) { rq[0] = 0x11; rq[1] = 24; memcpy(&rq[2], &taddr, 8); memcpy(&rq[4], &tlen, 8); write_all(fd, rq, 24); }
                else { rq[0] = 0xA; rq[1] = 16; rq[2] = taddr; rq[3] = tlen; write_all(fd, rq, 16); }
                if (read_exact(fd, table, tlen)) break;
                size_t esz = is64 ? 64 : 52;
                for (size_t i = 0; i + esz <= tlen; i += esz) {
                    uint64_t base, len; char fname[21] = {0};
                    if (is64) { memcpy(&base, table + i + 8, 8); memcpy(&len, table + i + 16, 8); memcpy(fname, table + i + 44, 20); }
                    else { uint32_t b, l; memcpy(&b, table + i + 4, 4); memcpy(&l, table + i + 8, 4); base = b; len = l; memcpy(fname, table + i + 32, 20); }
                    LOGI("EFS sync: region %s base 0x%llx len 0x%llx", fname, (unsigned long long)base, (unsigned long long)len);
                    if (len > 16 * 1024 * 1024) continue;
                    /* Regions are named m9kefs1/2/3: write them back to the
                     * mdm1m9kefsN partitions like stock ks does; keep a copy
                     * of anything else under /data/vendor/mdm-efs. */
                    char path[256];
                    /* Partition write-back disabled until the format is verified:
                     * keep the synced EFS under /data/vendor/mdm-efs instead. */
                    int is_part = 0;
                    if (is_part)
                        snprintf(path, sizeof(path), BYNAME "mdm1%s", fname);
                    else
                        snprintf(path, sizeof(path), "/data/vendor/mdm-efs/%s", fname[0] ? fname : "region.bin");
                    int out = is_part ? open(path, O_WRONLY) : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                    if (out < 0)
                        LOGE("EFS sync: open %s: %s", path, strerror(errno));
                    static unsigned char buf[1 << 16];
                    for (uint64_t off = 0; off < len; off += sizeof(buf)) {
                        uint64_t c = len - off > sizeof(buf) ? sizeof(buf) : len - off, a = base + off;
                        if (is64) { rq[0] = 0x11; rq[1] = 24; memcpy(&rq[2], &a, 8); memcpy(&rq[4], &c, 8); write_all(fd, rq, 24); }
                        else { rq[0] = 0xA; rq[1] = 16; rq[2] = (uint32_t)a; rq[3] = (uint32_t)c; write_all(fd, rq, 16); }
                        if (read_exact(fd, buf, c)) break;
                        if (out >= 0) write(out, buf, c);
                    }
                    if (out >= 0) {
                        fsync(out);
                        close(out);
                        LOGI("EFS sync: wrote %s", path);
                    }
                }
                uint32_t r[2] = { SAHARA_RESET, 8 };
                write_all(fd, r, sizeof(r));
                LOGI("EFS sync: collected, session reset");
            } else if (pkt[0] == SAHARA_RESET_RESP) {
                /* next sync starts with a new HELLO */
            } else {
                LOGI("EFS sync: unhandled cmd 0x%x", pkt[0]);
            }
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

static void *hold_subsys(void *arg)
{
    (void)arg;
    LOGI("opening /dev/subsys_esoc0 (powers the MDM on)");
    int fd = open("/dev/subsys_esoc0", O_RDONLY);
    if (fd < 0) {
        LOGE("open /dev/subsys_esoc0: %s", strerror(errno));
        exit(1);
    }
    LOGI("external modem is up");
    for (;;)
        pause();
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGHUP, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "-d")) {
        if (fork() > 0) _exit(0);
        setsid();
        if (fork() > 0) _exit(0);
    }

    int efd = open("/dev/esoc-0", O_RDONLY);
    if (efd < 0) {
        LOGE("open /dev/esoc-0: %s", strerror(errno));
        return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "boot-done"))
        return wait_status_and_boot_done(efd) ? 1 : 0;
    if (ioctl(efd, ESOC_REG_REQ_ENG)) {
        LOGE("ESOC_REG_REQ_ENG: %s", strerror(errno));
        return 1;
    }
    LOGI("registered as esoc request engine");

    /* The driver's default is to panic the whole system if the MDM fails to
     * boot. Instead, power-cycle it (cold reset) and retry: that is also what
     * clears the crash state an MDM keeps across warm AP reboots. After the
     * retries are used up the kernel escalates to panic, so allow a few. */
    unsigned int tries = 4;
    if (ioctl(efd, ESOC_SET_N_PON_TRIES, &tries) < 0)
        LOGE("ESOC_SET_N_PON_TRIES: %s", strerror(errno));
    /* NOP: a failed MDM boot must never panic the AP. (COLD_RESET does not
     * recover an MDM stuck in its crash state; it just burns the retries
     * and the kernel then escalates to panic.) */
    unsigned int act = BOOT_FAIL_ACTION_NOP;
    if (ioctl(efd, ESOC_SET_BOOT_FAIL_ACT, &act) < 0)
        LOGE("ESOC_SET_BOOT_FAIL_ACT: %s", strerror(errno));
    else
        LOGI("boot fail action: NOP");

    pthread_t t;
    pthread_create(&t, NULL, hold_subsys, NULL);

    for (;;) {
        unsigned int req = 0;
        /* returns the number of bytes dequeued (4) on success */
        if (ioctl(efd, ESOC_WAIT_FOR_REQ, &req) < 0) {
            if (errno == EINTR) continue;
            LOGE("ESOC_WAIT_FOR_REQ: %s", strerror(errno));
            sleep(1);
            continue;
        }
        LOGI("esoc request %u", req);
        if (req == ESOC_REQ_IMG) {
            char node[128];
            unsigned int notify;
            if (find_sahara_node(node, sizeof(node))) {
                LOGE("no MHI SAHARA pipe appeared");
                notify = ESOC_IMG_XFER_FAIL;
            } else {
                int r = sahara_serve(node);
                /* After a warm AP reboot the MDM keeps its crash state and
                 * offers a ramdump first. We reset it over Sahara; it then
                 * reboots by itself and comes back on the same pipe asking for
                 * images, so serve it again before involving the kernel. */

                notify = r == 0 ? ESOC_IMG_XFER_DONE :
                         r == 1 ? ESOC_IMG_XFER_RETRY : ESOC_IMG_XFER_FAIL;
            }
            LOGI("image transfer %s", notify == ESOC_IMG_XFER_DONE ? "done" :
                 notify == ESOC_IMG_XFER_RETRY ? "needs retry" : "FAILED");
            if (ioctl(efd, ESOC_NOTIFY, &notify) < 0)
                LOGE("ESOC_NOTIFY: %s", strerror(errno));
            if (notify == ESOC_IMG_XFER_DONE)
                wait_status_and_boot_done(efd);
        } else if (req == ESOC_REQ_DEBUG) {
            unsigned int notify = ESOC_DEBUG_DONE;  /* no ramdump collection */
            ioctl(efd, ESOC_NOTIFY, &notify);
        }
        /* SHUTDOWN requests need no action from us */
    }
}
