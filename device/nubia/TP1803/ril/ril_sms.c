/* SMS for the TP1803 QMI RIL (3GPP, over QMI WMS).
 *
 * Incoming messages are routed "transfer only": the modem hands every
 * point-to-point message (and status report) to us without storing it, and
 * waits for our ack, which we send when the framework acknowledges. Messages
 * the modem stored anyway (e.g. class 2 on the SIM) are read, delivered and
 * deleted. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "RIL-TP1803"
#include <utils/Log.h>

#include "ril_qmi.h"

#define WMS_SET_EVENT_REPORT  0x0001
#define WMS_EVENT_REPORT_IND  0x0001
#define WMS_RAW_SEND          0x0020
#define WMS_RAW_READ          0x0022
#define WMS_DELETE            0x0024
#define WMS_SET_ROUTES        0x0032
#define WMS_GET_SMSC          0x0034
#define WMS_SET_SMSC          0x0035
#define WMS_SEND_ACK          0x0037
#define WMS_SET_MEMORY_STATUS 0x003C

#define FMT_GW_PP   6
#define MODE_GW     1

/* Framework acks arrive in delivery order, without an id: keep a FIFO of
 * the modem transactions waiting for them (-1 = nothing to ack). */
#define ACKQ 32
static pthread_mutex_t sms_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t ackq[ACKQ];
static int ack_head, ack_count;

static char *tohex(const uint8_t *b, size_t n)
{
    static const char d[] = "0123456789ABCDEF";
    char *s = malloc(n * 2 + 1);
    if (!s)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = d[b[i] >> 4];
        s[2 * i + 1] = d[b[i] & 15];
    }
    s[n * 2] = 0;
    return s;
}

static int unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            return -1;
        out[n++] = v;
        s += 2;
    }
    return *s ? -1 : (int)n;
}

/* Length an SMS-DELIVER TPDU at p would have, or -1 if it isn't one. */
static int deliver_tpdu_len(const uint8_t *p, size_t len)
{
    if (len < 2 || (p[0] & 3) != 0)
        return -1;
    size_t i = 2 + (p[1] + 1) / 2 + 1;          /* first, OA len/digits, OA type */
    if (i + 10 > len)
        return -1;
    uint8_t dcs = p[i + 1];
    i += 2 + 7;                                  /* PID, DCS, SCTS */
    unsigned udl = p[i++];
    int septets = !(dcs & 0x80) ? ((dcs >> 2) & 3) == 0      /* general coding */
                : (dcs & 0xf0) == 0xf0 ? !(dcs & 4)             /* data coding */
                : (dcs & 0xf0) != 0xe0;                         /* MWI: GSM7 unless UCS2 */
    return i + (septets ? (udl * 7 + 7) / 8 : udl);
}

/* QMI hands over the bare TPDU; the framework wants SMSC + TPDU. Messages
 * read from storage may or may not carry the SMSC: decide by length. */
static void deliver(const uint8_t *raw, size_t len, int64_t txn, int maybe_smsc)
{
    int has_smsc = 0;
    if (maybe_smsc && deliver_tpdu_len(raw, len) != (int)len &&
        len > 1u + raw[0] && deliver_tpdu_len(raw + 1 + raw[0], len - 1 - raw[0]) ==
                                 (int)(len - 1 - raw[0]))
        has_smsc = 1;
    const uint8_t *tpdu = has_smsc ? raw + 1 + raw[0] : raw;
    size_t tlen = has_smsc ? len - 1 - raw[0] : len;
    if (tlen < 2)
        return;
    uint8_t mti = tpdu[0] & 3;
    char *hex = tohex(tpdu, tlen);
    char *msg = hex ? malloc(strlen(hex) + 3) : NULL;
    if (!msg) {
        free(hex);
        return;
    }
    snprintf(msg, strlen(hex) + 3, "00%s", hex);   /* empty SMSC */
    free(hex);
    pthread_mutex_lock(&sms_lock);
    if (ack_count < ACKQ) {
        ackq[(ack_head + ack_count) % ACKQ] = txn;
        ack_count++;
    }
    pthread_mutex_unlock(&sms_lock);
    ALOGI("sms: incoming %s (%zu bytes, txn %lld)", mti == 2 ? "status report" : "message",
          tlen, (long long)txn);
    RIL_onUnsolicitedResponse(mti == 2 ? RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT
                                       : RIL_UNSOL_RESPONSE_NEW_SMS,
                              msg, strlen(msg));
    free(msg);
}

struct stored { uint8_t storage; uint32_t index; };

/* Read, deliver and delete a message the modem stored. Runs on its own
 * thread: the indication thread must not wait for WMS responses. */
static void *fetch_stored(void *arg)
{
    struct stored s = *(struct stored *)arg;
    free(arg);
    struct qmi_msg q, r;
    uint16_t e;
    uint8_t id[5] = { s.storage, s.index, s.index >> 8, s.index >> 16, s.index >> 24 };
    qmi_msg_init(&q, WMS_RAW_READ);
    qmi_tlv_add(&q, 0x01, id, 5);
    qmi_tlv_add_u8(&q, 0x10, MODE_GW);
    if (qmi_request(&q_wms, &q, &r, 5000, &e) || e) {
        ALOGW("sms: reading stored %u/%u failed (%u)", s.storage, s.index, e);
        return NULL;
    }
    size_t tl;
    const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(tv, tl, 0x01, &vl);
    /* tag, format, u16 len, data */
    if (v && vl >= 4 && v[1] == FMT_GW_PP && 4u + qmi_le16(v + 2) <= vl)
        deliver(v + 4, qmi_le16(v + 2), -1, 1);
    qmi_msg_init(&q, WMS_DELETE);
    qmi_tlv_add_u8(&q, 0x01, s.storage);
    qmi_tlv_add_u32(&q, 0x10, s.index);
    qmi_tlv_add_u8(&q, 0x12, MODE_GW);
    qmi_request(&q_wms, &q, &r, 5000, &e);
    return NULL;
}

static void wms_ind(struct qmi_client *c, uint16_t id, const uint8_t *t, size_t tl, void *user)
{
    (void)c; (void)user;
    if (id != WMS_EVENT_REPORT_IND)
        return;
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(t, tl, 0x11, &vl);
    /* transfer route MT message: ack indicator, u32 txn, format, u16 len, data */
    if (v && vl >= 8) {
        uint16_t n = qmi_le16(v + 6);
        if (v[5] != FMT_GW_PP || 8u + n > vl) {
            ALOGW("sms: ignoring message format %u", v[5]);
            return;
        }
        deliver(v + 8, n, v[0] == 0 ? (int64_t)qmi_le32(v + 1) : -1, 0);
        return;
    }
    v = qmi_tlv_find(t, tl, 0x10, &vl);
    /* MT message stored: storage, u32 index */
    if (v && vl >= 5) {
        struct stored *s = malloc(sizeof(*s));
        if (!s)
            return;
        s->storage = v[0];
        s->index = qmi_le32(v + 1);
        ALOGI("sms: message stored at %u/%u", s->storage, s->index);
        pthread_t th;
        if (pthread_create(&th, NULL, fetch_stored, s) == 0)
            pthread_detach(th);
        else
            free(s);
    }
}

static int set_routes(int class2_on_sim)
{
    struct qmi_msg q, r;
    uint16_t e;
    /* u16 count, then {type, class, storage, action} per route */
    uint8_t routes[2 + 5 * 4];
    routes[0] = 5;
    routes[1] = 0;
    for (int cls = 0; cls < 5; cls++) {
        uint8_t *p = routes + 2 + cls * 4;
        p[0] = 0;                                   /* point to point */
        p[1] = cls;                                 /* class 0..3, 4 = none */
        if (cls == 2 && class2_on_sim) {
            p[2] = 0;                               /* UIM */
            p[3] = 1;                               /* store and notify */
        } else {
            p[2] = 0xff;                            /* no storage */
            p[3] = 2;                               /* transfer only */
        }
    }
    qmi_msg_init(&q, WMS_SET_ROUTES);
    qmi_tlv_add(&q, 0x01, routes, sizeof(routes));
    qmi_tlv_add_u8(&q, 0x10, 1);                    /* transfer status reports */
    if (qmi_request(&q_wms, &q, &r, 5000, &e))
        return -1;
    return e;
}

int sms_init(void)
{
    if (qmi_client_open(&q_wms, QMI_WMS, 10000, wms_ind, NULL))
        return -1;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, WMS_SET_EVENT_REPORT);
    qmi_tlv_add_u8(&q, 0x10, 1);                    /* report new MT messages */
    if (qmi_request(&q_wms, &q, &r, 5000, &e) || e)
        ALOGW("sms: event report setup failed (%u)", e);
    int re = set_routes(0);
    if (re) {
        ALOGW("sms: routes rejected (%d), storing class 2 on the SIM", re);
        re = set_routes(1);
    }
    if (re)
        ALOGE("sms: cannot set routes (%d)", re);
    return 0;
}

static void send_sms(RIL_Token t, char **d)
{
    const char *smsc = d[0] && d[0][0] ? d[0] : "00";
    const char *pdu = d[1] ? d[1] : "";
    uint8_t raw[3 + 512];
    int sl = unhex(smsc, raw + 3, 32);
    int pl = sl < 0 ? -1 : unhex(pdu, raw + 3 + sl, sizeof(raw) - 3 - sl);
    if (pl <= 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    uint16_t n = sl + pl;
    raw[0] = FMT_GW_PP;
    raw[1] = n;
    raw[2] = n >> 8;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, WMS_RAW_SEND);
    qmi_tlv_add(&q, 0x01, raw, 3 + n);
    RIL_SMS_Response resp = { 0, NULL, -1 };
    if (qmi_request(&q_wms, &q, &r, 60000, &e)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    size_t tl;
    const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(tv, tl, 0x01, &vl);
    if (v && vl >= 2)
        resp.messageRef = qmi_le16(v);
    if (e) {
        /* GSM/WCDMA cause: u16 RP cause, u8 TP cause */
        v = qmi_tlv_find(tv, tl, 0x12, &vl);
        int rp = v && vl >= 2 ? qmi_le16(v) : -1;
        int tp = v && vl >= 3 ? v[2] : -1;
        ALOGW("sms: send failed, qmi error %u rp %d tp %d", e, rp, tp);
        resp.errorCode = rp >= 0 ? rp : -1;
        /* Network rejections that are worth retrying vs. not. */
        RIL_Errno err = (e == 0x0034 /* network not ready */ ||
                         e == 0x0040 /* no network found */ ||
                         rp == 41 || rp == 42 || rp == 47)
                        ? RIL_E_SMS_SEND_FAIL_RETRY : RIL_E_GENERIC_FAILURE;
        RIL_onRequestComplete(t, err, &resp, sizeof(resp));
        return;
    }
    ALOGI("sms: sent, ref %d", resp.messageRef);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &resp, sizeof(resp));
}

static void ack_sms(RIL_Token t, int *d)
{
    int success = d ? d[0] : 1;
    int cause = d ? d[1] : 0;
    int64_t txn = -1;
    pthread_mutex_lock(&sms_lock);
    if (ack_count) {
        txn = ackq[ack_head];
        ack_head = (ack_head + 1) % ACKQ;
        ack_count--;
    }
    pthread_mutex_unlock(&sms_lock);
    if (txn >= 0) {
        struct qmi_msg q, r;
        uint16_t e;
        uint8_t info[6] = { txn, txn >> 8, txn >> 16, txn >> 24, MODE_GW, success ? 1 : 0 };
        qmi_msg_init(&q, WMS_SEND_ACK);
        qmi_tlv_add(&q, 0x01, info, sizeof(info));
        if (!success) {
            /* RP cause 0x6F (protocol error) unless memory is full (0xD3) */
            uint8_t fail[2] = { cause == 0xD3 ? 22 : 111, cause & 0xff };
            qmi_tlv_add(&q, 0x11, fail, 2);
        }
        if (qmi_request(&q_wms, &q, &r, 5000, &e) || e)
            ALOGW("sms: ack of txn %lld failed (%u)", (long long)txn, e);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void get_smsc(RIL_Token t)
{
    struct qmi_msg r;
    uint16_t e;
    if (qmi_simple(&q_wms, WMS_GET_SMSC, &r, &e) || e) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    size_t tl;
    const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(tv, tl, 0x01, &vl);
    /* char type[3], u8 len, digits */
    if (!v || vl < 4 || 4u + v[3] > vl) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    char addr[64];
    int n = v[3] < 40 ? v[3] : 40;
    int intl = v[4] == '+';
    snprintf(addr, sizeof(addr), "\"%.*s\",%d", n, (const char *)v + 4, intl ? 145 : 129);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, addr, strlen(addr));
}

static void set_smsc(RIL_Token t, const char *s)
{
    /* framework format: "+123456789",145 (quotes and type optional) */
    char addr[48];
    int n = 0;
    for (; s && *s && *s != ',' && n < (int)sizeof(addr) - 1; s++)
        if (*s != '"')
            addr[n++] = *s;
    addr[n] = 0;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, WMS_SET_SMSC);
    qmi_tlv_add(&q, 0x01, addr, n);
    if (qmi_request(&q_wms, &q, &r, 5000, &e) || e)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

void sms_request(int request, void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;
    switch (request) {
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        send_sms(t, data);
        break;
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
        ack_sms(t, data);
        break;
    case RIL_REQUEST_GET_SMSC_ADDRESS:
        get_smsc(t);
        break;
    case RIL_REQUEST_SET_SMSC_ADDRESS:
        set_smsc(t, data);
        break;
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS: {
        struct qmi_msg q, r;
        uint16_t e;
        qmi_msg_init(&q, WMS_SET_MEMORY_STATUS);
        qmi_tlv_add_u8(&q, 0x01, data && ((int *)data)[0] ? 1 : 0);
        qmi_request(&q_wms, &q, &r, 5000, &e);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    }
    case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
    }
}
