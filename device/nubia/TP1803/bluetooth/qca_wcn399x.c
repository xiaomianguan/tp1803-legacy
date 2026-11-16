/*
 * WCN399x bring-up over H4 UART. See qca_wcn399x.h.
 */
#include "qca_wcn399x.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef ANDROID_LOG
#include <android/log.h>
static void default_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "bt_tp1803", fmt, ap);
    va_end(ap);
}
#else
static void default_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#endif
void (*qca_log)(const char *fmt, ...) = default_log;
#define LOG(...) qca_log(__VA_ARGS__)

/* downstream drivers/bluetooth/bluetooth-power.c */
#define BT_CMD_PWR_CTRL     0xbfad
/* msm_geni_serial: vote UART clocks on/off */
#define TIOCPMGET_          0x544D
#define TIOCPMPUT_          0x544E

#define H4_CMD   0x01
#define H4_EVT   0x04

#define EVT_CMD_COMPLETE 0x0e
#define EVT_CMD_STATUS   0x0f
#define EVT_VENDOR       0xff

#define EDL_PATCH_CMD_OPCODE     0xFC00
#define EDL_WRITE_BD_ADDR_OPCODE 0xFC14
#define QCA_SET_BAUDRATE_OPCODE  0xFC48
#define EDL_PATCH_VER_REQ_CMD    0x19
#define EDL_PATCH_TLV_REQ_CMD    0x1E
#define EDL_APP_VER_RES_EVT      0x02
#define EDL_TVL_DNLD_RES_EVT     0x04
#define MAX_TLV_SEGMENT          243
#define EDL_TAG_ID_HCI           17
#define EDL_TAG_ID_DEEP_SLEEP    27

enum { TLV_TYPE_PATCH = 1, TLV_TYPE_NVM = 2 };
/* download_mode values in the rampatch header */
enum { SKIP_EVT_NONE = 0, SKIP_EVT_VSE = 1, SKIP_EVT_CC = 2, SKIP_EVT_VSE_CC = 3 };

static int pwr_fd = -1;
static int clock_votes;  /* TIOCPMGET votes we hold on the UART */
static int keep_ibs;    /* leave IBS/deep sleep enabled in the NVM */

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void msleep(int ms) { usleep(ms * 1000); }

/* ---- UART ---- */

static speed_t speed_const(unsigned int baud)
{
    switch (baud) {
    case 2400: return B2400;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 3500000: return B3500000;
    case 4000000: return B4000000;
    default: return 0;
    }
}

static int qca_baud_code(unsigned int baud)
{
    switch (baud) {
    case 115200: return 0;
    case 230400: return 5;
    case 460800: return 7;
    case 921600: return 10;
    case 1000000: return 11;
    case 2000000: return 13;
    case 3000000: return 14;
    case 4000000: return 15;
    case 3500000: return 18;
    default: return -1;
    }
}

static int uart_set(int fd, unsigned int baud, int flow)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -errno;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSTOPB;
    if (flow)
        t.c_cflag |= CRTSCTS;
    else
        t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    speed_t s = speed_const(baud);
    cfsetospeed(&t, s);
    cfsetispeed(&t, s);
    if (tcsetattr(fd, TCSANOW, &t) < 0)
        return -errno;
    /* Without flow control, also hold RTS deasserted (as hci_qca does). */
    int rts = TIOCM_RTS;
    ioctl(fd, flow ? TIOCMBIS : TIOCMBIC, &rts);
    return 0;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            return -errno;
        }
        p += w;
        n -= w;
    }
    return 0;
}

static int read_exact(int fd, uint8_t *p, size_t n, long long deadline)
{
    while (n) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return -ETIMEDOUT;
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, left);
        if (r <= 0)
            continue;
        ssize_t got = read(fd, p, n);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -errno;
        }
        p += got;
        n -= got;
    }
    return 0;
}

/* ---- HCI ---- */

static int hci_send(int fd, uint16_t opcode, const uint8_t *param, uint8_t plen)
{
    uint8_t buf[4 + 255];
    buf[0] = H4_CMD;
    buf[1] = opcode & 0xff;
    buf[2] = opcode >> 8;
    buf[3] = plen;
    if (plen)
        memcpy(buf + 4, param, plen);
    return write_all(fd, buf, 4 + plen);
}

/* Read one HCI event (skipping any non-event H4 packets). Returns the
 * parameter length, fills *code. */
static int hci_read_event(int fd, uint8_t *code, uint8_t *param, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        uint8_t type;
        int r = read_exact(fd, &type, 1, deadline);
        if (r)
            return r;
        if (type == H4_EVT) {
            uint8_t hdr[2];
            if ((r = read_exact(fd, hdr, 2, deadline)))
                return r;
            if ((r = read_exact(fd, param, hdr[1], deadline)))
                return r;
            *code = hdr[0];
            return hdr[1];
        }
        if (type == 0x02) {          /* ACL: skip */
            uint8_t hdr[4], junk[1024];
            if ((r = read_exact(fd, hdr, 4, deadline)))
                return r;
            int len = hdr[2] | hdr[3] << 8;
            if (len > (int)sizeof(junk) || (r = read_exact(fd, junk, len, deadline)))
                return r ? r : -EPROTO;
            continue;
        }
        /* Once the NVM is applied with IBS on, the controller uses in-band
         * sleep: acknowledge its wake requests, ignore its sleep notices. */
        if (type == 0xFD) {
            uint8_t ack = 0xFC;
            write_all(fd, &ack, 1);
            continue;
        }
        if (type == 0xFE || type == 0xFC)
            continue;
        LOG("unexpected H4 byte 0x%02x", type);
    }
}

/* Wait for a vendor event whose EDL response type is rtype. Command
 * complete/status events for the vendor opcode are skipped. */
static int wait_vendor(int fd, uint8_t rtype, uint8_t *out, int outlen, int timeout_ms)
{
    uint8_t p[256], code;
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return -ETIMEDOUT;
        int n = hci_read_event(fd, &code, p, left);
        if (n < 0)
            return n;
        if (code == EVT_VENDOR && n >= 2 && p[1] == rtype) {
            if (getenv("QCA_DUMP")) {
                char hex[3 * 64 + 1] = "";
                int i;
                for (i = 0; i < n && i < 64; i++)
                    sprintf(hex + 3 * i, "%02x ", p[i]);
                LOG("vendor event %02x: %s", rtype, hex);
            }
            if (p[0] != 0)
                LOG("vendor event 0x%02x status 0x%02x", rtype, p[0]);
            if (out)
                memcpy(out, p + 2, n - 2 < outlen ? n - 2 : outlen);
            return p[0] ? -EIO : n - 2;
        }
        if (code == EVT_CMD_COMPLETE || code == EVT_CMD_STATUS)
            continue;
        LOG("ignoring event 0x%02x (len %d)", code, n);
    }
}

/* Standard command, wait for its Command Complete; returns status. */
static int hci_cmd_sync(int fd, uint16_t opcode, const uint8_t *param, uint8_t plen,
                        uint8_t *ret, int retlen)
{
    int r = hci_send(fd, opcode, param, plen);
    if (r)
        return r;
    uint8_t p[256], code;
    long long deadline = now_ms() + 2000;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            return -ETIMEDOUT;
        int n = hci_read_event(fd, &code, p, left);
        if (n < 0)
            return n;
        if (code == EVT_CMD_COMPLETE && n >= 3 && (p[1] | p[2] << 8) == opcode) {
            if (ret)
                memcpy(ret, p + 3, n - 3 < retlen ? n - 3 : retlen);
            return n > 3 ? p[3] : 0;
        }
        if (code == EVT_CMD_STATUS && n >= 4 && (p[2] | p[3] << 8) == opcode && p[0])
            return p[0];
    }
}

/* ---- power ---- */

static int power(int on)
{
    if (pwr_fd < 0)
        pwr_fd = open(QCA_BT_POWER_DEV, O_RDWR | O_CLOEXEC);
    if (pwr_fd < 0)
        return -errno;
    if (ioctl(pwr_fd, BT_CMD_PWR_CTRL, (unsigned long)on) < 0)
        return -errno;
    return 0;
}

static int send_pulse(int fd, unsigned int baud, uint8_t pulse)
{
    int r = uart_set(fd, baud, 0);
    if (r)
        return r;
    tcflush(fd, TCIOFLUSH);
    if ((r = write_all(fd, &pulse, 1)))
        return r;
    tcdrain(fd);
    return 0;
}

/* ---- firmware ---- */

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* Patch NVM tags in one type-2 section: IBS and deep sleep off, UART
 * baud = oper speed. */
static void fixup_nvm_tags(uint8_t *data, size_t length, int baud_code)
{
    size_t idx = 0;
    while (idx + 12 <= length) {
        uint8_t *tag = data + idx;
        uint16_t id = tag[0] | tag[1] << 8;
        uint16_t tlen = tag[2] | tag[3] << 8;
        uint8_t *val = tag + 12;
        if (idx + 12 + tlen > length)
            break;
        if (id == EDL_TAG_ID_HCI && tlen >= 3) {
            LOG("NVM HCI tag: flags 0x%02x baud 0x%02x -> IBS %s, baud %d", val[0], val[2],
                keep_ibs ? "on" : "off", baud_code);
            if (keep_ibs)
                val[0] |= 0x80;
            else
                val[0] &= ~0x80;
            val[2] = baud_code;
        } else if (id == EDL_TAG_ID_DEEP_SLEEP && tlen >= 1) {
            LOG("NVM deep sleep tag 0x%02x -> %s", val[0], keep_ibs ? "on" : "off");
            if (keep_ibs)
                val[0] |= 0x01;
            else
                val[0] &= ~0x01;
        }
        idx += 12 + tlen;
    }
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* NVM files are either a plain type-2 TLV, or (newer, as shipped here) a
 * type-4 container holding type-2 (tags) and type-3 sections. */
static void fixup_nvm(uint8_t *fw, size_t len, int baud_code)
{
    if (len < 4)
        return;
    uint32_t tl = rd32(fw);
    size_t length = tl >> 8;
    if (length > len - 4)
        length = len - 4;
    if ((tl & 0xff) != 4) {
        fixup_nvm_tags(fw + 4, length, baud_code);
        return;
    }
    size_t off = 4;
    while (off + 4 <= 4 + length) {
        uint32_t itl = rd32(fw + off);
        size_t ilen = itl >> 8;
        if (off + 4 + ilen > len)
            break;
        if ((itl & 0xff) == 2)
            fixup_nvm_tags(fw + off + 4, ilen, baud_code);
        off += 4 + ilen;
    }
}

static int download(int fd, const char *path, int type, int baud_code)
{
    size_t len;
    uint8_t *fw = load_file(path, &len);
    if (!fw) {
        LOG("can't read %s: %s", path, strerror(errno));
        return -ENOENT;
    }
    int mode = SKIP_EVT_NONE;
    if (type == TLV_TYPE_PATCH && len > 4 + 11)
        mode = fw[4 + 10];              /* tlv_type_patch.download_mode */
    if (type == TLV_TYPE_NVM)
        fixup_nvm(fw, len, baud_code);
    LOG("downloading %s (%zu bytes, mode %d)", path, len, mode);

    int r = 0;
    size_t off = 0;
    while (off < len) {
        uint8_t cmd[2 + MAX_TLV_SEGMENT];
        size_t seg = len - off > MAX_TLV_SEGMENT ? MAX_TLV_SEGMENT : len - off;
        cmd[0] = EDL_PATCH_TLV_REQ_CMD;
        cmd[1] = seg;
        memcpy(cmd + 2, fw + off, seg);
        off += seg;
        int last = off >= len;
        if ((r = hci_send(fd, EDL_PATCH_CMD_OPCODE, cmd, seg + 2)))
            break;
        /* With VSE skipped only the last segment is acknowledged. */
        if (!last && (mode == SKIP_EVT_VSE || mode == SKIP_EVT_VSE_CC))
            continue;
        uint8_t res[4];
        r = wait_vendor(fd, EDL_TVL_DNLD_RES_EVT, res, sizeof(res), 3000);
        if (r < 0) {
            LOG("segment at %zu: no ack (%d)", off - seg, r);
            break;
        }
        r = 0;
    }
    free(fw);
    return r;
}

/* ---- public ---- */

int qca_wcn399x_open(unsigned int oper_baud, const uint8_t *bdaddr, int ibs)
{
    keep_ibs = ibs;
    int r, fd = -1;
    int baud_code = qca_baud_code(oper_baud);
    if (baud_code < 0 || !speed_const(oper_baud))
        return -EINVAL;

    if ((r = power(1))) {
        LOG("btpower on failed: %d", r);
        return r;
    }
    msleep(50);
    fd = open(QCA_BT_UART_DEV, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        r = -errno;
        LOG("open %s: %s", QCA_BT_UART_DEV, strerror(errno));
        goto fail_power;
    }
    if (ioctl(fd, TIOCPMGET_) == 0)     /* UART clocks on */
        clock_votes = 1;

    /* Enter boot mode: off pulse at 2400, on pulse at 115200. */
    send_pulse(fd, 2400, 0xC0);
    msleep(10);
    send_pulse(fd, 115200, 0xFC);
    msleep(100);
    uart_set(fd, 115200, 1);
    tcflush(fd, TCIOFLUSH);

    /* SoC version */
    uint8_t ver[12];
    uint8_t c = EDL_PATCH_VER_REQ_CMD;
    int tries;
    for (tries = 0; tries < 3; tries++) {
        if ((r = hci_send(fd, EDL_PATCH_CMD_OPCODE, &c, 1)))
            goto fail;
        r = wait_vendor(fd, EDL_APP_VER_RES_EVT, ver, sizeof(ver), 1000);
        if (r >= 12)
            break;
        LOG("version query attempt %d failed (%d)", tries + 1, r);
    }
    if (r < 12) {
        r = r < 0 ? r : -EPROTO;
        goto fail;
    }
    uint32_t product = ver[0] | ver[1] << 8 | ver[2] << 16 | (uint32_t)ver[3] << 24;
    uint16_t patch = ver[4] | ver[5] << 8;
    uint16_t rom = ver[6] | ver[7] << 8;
    uint32_t soc = ver[8] | ver[9] << 8 | ver[10] << 16 | (uint32_t)ver[11] << 24;
    uint32_t soc_ver = soc << 16 | rom;
    uint8_t rom_ver = ((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf);
    /* The upstream naming rule doesn't cover every revision; this one
     * (SoC 0x40010224, ROM 0x1001) is what Nubia ships crbtfw21/crnv21.bXX
     * for. Fall back to it whenever the derived rampatch doesn't exist. */
    {
        char probe[128];
        snprintf(probe, sizeof(probe), QCA_BT_FW_DIR "/crbtfw%02x.tlv", rom_ver);
        if (access(probe, R_OK) != 0) {
            LOG("no %s, using firmware 21", probe);
            rom_ver = 0x21;
        }
    }
    LOG("WCN399x: product 0x%08x soc 0x%08x rom 0x%04x patch 0x%04x -> fw %02x",
        product, soc, rom, patch, rom_ver);

    /* Operating speed: chip first, then host; the chip's ack arrives at
     * the new speed and is discarded. */
    uart_set(fd, 115200, 0);
    uint8_t b = baud_code;
    if ((r = hci_send(fd, QCA_SET_BAUDRATE_OPCODE, &b, 1)))
        goto fail;
    tcdrain(fd);
    msleep(10);
    uart_set(fd, oper_baud, 1);
    {
        uint8_t p[256], code;
        while (hci_read_event(fd, &code, p, 200) >= 0)
            ;   /* drain the baud-rate vendor event */
    }

    char path[128];
    snprintf(path, sizeof(path), QCA_BT_FW_DIR "/crbtfw%02x.tlv", rom_ver);
    if ((r = download(fd, path, TLV_TYPE_PATCH, baud_code)))
        goto fail;
    msleep(10);
    /* Board ID selects a board-specific NVM (crnvXX.bYY) when present. */
    {
        /* Reply is either a Command Complete for 0xFC00 or a vendor
         * event; both carry {cresp, 0x23, d0, d1, d2}, bid = d1 << 8 | d2. */
        uint8_t q = 0x23, p[256], code;
        unsigned bid = 0;
        int st = hci_send(fd, EDL_PATCH_CMD_OPCODE, &q, 1);
        long long deadline = now_ms() + 1000;
        while (st == 0) {
            int left = (int)(deadline - now_ms());
            int n = left > 0 ? hci_read_event(fd, &code, p, left) : -ETIMEDOUT;
            if (n < 0) {
                st = n;
                break;
            }
            const uint8_t *e = NULL;
            if (code == EVT_CMD_COMPLETE && n >= 8 && (p[1] | p[2] << 8) == EDL_PATCH_CMD_OPCODE)
                e = p + 3;
            else if (code == EVT_VENDOR && n >= 5 && p[1] == 0x23)
                e = p;
            if (e) {
                if (e[1] == 0x23)
                    bid = e[3] << 8 | e[4];
                break;
            }
        }
        LOG("board id: 0x%x (status %d)", bid, st);
        path[0] = 0;
        if (bid) {
            if (bid & 0xff00)
                snprintf(path, sizeof(path), QCA_BT_FW_DIR "/crnv%02x.b%x", rom_ver, bid);
            else
                snprintf(path, sizeof(path), QCA_BT_FW_DIR "/crnv%02x.b%02x", rom_ver, bid);
            if (access(path, R_OK) != 0) {
                LOG("no board NVM %s", path);
                path[0] = 0;
            }
        }
        if (!path[0])
            snprintf(path, sizeof(path), QCA_BT_FW_DIR "/crnv%02x.bin", rom_ver);
    }
    if ((r = download(fd, path, TLV_TYPE_NVM, baud_code)))
        goto fail;
    msleep(10);

    if ((r = hci_cmd_sync(fd, 0x0c03, NULL, 0, NULL, 0))) {   /* HCI_Reset */
        LOG("HCI reset failed: %d", r);
        if (r > 0)
            r = -EIO;
        goto fail;
    }
    if (bdaddr) {
        uint8_t a[6];
        int i;
        for (i = 0; i < 6; i++)
            a[i] = bdaddr[5 - i];       /* controller wants LSB first */
        if (hci_send(fd, EDL_WRITE_BD_ADDR_OPCODE, a, 6) == 0)
            wait_vendor(fd, 0x00, NULL, 0, 1000);
    }

    uint8_t lv[9];
    if (hci_cmd_sync(fd, 0x1001, NULL, 0, lv, sizeof(lv)) == 0)   /* Read_Local_Version */
        LOG("controller up: HCI %u rev 0x%04x, LMP %u subver 0x%04x", lv[1], lv[2] | lv[3] << 8,
            lv[4], lv[7] | lv[8] << 8);
    uint8_t ba[7];
    if (hci_cmd_sync(fd, 0x1009, NULL, 0, ba, sizeof(ba)) == 0)   /* Read_BD_ADDR */
        LOG("BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x", ba[6], ba[5], ba[4], ba[3], ba[2], ba[1]);
    return fd;

fail:
    LOG("WCN399x bring-up failed: %d", r);
    qca_wcn399x_close(fd);
    return r;
fail_power:
    power(0);
    return r;
}

void qca_wcn399x_close(int fd)
{
    if (fd >= 0) {
        while (clock_votes > 0 && ioctl(fd, TIOCPMPUT_) == 0)
            clock_votes--;
        clock_votes = 0;
        close(fd);
    }
    power(0);
}

int qca_uart_clock(int fd, int on)
{
    if (!on && clock_votes <= 0)
        return 0;
    if (ioctl(fd, on ? TIOCPMGET_ : TIOCPMPUT_) < 0)
        return -errno;
    clock_votes += on ? 1 : -1;
    return 0;
}
