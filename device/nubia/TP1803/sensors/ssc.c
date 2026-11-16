/*
 * Minimal SEE (Snapdragon Sensor Core) client: QMI SNS_CLIENT over QRTR with
 * hand-rolled protobuf encoding. See ssc.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ssc.h"

/* QRTR (AF_QIPCRTR) definitions; Android 6's bionic predates them. */
#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#define QRTR_PORT_CTRL 0xfffffffeu
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_NEW_LOOKUP 10
struct sockaddr_qrtr { unsigned short sq_family; uint32_t sq_node; uint32_t sq_port; };
struct qrtr_ctrl_pkt { uint32_t cmd; uint32_t service, instance, node, port; };
#define QMI_REQUEST    0
#define QMI_INDICATION 4

#define QMI_SNS_CLIENT_REQ        0x20
#define QMI_SNS_CLIENT_JUMBO_IND  0x21
#define QMI_SNS_CLIENT_REPORT_IND 0x22

struct ssc {
    int sock;
    uint32_t node, port;
    uint16_t txn;
    int wakeup;
    ssc_event_cb cb;
    void *ctx;
    /* SUID lookup state */
    const char *want_type;
    struct ssc_suid found;
    int found_ok;
};

static int ssc_debug = -1;
static const struct ssc_suid SUID_LOOKUP = { 0xababababababababULL, 0xababababababababULL };

/* ------------------------------------------------------------ protobuf */

struct pb { uint8_t *p; size_t len, cap; };

static void pb_byte(struct pb *b, uint8_t v) { if (b->len < b->cap) b->p[b->len] = v; b->len++; }
static void pb_varint(struct pb *b, uint64_t v)
{
    do { uint8_t c = v & 0x7f; v >>= 7; pb_byte(b, c | (v ? 0x80 : 0)); } while (v);
}
static void pb_tag(struct pb *b, int field, int wt) { pb_varint(b, (uint64_t)field << 3 | wt); }
static void pb_fixed64(struct pb *b, int field, uint64_t v)
{
    pb_tag(b, field, 1);
    for (int i = 0; i < 8; i++) pb_byte(b, v >> (8 * i));
}
static void pb_fixed32(struct pb *b, int field, uint32_t v)
{
    pb_tag(b, field, 5);
    for (int i = 0; i < 4; i++) pb_byte(b, v >> (8 * i));
}
static void pb_bytes(struct pb *b, int field, const void *d, size_t n)
{
    pb_tag(b, field, 2);
    pb_varint(b, n);
    for (size_t i = 0; i < n; i++) pb_byte(b, ((const uint8_t *)d)[i]);
}
static void pb_uint(struct pb *b, int field, uint64_t v) { pb_tag(b, field, 0); pb_varint(b, v); }
static void pb_float(struct pb *b, int field, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    pb_tag(b, field, 5);
    for (int i = 0; i < 4; i++) pb_byte(b, v >> (8 * i));
}

static int rd_varint(const uint8_t **p, const uint8_t *e, uint64_t *v)
{
    *v = 0;
    for (int s = 0; *p < e && s < 64; s += 7) {
        uint8_t c = *(*p)++;
        *v |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80)) return 0;
    }
    return -1;
}

/* Iterate fields: calls f for each (field, wiretype, value ptr/len, varint) */
typedef int (*pb_visit)(void *ctx, int field, int wt, const uint8_t *d, size_t n, uint64_t v);
static int pb_walk(const uint8_t *p, size_t len, pb_visit f, void *ctx)
{
    const uint8_t *e = p + len;
    while (p < e) {
        uint64_t key, v = 0;
        if (rd_varint(&p, e, &key)) return -1;
        int field = key >> 3, wt = key & 7;
        const uint8_t *d = p; size_t n = 0;
        switch (wt) {
        case 0: if (rd_varint(&p, e, &v)) return -1; break;
        case 1: if (p + 8 > e) return -1; memcpy(&v, p, 8); n = 8; p += 8; break;
        case 5: if (p + 4 > e) return -1; { uint32_t t; memcpy(&t, p, 4); v = t; } n = 4; p += 4; break;
        case 2: if (rd_varint(&p, e, &v) || p + v > e) return -1; d = p; n = v; p += v; break;
        default: return -1;
        }
        if (f(ctx, field, wt, d, n, v)) return 1;
    }
    return 0;
}

struct float_ctx { int field; float *out; int max, n; };
static int float_visit(void *c, int field, int wt, const uint8_t *d, size_t n, uint64_t v)
{
    struct float_ctx *fc = c;
    if (field != fc->field) return 0;
    if (wt == 5 && fc->n < fc->max) { uint32_t t = v; memcpy(&fc->out[fc->n++], &t, 4); }
    else if (wt == 2)                 /* packed */
        for (size_t i = 0; i + 4 <= n && fc->n < fc->max; i += 4) memcpy(&fc->out[fc->n++], d + i, 4);
    return 0;
}
int pb_floats(const uint8_t *p, size_t len, int field, float *out, int max)
{
    struct float_ctx fc = { field, out, max, 0 };
    pb_walk(p, len, float_visit, &fc);
    return fc.n;
}

struct varint_ctx { int field; uint64_t v; int found; };
static int varint_visit(void *c, int field, int wt, const uint8_t *d, size_t n, uint64_t v)
{
    struct varint_ctx *vc = c; (void)d; (void)n;
    if (field == vc->field && (wt == 0 || wt == 5 || wt == 1)) { vc->v = v; vc->found = 1; return 1; }
    return 0;
}
int pb_varint_field(const uint8_t *p, size_t len, int field, uint64_t *out)
{
    struct varint_ctx vc = { field, 0, 0 };
    pb_walk(p, len, varint_visit, &vc);
    if (vc.found) *out = vc.v;
    return vc.found ? 0 : -1;
}

/* ------------------------------------------------------------ QMI */

static int send_request(struct ssc *s, const struct ssc_suid *suid, uint32_t msg_id,
                        const uint8_t *payload, size_t plen)
{
    uint8_t req[1024], inner[900], sbuf[32], susp[16];
    struct pb sb = { sbuf, 0, sizeof(sbuf) };
    pb_fixed64(&sb, 1, suid->low);
    pb_fixed64(&sb, 2, suid->high);
    struct pb sp = { susp, 0, sizeof(susp) };
    pb_uint(&sp, 1, 1);      /* client_proc_type: SNS_STD_CLIENT_PROCESSOR_APSS */
    pb_uint(&sp, 2, s->wakeup ? 0 : 1);   /* delivery_type: WAKEUP / NO_WAKEUP */
    struct pb ib = { inner, 0, sizeof(inner) };   /* sns_std_request */
    if (payload)
        pb_bytes(&ib, 2, payload, plen);
    struct pb rb = { req, 0, sizeof(req) };       /* sns_client_request_msg */
    pb_bytes(&rb, 1, sbuf, sb.len);
    pb_fixed32(&rb, 2, msg_id);
    pb_bytes(&rb, 3, susp, sp.len);
    pb_bytes(&rb, 4, inner, ib.len);
    if (rb.len > rb.cap) return -E2BIG;

    uint8_t msg[7 + 3 + 2 + sizeof(req) + 4];
    uint8_t *p = msg + 7;
    *p++ = 0x01;                                  /* payload: u16 len + bytes */
    uint16_t tl = 2 + rb.len;
    *p++ = tl & 0xff; *p++ = tl >> 8;
    *p++ = rb.len & 0xff; *p++ = rb.len >> 8;
    memcpy(p, req, rb.len); p += rb.len;
    *p++ = 0x10; *p++ = 1; *p++ = 0; *p++ = 1;    /* use_jumbo_report = 1 */
    uint16_t mlen = p - (msg + 7);
    uint16_t txn = ++s->txn;
    msg[0] = QMI_REQUEST; msg[1] = txn & 0xff; msg[2] = txn >> 8;
    msg[3] = QMI_SNS_CLIENT_REQ; msg[4] = 0;
    msg[5] = mlen & 0xff; msg[6] = mlen >> 8;
    struct sockaddr_qrtr sq = { AF_QIPCRTR, s->node, s->port };
    if (sendto(s->sock, msg, 7 + mlen, 0, (void *)&sq, sizeof(sq)) < 0)
        return -errno;
    return 0;
}

/* ------------------------------------------------------------ events */

struct ev_ctx { struct ssc *s; struct ssc_suid suid; };

static int suid_visit(void *c, int field, int wt, const uint8_t *d, size_t n, uint64_t v)
{
    struct ssc_suid *u = c; (void)d; (void)n;
    if (wt == 1 && field == 1) u->low = v;
    if (wt == 1 && field == 2) u->high = v;
    return 0;
}

struct suidev_ctx { struct ssc *s; char type[64]; };
static int suidev_visit(void *c, int field, int wt, const uint8_t *d, size_t n, uint64_t v)
{
    struct suidev_ctx *sc = c; (void)v;
    if (field == 1 && wt == 2) {
        size_t k = n < sizeof(sc->type) - 1 ? n : sizeof(sc->type) - 1;
        memcpy(sc->type, d, k); sc->type[k] = 0;
    } else if (field == 2 && wt == 2 && sc->s->want_type && !strcmp(sc->type, sc->s->want_type) && !sc->s->found_ok) {
        struct ssc_suid u = { 0, 0 };
        pb_walk(d, n, suid_visit, &u);
        if (u.low || u.high) { sc->s->found = u; sc->s->found_ok = 1; }
    }
    return 0;
}

static int event_visit(void *c, int field, int wt, const uint8_t *d, size_t n, uint64_t v)
{
    struct ev_ctx *ec = c; (void)v;
    if (field == 1 && wt == 2) {
        pb_walk(d, n, suid_visit, &ec->suid);
    } else if (field == 2 && wt == 2) {           /* sns_client_event */
        uint64_t msg_id = 0, ts = 0;
        const uint8_t *pl = NULL; size_t pn = 0;
        const uint8_t *p = d, *e = d + n;
        while (p < e) {
            uint64_t key, val = 0;
            if (rd_varint(&p, e, &key)) break;
            int f = key >> 3, w = key & 7;
            if (w == 5) { uint32_t t; memcpy(&t, p, 4); val = t; p += 4; }
            else if (w == 1) { memcpy(&val, p, 8); p += 8; }
            else if (w == 0) { if (rd_varint(&p, e, &val)) break; }
            else if (w == 2) { if (rd_varint(&p, e, &val)) break; if (f == 3) { pl = p; pn = val; } p += val; }
            else break;
            if (f == 1) msg_id = val;
            if (f == 2) ts = val;
        }
        if (msg_id == SNS_SUID_MSGID_SNS_SUID_EVENT && pl) {
            struct suidev_ctx sc = { ec->s, "" };
            pb_walk(pl, pn, suidev_visit, &sc);
        } else if (ec->s->cb && pl) {
            ec->s->cb(ec->s->ctx, &ec->suid, msg_id, ts, pl, pn);
        }
    }
    return 0;
}

static void handle_indication(struct ssc *s, const uint8_t *buf, int n)
{
    const uint8_t *q = buf + 7, *e = buf + n;
    while (q + 3 <= e) {
        uint8_t t = q[0];
        uint16_t l = q[1] | q[2] << 8;
        if (q + 3 + l > e) break;
        if (t == 0x02 && l >= 2) {                /* payload: u16 len + bytes */
            uint16_t pl = q[3] | q[4] << 8;
            if (5 + pl <= 3 + l) {
                struct ev_ctx ec = { s, { 0, 0 } };
                pb_walk(q + 5, pl, event_visit, &ec);
            }
        }
        q += 3 + l;
    }
}

int ssc_poll(struct ssc *s, int timeout_ms)
{
    struct pollfd pfd = { s->sock, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return 0;
    for (;;) {
        static uint8_t buf[65536];
        struct sockaddr_qrtr sq;
        socklen_t sl = sizeof(sq);
        int n = recvfrom(s->sock, buf, sizeof(buf), MSG_DONTWAIT, (void *)&sq, &sl);
        if (n < 0) break;
        if (n < 7 || sq.sq_node != s->node || sq.sq_port != s->port) continue;
        uint16_t id = buf[3] | buf[4] << 8;
        if (ssc_debug) {
            fprintf(stderr, "ssc: rx type %u id 0x%x len %d:", buf[0], id, n);
            for (int i = 7; i < n && i < 7 + 120; i++) fprintf(stderr, " %02x", buf[i]);
            fprintf(stderr, "\n");
        }
        if (buf[0] == QMI_INDICATION && (id == QMI_SNS_CLIENT_REPORT_IND || id == QMI_SNS_CLIENT_JUMBO_IND))
            handle_indication(s, buf, n);
    }
    return 1;
}

int ssc_fd(struct ssc *s) { return s->sock; }
void ssc_set_wakeup(struct ssc *s, int wakeup) { s->wakeup = wakeup; }

/* ------------------------------------------------------------ API */

static int open_sock(void)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* Ask the name server where SNS_CLIENT lives (cached after the first hit). */
static uint32_t svc_node, svc_port;
static int find_service(struct ssc *s, int timeout_ms)
{
    if (svc_port) { s->node = svc_node; s->port = svc_port; return 0; }
    int ls = open_sock();
    if (ls < 0) return -1;
    struct sockaddr_qrtr me; socklen_t ml = sizeof(me);
    if (getsockname(ls, (void *)&me, &ml) < 0) { close(ls); return -1; }
    struct qrtr_ctrl_pkt pkt = { QRTR_TYPE_NEW_LOOKUP, SSC_SERVICE, 0, 0, 0 };
    struct sockaddr_qrtr ns = { AF_QIPCRTR, me.sq_node, QRTR_PORT_CTRL };
    int ret = -1;
    if (sendto(ls, &pkt, sizeof(pkt), 0, (void *)&ns, sizeof(ns)) < 0) { close(ls); return -1; }
    for (int waited = 0; waited < timeout_ms && ret; waited += 100) {
        struct pollfd pfd = { ls, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;
        struct qrtr_ctrl_pkt r; struct sockaddr_qrtr sq; socklen_t sl = sizeof(sq);
        while (recvfrom(ls, &r, sizeof(r), MSG_DONTWAIT, (void *)&sq, &sl) >= (int)sizeof(r)) {
            if (r.cmd == QRTR_TYPE_NEW_SERVER && r.service == SSC_SERVICE && r.port) {
                svc_node = r.node; svc_port = r.port;
                s->node = r.node; s->port = r.port; ret = 0;
                break;
            }
            sl = sizeof(sq);
        }
    }
    close(ls);
    return ret;
}

struct ssc *ssc_open(ssc_event_cb cb, void *ctx)
{
    if (ssc_debug < 0) ssc_debug = getenv("SSC_DEBUG") != NULL;
    struct ssc *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cb = cb; s->ctx = ctx;
    if (find_service(s, 2000)) { free(s); return NULL; }
    s->sock = open_sock();
    if (s->sock < 0) { free(s); return NULL; }
    return s;
}

void ssc_close(struct ssc *s)
{
    if (!s) return;
    close(s->sock);
    free(s);
}

int ssc_lookup(struct ssc *s, const char *data_type, struct ssc_suid *out, int timeout_ms)
{
    uint8_t req[96];
    struct pb b = { req, 0, sizeof(req) };
    pb_bytes(&b, 1, data_type, strlen(data_type));   /* data_type */
    pb_uint(&b, 2, 1);                               /* register_updates */
    pb_uint(&b, 3, 1);                               /* default_only */
    s->want_type = data_type;
    s->found_ok = 0;
    if (send_request(s, &SUID_LOOKUP, SNS_SUID_MSGID_SNS_SUID_REQ, req, b.len))
        return -1;
    for (int waited = 0; waited < timeout_ms && !s->found_ok; waited += 100)
        ssc_poll(s, 100);
    s->want_type = NULL;
    if (!s->found_ok) return -1;
    *out = s->found;
    return 0;
}

int ssc_enable(struct ssc *s, const struct ssc_suid *suid, float rate_hz)
{
    if (rate_hz > 0) {
        uint8_t cfg[16];
        struct pb b = { cfg, 0, sizeof(cfg) };
        pb_float(&b, 1, rate_hz);                    /* sns_std_sensor_config.sample_rate */
        return send_request(s, suid, SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_CONFIG, cfg, b.len);
    }
    return send_request(s, suid, SNS_STD_SENSOR_MSGID_SNS_STD_ON_CHANGE_CONFIG, NULL, 0);
}

int ssc_disable(struct ssc *s, const struct ssc_suid *suid)
{
    return send_request(s, suid, SNS_CLIENT_MSGID_SNS_CLIENT_DISABLE_REQ, NULL, 0);
}
