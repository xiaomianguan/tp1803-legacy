/* QMI over QRTR: see qmi.h */
#include "qmi.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL       0xfffffffeu
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_NEW_LOOKUP 10

struct sockaddr_qrtr { unsigned short sq_family; uint32_t sq_node; uint32_t sq_port; };
struct qrtr_ctrl_pkt { uint32_t cmd; uint32_t service, instance, node, port; };

static void default_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void (*qmi_log)(const char *fmt, ...) = default_log;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---- name service lookup ---- */

static int lookup(uint32_t service, uint32_t *node, uint32_t *port, int timeout_ms)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct sockaddr_qrtr me;
    socklen_t ml = sizeof(me);
    if (getsockname(fd, (struct sockaddr *)&me, &ml) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    /* instance 0 + mask 0 = any instance */
    struct qrtr_ctrl_pkt pkt = { QRTR_TYPE_NEW_LOOKUP, service, 0, 0, 0 };
    struct sockaddr_qrtr ns = { AF_QIPCRTR, me.sq_node, QRTR_PORT_CTRL };
    if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ns, sizeof(ns)) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    long long deadline = now_ms() + timeout_ms;
    int r = -ETIMEDOUT;
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0)
            break;
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, left) <= 0)
            continue;
        struct qrtr_ctrl_pkt resp;
        ssize_t n = recv(fd, &resp, sizeof(resp), 0);
        if (n < (ssize_t)sizeof(resp))
            continue;
        if (resp.cmd == QRTR_TYPE_NEW_SERVER && resp.service == service && resp.port) {
            /* Prefer the modem (node 0): other subsystems publish some of
             * the same service ids (e.g. SLPI). */
            *node = resp.node;
            *port = resp.port;
            r = 0;
            if (resp.node == 0)
                break;
        } else if (resp.cmd == QRTR_TYPE_NEW_SERVER && resp.service == 0 && r == 0) {
            break;      /* end of the initial listing */
        }
    }
    close(fd);
    return r;
}

/* ---- reader ---- */

static void *reader(void *arg)
{
    struct qmi_client *c = arg;
    static __thread struct qmi_msg in;
    while (c->running) {
        struct pollfd pf = { c->fd, POLLIN, 0 };
        if (poll(&pf, 1, 500) <= 0)
            continue;
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(c->fd, in.buf, sizeof(in.buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 7)
            continue;
        if (from.sq_port != c->port)
            continue;           /* control messages etc. */
        in.len = n;
        uint8_t flags = in.buf[0];
        uint16_t txn = qmi_le16(in.buf + 1);
        if (flags == 2) {               /* response */
            pthread_mutex_lock(&c->lock);
            if (c->wait_resp && !c->wait_done && txn == c->wait_txn) {
                memcpy(c->wait_resp->buf, in.buf, n);
                c->wait_resp->len = n;
                c->wait_done = 1;
                pthread_cond_broadcast(&c->cond);
            }
            pthread_mutex_unlock(&c->lock);
        } else if (flags == 4 && c->ind_cb) {   /* indication */
            size_t tl;
            const uint8_t *t = qmi_msg_tlvs(&in, &tl);
            c->ind_cb(c, qmi_msg_id(&in), t, tl, c->ind_user);
        }
    }
    return NULL;
}

int qmi_client_open(struct qmi_client *c, uint32_t service, int timeout_ms,
                    qmi_ind_cb cb, void *user)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    int r = lookup(service, &c->node, &c->port, timeout_ms);
    if (r)
        return r;
    c->fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0)
        return -errno;
    c->service = service;
    c->next_txn = 1;
    c->ind_cb = cb;
    c->ind_user = user;
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->req_lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->running = 1;
    if (pthread_create(&c->reader, NULL, reader, c)) {
        close(c->fd);
        c->fd = -1;
        return -EAGAIN;
    }
    return 0;
}

void qmi_client_close(struct qmi_client *c)
{
    if (c->fd < 0)
        return;
    c->running = 0;
    pthread_join(c->reader, NULL);
    close(c->fd);
    c->fd = -1;
}

/* ---- messages ---- */

void qmi_msg_init(struct qmi_msg *m, uint16_t msg_id)
{
    m->buf[0] = 0;
    m->buf[1] = m->buf[2] = 0;
    m->buf[3] = msg_id & 0xff;
    m->buf[4] = msg_id >> 8;
    m->buf[5] = m->buf[6] = 0;
    m->len = 7;
}

int qmi_tlv_add(struct qmi_msg *m, uint8_t type, const void *data, uint16_t len)
{
    if (m->len + 3 + len > sizeof(m->buf))
        return -ENOSPC;
    uint8_t *p = m->buf + m->len;
    p[0] = type;
    p[1] = len & 0xff;
    p[2] = len >> 8;
    if (len)
        memcpy(p + 3, data, len);
    m->len += 3 + len;
    uint16_t tl = m->len - 7;
    m->buf[5] = tl & 0xff;
    m->buf[6] = tl >> 8;
    return 0;
}

int qmi_tlv_add_u8(struct qmi_msg *m, uint8_t type, uint8_t v) { return qmi_tlv_add(m, type, &v, 1); }
int qmi_tlv_add_u16(struct qmi_msg *m, uint8_t type, uint16_t v)
{
    uint8_t b[2] = { v & 0xff, v >> 8 };
    return qmi_tlv_add(m, type, b, 2);
}
int qmi_tlv_add_u32(struct qmi_msg *m, uint8_t type, uint32_t v)
{
    uint8_t b[4] = { v & 0xff, v >> 8, v >> 16, v >> 24 };
    return qmi_tlv_add(m, type, b, 4);
}

uint16_t qmi_msg_id(const struct qmi_msg *m) { return qmi_le16(m->buf + 3); }

const uint8_t *qmi_msg_tlvs(const struct qmi_msg *m, size_t *len)
{
    size_t tl = qmi_le16(m->buf + 5);
    if (tl > m->len - 7)
        tl = m->len - 7;
    *len = tl;
    return m->buf + 7;
}

const uint8_t *qmi_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, uint16_t *vlen)
{
    size_t off = 0;
    while (off + 3 <= len) {
        uint16_t l = qmi_le16(tlvs + off + 1);
        if (off + 3 + l > len)
            break;
        if (tlvs[off] == type) {
            if (vlen)
                *vlen = l;
            return tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

int qmi_request(struct qmi_client *c, struct qmi_msg *req, struct qmi_msg *resp,
                int timeout_ms, uint16_t *qmi_err)
{
    static __thread struct qmi_msg scratch;
    if (!resp)
        resp = &scratch;
    pthread_mutex_lock(&c->req_lock);
    pthread_mutex_lock(&c->lock);
    uint16_t txn = c->next_txn++;
    if (!c->next_txn)
        c->next_txn = 1;
    c->wait_txn = txn;
    c->wait_done = 0;
    c->wait_resp = resp;
    pthread_mutex_unlock(&c->lock);

    req->buf[0] = 0;
    req->buf[1] = txn & 0xff;
    req->buf[2] = txn >> 8;
    struct sockaddr_qrtr to = { AF_QIPCRTR, c->node, c->port };
    int r = 0;
    if (sendto(c->fd, req->buf, req->len, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
        r = -errno;

    pthread_mutex_lock(&c->lock);
    if (!r) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!c->wait_done) {
            if (pthread_cond_timedwait(&c->cond, &c->lock, &ts) == ETIMEDOUT) {
                r = -ETIMEDOUT;
                break;
            }
        }
    }
    c->wait_resp = NULL;
    pthread_mutex_unlock(&c->lock);
    pthread_mutex_unlock(&c->req_lock);

    if (!r && qmi_err) {
        size_t tl;
        const uint8_t *t = qmi_msg_tlvs(resp, &tl);
        uint16_t vl;
        const uint8_t *res = qmi_tlv_find(t, tl, 0x02, &vl);
        *qmi_err = (res && vl >= 4) ? qmi_le16(res + 2) : 0xffff;
        if (res && vl >= 4 && qmi_le16(res) == 0)
            *qmi_err = 0;
    }
    if (r)
        qmi_log("qmi: svc 0x%02x msg 0x%04x: %s", c->service, qmi_le16(req->buf + 3),
                r == -ETIMEDOUT ? "timeout" : strerror(-r));
    return r;
}

int qmi_simple(struct qmi_client *c, uint16_t msg_id, struct qmi_msg *resp, uint16_t *qmi_err)
{
    struct qmi_msg req;
    qmi_msg_init(&req, msg_id);
    return qmi_request(c, &req, resp, 5000, qmi_err);
}
