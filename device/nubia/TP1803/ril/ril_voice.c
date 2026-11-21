/*
 * Voice calls for the TP1803 QMI RIL (QMI VOICE service). Message layouts
 * follow libqmi's qmi-service-voice.json.
 */
#define LOG_TAG "RIL-TP1803"

#include "ril_qmi.h"

#include <cutils/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VOICE_INDICATION_REGISTER 0x0003
#define VOICE_DIAL_CALL           0x0020
#define VOICE_END_CALL            0x0021
#define VOICE_ANSWER_CALL         0x0022
#define VOICE_START_CONT_DTMF     0x0029
#define VOICE_STOP_CONT_DTMF      0x002A
#define VOICE_ALL_CALL_STATUS_IND 0x002E
#define VOICE_GET_ALL_CALL_INFO   0x002F
#define VOICE_MANAGE_CALLS        0x0031

/* QMI call states */
enum {
    QS_ORIGINATING = 1, QS_INCOMING, QS_CONVERSATION, QS_CC_IN_PROGRESS,
    QS_ALERTING, QS_HOLD, QS_WAITING, QS_DISCONNECTING, QS_END, QS_SETUP,
};

struct call {
    int id, state, direction, mode, multiparty;
    char number[64];
    int presentation;
};

static pthread_mutex_t call_lock = PTHREAD_MUTEX_INITIALIZER;
static struct call calls[8];
static int ncalls;
static int last_fail_cause = CALL_FAIL_NORMAL;

/* Parse a call-info list: tlv_info holds {n, n x 7-byte call records},
 * tlv_num holds {n, n x (id, presentation, len, digits)}. */
static void parse_calls(const uint8_t *t, size_t tl, uint8_t tlv_info, uint8_t tlv_num,
                        uint8_t tlv_end_reason)
{
    uint16_t vl;
    const uint8_t *ci = qmi_tlv_find(t, tl, tlv_info, &vl);
    ncalls = 0;
    if (ci && vl >= 1) {
        int n = ci[0];
        const uint8_t *p = ci + 1;
        for (int i = 0; i < n && p + 7 <= ci + vl && ncalls < 8; i++, p += 7) {
            struct call *c = &calls[ncalls];
            memset(c, 0, sizeof(*c));
            c->id = p[0];
            c->state = p[1];
            c->direction = p[3];        /* 1 MO, 2 MT */
            c->mode = p[4];
            c->multiparty = p[5];
            if (c->state == QS_END || c->state == QS_DISCONNECTING)
                continue;
            ncalls++;
        }
    }
    const uint8_t *rn = qmi_tlv_find(t, tl, tlv_num, &vl);
    if (rn && vl >= 1) {
        int n = rn[0];
        const uint8_t *p = rn + 1;
        for (int i = 0; i < n && p + 3 <= rn + vl; i++) {
            int id = p[0], pres = p[1], len = p[2];
            if (p + 3 + len > rn + vl)
                break;
            for (int k = 0; k < ncalls; k++) {
                if (calls[k].id == id) {
                    int l = len < 63 ? len : 63;
                    memcpy(calls[k].number, p + 3, l);
                    calls[k].number[l] = 0;
                    calls[k].presentation = pres;
                }
            }
            p += 3 + len;
        }
    }
    /* Call end reasons: {n, n x (id, u16 reason)} */
    const uint8_t *er = tlv_end_reason ? qmi_tlv_find(t, tl, tlv_end_reason, &vl) : NULL;
    if (er && vl >= 4) {
        int reason = qmi_le16(er + 2);
        /* QMI end reasons: 0x01 offline .. 0x18 normal, 0x1a busy (CM reasons
         * vary); map the common cases. */
        switch (reason) {
        case 0x1f: case 0x20: case 0x0c: case 0x2b:
            last_fail_cause = CALL_FAIL_BUSY;
            break;
        case 0x16: case 0x13: case 0x14:
            last_fail_cause = CALL_FAIL_NORMAL;
            break;
        default:
            last_fail_cause = CALL_FAIL_NORMAL;
        }
    }
}

static RIL_CallState ril_call_state(int s, int direction, int others_active)
{
    /* An MT call is first reported in SETUP (or CC_IN_PROGRESS) before
     * INCOMING. Reporting that as DIALING makes the framework see a
     * "phantom" call, which Telecom adds as an unknown call: the in-call
     * UI then shows no answer/decline buttons. */
    if (direction == 2 && (s == QS_SETUP || s == QS_CC_IN_PROGRESS || s == QS_ORIGINATING))
        return others_active ? RIL_CALL_WAITING : RIL_CALL_INCOMING;
    switch (s) {
    case QS_CONVERSATION: return RIL_CALL_ACTIVE;
    case QS_HOLD:         return RIL_CALL_HOLDING;
    case QS_ALERTING:     return RIL_CALL_ALERTING;
    case QS_INCOMING:     return RIL_CALL_INCOMING;
    case QS_WAITING:      return RIL_CALL_WAITING;
    default:              return RIL_CALL_DIALING;
    }
}

static void voice_ind(struct qmi_client *c, uint16_t id, const uint8_t *t, size_t tl, void *user)
{
    (void)c; (void)user;
    if (id != VOICE_ALL_CALL_STATUS_IND)
        return;
    pthread_mutex_lock(&call_lock);
    parse_calls(t, tl, 0x01, 0x10, 0x20);
    int ringing = 0;
    for (int i = 0; i < ncalls; i++)
        if (calls[i].state == QS_INCOMING ||
            (calls[i].direction == 2 && calls[i].state == QS_SETUP))
            ringing = 1;
    pthread_mutex_unlock(&call_lock);
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
    if (ringing)
        RIL_onUnsolicitedResponse(RIL_UNSOL_CALL_RING, NULL, 0);
}

int voice_init(void)
{
    if (qmi_client_open(&q_voice, QMI_VOICE, 10000, voice_ind, NULL))
        return -1;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, VOICE_INDICATION_REGISTER);
    qmi_tlv_add_u8(&q, 0x10, 1);    /* DTMF events */
    qmi_tlv_add_u8(&q, 0x13, 1);    /* call notification events */
    qmi_request(&q_voice, &q, &r, 5000, &e);
    return 0;
}

void voice_get_current_calls(RIL_Token t)
{
    struct qmi_msg r;
    uint16_t e;
    if (!qmi_simple(&q_voice, VOICE_GET_ALL_CALL_INFO, &r, &e) && !e) {
        size_t tl;
        const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
        pthread_mutex_lock(&call_lock);
        parse_calls(tv, tl, 0x10, 0x11, 0);
        pthread_mutex_unlock(&call_lock);
    }
    static RIL_Call rc[8];
    static RIL_Call *pp[8];
    pthread_mutex_lock(&call_lock);
    int n = ncalls;
    for (int i = 0; i < n; i++) {
        struct call *c = &calls[i];
        RIL_Call *x = &rc[i];
        memset(x, 0, sizeof(*x));
        int others = 0;
        for (int k = 0; k < n; k++)
            if (k != i && (calls[k].state == QS_CONVERSATION || calls[k].state == QS_HOLD))
                others = 1;
        x->state = ril_call_state(c->state, c->direction, others);
        x->index = c->id;
        x->toa = c->number[0] == '+' ? 145 : 129;
        x->isMpty = c->multiparty;
        x->isMT = c->direction == 2;
        x->als = 0;
        x->isVoice = 1;
        x->number = c->number[0] ? c->number : NULL;
        x->numberPresentation = c->presentation == 1 ? 1 : 0;   /* restricted */
        x->namePresentation = 2;                                 /* unknown */
        pp[i] = x;
    }
    pthread_mutex_unlock(&call_lock);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, n ? pp : NULL, n * sizeof(RIL_Call *));
}

void voice_dial(RIL_Token t, RIL_Dial *d)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, VOICE_DIAL_CALL);
    qmi_tlv_add(&q, 0x01, d->address, strlen(d->address));
    if (qmi_request(&q_voice, &q, &r, 10000, &e) || e) {
        ALOGE("dial failed: QMI error %u", e);
        last_fail_cause = CALL_FAIL_ERROR_UNSPECIFIED;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static int call_op(uint16_t msg, int id)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, msg);
    qmi_tlv_add_u8(&q, 0x01, id);
    if (qmi_request(&q_voice, &q, &r, 10000, &e))
        return -1;
    if (e)
        ALOGE("voice msg 0x%04x call %d: QMI error %u", msg, id, e);
    return e ? -1 : 0;
}

static int first_call_in(int s1, int s2)
{
    int id = -1;
    pthread_mutex_lock(&call_lock);
    for (int i = 0; i < ncalls; i++)
        if (calls[i].state == s1 || calls[i].state == s2) {
            id = calls[i].id;
            break;
        }
    pthread_mutex_unlock(&call_lock);
    return id;
}

/* Supplementary call management (3GPP 22.030 CHLD). */
static int manage(int sups_type, int call_id)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, VOICE_MANAGE_CALLS);
    qmi_tlv_add_u8(&q, 0x01, sups_type);
    if (call_id >= 0)
        qmi_tlv_add_u8(&q, 0x10, call_id);
    if (qmi_request(&q_voice, &q, &r, 10000, &e))
        return -1;
    if (e)
        ALOGE("manage calls %d: QMI error %u", sups_type, e);
    return e ? -1 : 0;
}

void voice_request(int request, void *data, RIL_Token t)
{
    int r = -1;
    switch (request) {
    case RIL_REQUEST_DIAL:
        voice_dial(t, data);
        return;
    case RIL_REQUEST_ANSWER: {
        int id = first_call_in(QS_INCOMING, QS_WAITING);
        r = id < 0 ? -1 : call_op(VOICE_ANSWER_CALL, id);
        break;
    }
    case RIL_REQUEST_HANGUP:
        r = call_op(VOICE_END_CALL, ((int *)data)[0]);
        break;
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND: {
        /* Reject an incoming call outright; otherwise release held/waiting */
        int id = first_call_in(QS_INCOMING, -1);
        r = id >= 0 ? call_op(VOICE_END_CALL, id) : manage(0x01, -1);
        break;
    }
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        r = manage(0x02, -1);
        break;
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        r = manage(0x03, -1);
        break;
    case RIL_REQUEST_CONFERENCE:
        r = manage(0x05, -1);
        break;
    case RIL_REQUEST_SEPARATE_CONNECTION:
        r = manage(0x04, ((int *)data)[0]);
        break;
    case RIL_REQUEST_UDUB:
        r = manage(0x01, -1);
        break;
    case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
        r = manage(0x06, -1);
        break;
    case RIL_REQUEST_DTMF_START: {
        struct qmi_msg q, rr;
        uint16_t e;
        qmi_msg_init(&q, VOICE_START_CONT_DTMF);
        uint8_t d[2] = { 0xff, (uint8_t)((char *)data)[0] };   /* current call */
        qmi_tlv_add(&q, 0x01, d, 2);
        r = (qmi_request(&q_voice, &q, &rr, 5000, &e) || e) ? -1 : 0;
        break;
    }
    case RIL_REQUEST_DTMF_STOP: {
        struct qmi_msg q, rr;
        uint16_t e;
        qmi_msg_init(&q, VOICE_STOP_CONT_DTMF);
        qmi_tlv_add_u8(&q, 0x01, 0xff);
        r = (qmi_request(&q_voice, &q, &rr, 5000, &e) || e) ? -1 : 0;
        break;
    }
    case RIL_REQUEST_DTMF: {
        struct qmi_msg q, rr;
        uint16_t e;
        qmi_msg_init(&q, VOICE_START_CONT_DTMF);
        uint8_t d[2] = { 0xff, (uint8_t)((char *)data)[0] };
        qmi_tlv_add(&q, 0x01, d, 2);
        r = (qmi_request(&q_voice, &q, &rr, 5000, &e) || e) ? -1 : 0;
        usleep(150000);
        qmi_msg_init(&q, VOICE_STOP_CONT_DTMF);
        qmi_tlv_add_u8(&q, 0x01, 0xff);
        qmi_request(&q_voice, &q, &rr, 5000, &e);
        break;
    }
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &last_fail_cause, sizeof(last_fail_cause));
        return;
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, r ? RIL_E_GENERIC_FAILURE : RIL_E_SUCCESS, NULL, 0);
}
