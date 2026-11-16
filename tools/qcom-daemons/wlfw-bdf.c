/*
 * wlfw-bdf: download the WLAN board data file (BDF) to the WCN399x firmware
 * over QMI, the part of Qualcomm's cnss-daemon that the icnss WLAN firmware
 * on SM8150 waits for before it reports FW_READY.
 *
 * Talks to the WLFW QMI service (0x45) over QRTR, like the kernel's icnss
 * driver does, and sends QMI_WLFW_BDF_DOWNLOAD_REQ (0x25) in 6 KiB segments.
 * Message layout from the kernel's wlan_firmware_service_v01.c:
 *   0x01 valid(u8) 0x10 file_id(s32) 0x11 total_size(u32) 0x12 seg_id(u32)
 *   0x13 data(u16 len + bytes) 0x14 end(u8) 0x15 bdf_type(u8)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>

#include "libqrtr.h"

#define TAG "wlfw-bdf"
#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

#define WLFW_SERVICE_ID     0x45
#define WLFW_SERVICE_VERS   0x01
#define WLFW_SERVICE_INS    0
#define QMI_WLFW_BDF_DOWNLOAD 0x25
#define SEG_MAX 6144

static const char *const bdf_paths[] = {
    "/firmware/image/bdwlan.bin",
    "/vendor/firmware/bdwlan.bin",
    NULL,
};

static uint8_t *put_tlv(uint8_t *p, uint8_t type, const void *val, uint16_t len)
{
    *p++ = type;
    *p++ = len & 0xff;
    *p++ = len >> 8;
    memcpy(p, val, len);
    return p + len;
}

/* Returns 0 on success, QMI error code (>0) or -errno */
static int send_seg(int sock, uint32_t node, uint32_t port, uint16_t txn,
                    uint32_t total, uint32_t seg, const uint8_t *data,
                    uint16_t len, uint8_t end)
{
    static uint8_t msg[7 + 16 + SEG_MAX + 64];
    uint8_t *p = msg + 7;
    uint8_t u8;
    int32_t s32;
    uint32_t u32;

    u8 = 1;   p = put_tlv(p, 0x01, &u8, 1);           /* valid */
    s32 = 0;  p = put_tlv(p, 0x10, &s32, 4);          /* file_id */
    u32 = total; p = put_tlv(p, 0x11, &u32, 4);       /* total_size */
    u32 = seg;   p = put_tlv(p, 0x12, &u32, 4);       /* seg_id */
    *p++ = 0x13;                                       /* data: len16 + bytes */
    uint16_t tl = 2 + len;
    *p++ = tl & 0xff; *p++ = tl >> 8;
    *p++ = len & 0xff; *p++ = len >> 8;
    memcpy(p, data, len); p += len;
    u8 = end; p = put_tlv(p, 0x14, &u8, 1);           /* end */
    u8 = 0;   p = put_tlv(p, 0x15, &u8, 1);           /* bdf_type: BIN */

    uint16_t mlen = (uint16_t)(p - (msg + 7));
    msg[0] = QMI_REQUEST;
    msg[1] = txn & 0xff; msg[2] = txn >> 8;
    msg[3] = QMI_WLFW_BDF_DOWNLOAD & 0xff; msg[4] = QMI_WLFW_BDF_DOWNLOAD >> 8;
    msg[5] = mlen & 0xff; msg[6] = mlen >> 8;

    if (qrtr_sendto(sock, node, port, msg, 7 + mlen) < 0)
        return -errno;

    for (int tries = 0; tries < 50; tries++) {
        if (qrtr_poll(sock, 200) <= 0)
            continue;
        uint8_t buf[512];
        uint32_t rnode, rport;
        int n = qrtr_recvfrom(sock, buf, sizeof(buf), &rnode, &rport);
        if (n < 7 || rnode != node || rport != port || buf[0] != QMI_RESPONSE)
            continue;
        if ((buf[1] | buf[2] << 8) != txn)
            continue;
        /* result TLV 0x02: u16 result, u16 error */
        uint8_t *q = buf + 7, *e = buf + n;
        while (q + 3 <= e) {
            uint8_t t = q[0];
            uint16_t l = q[1] | q[2] << 8;
            if (t == 0x02 && l >= 4) {
                uint16_t res = q[3] | q[4] << 8, err = q[5] | q[6] << 8;
                return res ? (err ? err : 1) : 0;
            }
            q += 3 + l;
        }
        return 0;
    }
    return -ETIMEDOUT;
}

static int download_bdf(int sock, uint32_t node, uint32_t port)
{
    const char *path = NULL;
    for (int i = 0; bdf_paths[i]; i++)
        if (!access(bdf_paths[i], R_OK)) { path = bdf_paths[i]; break; }
    if (!path) {
        LOGE("no BDF file found");
        return -ENOENT;
    }
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) {
        LOGE("open %s: %s", path, strerror(errno));
        return -errno;
    }
    uint8_t *data = malloc(st.st_size);
    if (!data || read(fd, data, st.st_size) != st.st_size) {
        close(fd);
        free(data);
        return -EIO;
    }
    close(fd);

    LOGI("downloading BDF %s (%lld bytes) to WLFW %u:%u", path, (long long)st.st_size, node, port);
    static uint16_t txn = 1;
    uint32_t seg = 0;
    for (off_t off = 0; off < st.st_size; seg++) {
        uint16_t len = st.st_size - off > SEG_MAX ? SEG_MAX : st.st_size - off;
        int r = send_seg(sock, node, port, txn++, st.st_size, seg, data + off, len,
                         off + len >= st.st_size);
        if (r) {
            LOGE("BDF segment %u failed: %d", seg, r);
            free(data);
            return r;
        }
        off += len;
    }
    free(data);
    LOGI("BDF download complete (%u segments)", seg);
    return 0;
}


#define QMI_WLFW_IND_REGISTER   0x20
#define QMI_WLFW_FW_READY_IND   0x21
#define QMI_WLFW_FW_MEM_READY_IND 0x37
#define FW_STATUS_FW_READY      0x02ULL
#define FW_STATUS_FW_MEM_READY  0x08ULL
#define FW_STATUS_MSA_READY     0x04ULL
#define QMI_WLFW_MSA_READY_IND  0x2B
#define QMI_WLFW_MAC_ADDR       0x33

/* MAC from /persist/wlan_mac.bin ("Intf0MacAddress=DCF090BFBFFD") */
static int read_mac(uint8_t mac[6])
{
    char buf[256];
    int fd = open("/persist/wlan_mac.bin", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "Intf0MacAddress=");
    if (!p) return -1;
    p += strlen("Intf0MacAddress=");
    for (int i = 0; i < 6; i++) {
        unsigned v;
        if (sscanf(p + 2 * i, "%2x", &v) != 1) return -1;
        mac[i] = v;
    }
    return 0;
}

/* Empty calibration report (no stored cal data), as ath10k sends after the
 * BDF; the firmware then runs its cold-boot calibration and reports ready. */
static int send_cal_report(int sock, uint32_t node, uint32_t port)
{
    uint8_t msg[7 + 4], *p = msg + 7, zero = 0;
    p = put_tlv(p, 0x01, &zero, 1);
    uint16_t mlen = (uint16_t)(p - (msg + 7));
    msg[0] = QMI_REQUEST; msg[1] = 0x79; msg[2] = 0;
    msg[3] = 0x26; msg[4] = 0;
    msg[5] = mlen & 0xff; msg[6] = mlen >> 8;
    if (qrtr_sendto(sock, node, port, msg, 7 + mlen) < 0) return -1;
    LOGI("sent empty calibration report");
    return 0;
}

static int send_mac(int sock, uint32_t node, uint32_t port)
{
    uint8_t mac[6];
    if (read_mac(mac)) {
        LOGE("no MAC in /persist/wlan_mac.bin");
        return -1;
    }
    uint8_t msg[7 + 9], *p = msg + 7;
    p = put_tlv(p, 0x10, mac, 6);
    uint16_t mlen = (uint16_t)(p - (msg + 7));
    msg[0] = QMI_REQUEST; msg[1] = 0x78; msg[2] = 0;
    msg[3] = QMI_WLFW_MAC_ADDR; msg[4] = 0;
    msg[5] = mlen & 0xff; msg[6] = mlen >> 8;
    if (qrtr_sendto(sock, node, port, msg, 7 + mlen) < 0) return -1;
    LOGI("sent MAC %02x:%02x:%02x:%02x:%02x:%02x to firmware",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

/* Register for WLFW indications (as cnss-daemon does). Returns fw_status bits
 * from the response, or -1 on failure. */
static long long ind_register(int sock, uint32_t node, uint32_t port)
{
    uint8_t msg[64], *p = msg + 7, one = 1;
    uint32_t client = 0x434e5353;   /* "CNSS" */
    p = put_tlv(p, 0x10, &one, 1);      /* fw_ready_enable */
    p = put_tlv(p, 0x11, &one, 1);      /* initiate_cal_download_enable */
    p = put_tlv(p, 0x12, &one, 1);      /* initiate_cal_update_enable */
    p = put_tlv(p, 0x13, &one, 1);      /* msa_ready_enable */
    p = put_tlv(p, 0x14, &one, 1);      /* pin_connect_result_enable */
    p = put_tlv(p, 0x15, &client, 4);   /* client_id */
    p = put_tlv(p, 0x16, &one, 1);      /* request_mem_enable */
    p = put_tlv(p, 0x17, &one, 1);      /* fw_mem_ready_enable */
    p = put_tlv(p, 0x18, &one, 1);      /* cold_boot_cal_done_enable */
    p = put_tlv(p, 0x1A, &one, 1);      /* xo_cal_enable */
    uint16_t mlen = (uint16_t)(p - (msg + 7));
    msg[0] = QMI_REQUEST; msg[1] = 0x77; msg[2] = 0;
    msg[3] = QMI_WLFW_IND_REGISTER; msg[4] = 0;
    msg[5] = mlen & 0xff; msg[6] = mlen >> 8;
    if (qrtr_sendto(sock, node, port, msg, 7 + mlen) < 0)
        return -1;
    for (int tries = 0; tries < 50; tries++) {
        if (qrtr_poll(sock, 200) <= 0) continue;
        uint8_t buf[256]; uint32_t rn, rp;
        int n = qrtr_recvfrom(sock, buf, sizeof(buf), &rn, &rp);
        if (n < 7 || buf[0] != QMI_RESPONSE || buf[3] != QMI_WLFW_IND_REGISTER) continue;
        long long status = 0; int res = 0;
        uint8_t *q = buf + 7, *e = buf + n;
        while (q + 3 <= e) {
            uint16_t l = q[1] | q[2] << 8;
            if (q[0] == 0x02 && l >= 4) res = q[3] | q[4] << 8;
            if (q[0] == 0x10 && l >= 8) memcpy(&status, q + 3, 8);
            q += 3 + l;
        }
        LOGI("indication register: result %d fw_status 0x%llx", res, status);
        return res ? -1 : status;
    }
    return -1;
}

/* Wait for FW_MEM_READY (then send the BDF) and report FW_READY. */
static void serve_wlfw(uint32_t node, uint32_t port)
{
    int sock = qrtr_open(0);
    if (sock < 0) return;
    long long st = ind_register(sock, node, port);
    int sent = 0;
    if (st >= 0 && (st & (FW_STATUS_FW_MEM_READY | FW_STATUS_MSA_READY))) {
        LOGI("MSA/memory already ready: sending BDF + MAC");
        sent = download_bdf(sock, node, port) == 0;
        send_mac(sock, node, port);
        send_cal_report(sock, node, port);
    }
    if (st >= 0 && (st & FW_STATUS_FW_READY))
        LOGI("firmware already reports FW_READY");
    for (;;) {
        if (qrtr_poll(sock, 1000) <= 0) continue;
        uint8_t buf[512]; uint32_t rn, rp;
        int n = qrtr_recvfrom(sock, buf, sizeof(buf), &rn, &rp);
        if (n < 0 && errno != EAGAIN) break;
        if (n < 7 || buf[0] != QMI_INDICATION) continue;
        uint16_t id = buf[3] | buf[4] << 8;
        char hex[3 * 32 + 1] = {0};
        for (int i = 7; i < n && i < 7 + 32; i++)
            sprintf(hex + 3 * (i - 7), "%02x ", buf[i]);
        LOGI("WLFW indication 0x%x (%d bytes): %s", id, n - 7, hex);
        if ((id == QMI_WLFW_FW_MEM_READY_IND || id == QMI_WLFW_MSA_READY_IND) && !sent) {
            LOGI("MSA ready: sending BDF + MAC");
            for (int a = 0; a < 5 && !sent; a++) {
                sent = download_bdf(sock, node, port) == 0;
                if (!sent) sleep(1);
            }
            send_mac(sock, node, port);
            send_cal_report(sock, node, port);
        } else if (id == QMI_WLFW_FW_READY_IND) {
            LOGI("WLAN firmware is READY");
        }
    }
    close(sock);
}

int main(void)
{
    signal(SIGHUP, SIG_IGN);
    for (;;) {
        int sock = qrtr_open(0);
        if (sock < 0) {
            LOGE("qrtr_open: %s", strerror(errno));
            sleep(2);
            continue;
        }
        if (qrtr_new_lookup(sock, WLFW_SERVICE_ID, 0, 0) < 0) {
            LOGE("lookup: %s", strerror(errno));
            close(sock);
            sleep(2);
            continue;
        }
        LOGI("waiting for the WLFW service");
        for (;;) {
            uint8_t buf[256];
            struct sockaddr_qrtr sq;
            socklen_t sl = sizeof(sq);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (void *)&sq, &sl);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) {          /* libqrtr sockets are non-blocking */
                    qrtr_poll(sock, 1000);
                    continue;
                }
                LOGE("recvfrom: %s", strerror(errno));
                break;
            }
            struct qrtr_packet pkt;
            if (qrtr_decode(&pkt, buf, n, &sq) < 0)
                continue;
            if (pkt.type == QRTR_TYPE_NEW_SERVER)
                LOGI("server: service 0x%x ver %u inst %u at %u:%u", pkt.service, pkt.version, pkt.instance, pkt.node, pkt.port);
            if (pkt.type == QRTR_TYPE_NEW_SERVER && pkt.service == WLFW_SERVICE_ID && pkt.port) {
                LOGI("WLFW service up at %u:%u", pkt.node, pkt.port);
                static uint32_t served_port;
                if (pkt.port == served_port)
                    continue;
                served_port = pkt.port;
                if (fork() == 0) {          /* one handler per WLFW instance */
                    serve_wlfw(pkt.node, pkt.port);
                    _exit(0);
                }
            } else if (pkt.type == QRTR_TYPE_DEL_SERVER && pkt.service == WLFW_SERVICE_ID) {
                LOGI("WLFW service went away");
            }
        }
        close(sock);
        sleep(1);
    }
}
