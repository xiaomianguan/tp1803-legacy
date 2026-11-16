/*
 * TP1803 RIL: Android 6 telephony on the SDM855 modem (MPSS) through QMI
 * over QRTR. Replaces Qualcomm's qcril (Android 10 HIDL only).
 *
 * Radio power, identity, SIM, registration, operator, signal, network time
 * (ril.c, ril_sim.c), voice calls (ril_voice.c), SMS (ril_sms.c) and
 * mobile data (ril_data.c, data_path.c).
 */
#define LOG_TAG "RIL-TP1803"

#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include "ril_qmi.h"

const struct RIL_Env *s_rilenv;
struct qmi_client q_dms, q_nas, q_uim, q_voice, q_wms, q_wds;

/* DMS */
#define DMS_GET_REVISION      0x0023
#define DMS_GET_SERIALS       0x0025
#define DMS_GET_OPER_MODE     0x002D
#define DMS_SET_OPER_MODE     0x002E
/* NAS */
#define NAS_REGISTER_INDS     0x0003
#define NAS_SET_SYS_SEL_PREF  0x0033
#define NAS_GET_SYS_SEL_PREF  0x0034
#define NAS_GET_PLMN_NAME     0x0044
#define NAS_NETWORK_TIME_IND  0x004C
#define NAS_GET_SYS_INFO      0x004D
#define NAS_SYS_INFO_IND      0x004E
#define NAS_GET_SIG_INFO      0x004F
#define NAS_SIG_INFO_IND      0x0051

static RIL_RadioState radio_state = RADIO_STATE_UNAVAILABLE;
static int pref_network = 9;      /* NETWORK_MODE_LTE_GSM_WCDMA */

static void log_android(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LOG_PRI_VA(ANDROID_LOG_INFO, LOG_TAG, fmt, ap);
    va_end(ap);
}

static void set_radio_state(RIL_RadioState s)
{
    if (s == radio_state)
        return;
    radio_state = s;
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
}

void ril_sim_changed(void)
{
    /* Registration depends on the SIM; let the framework re-poll. */
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
}

/* ------------------------------------------------------------------ DMS */

static int dms_string(uint16_t msg, uint8_t tlv, char *out, size_t outlen)
{
    struct qmi_msg r;
    uint16_t e;
    if (qmi_simple(&q_dms, msg, &r, &e) || e)
        return -1;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(t, tl, tlv, &vl);
    if (!v)
        return -1;
    if (vl >= outlen)
        vl = outlen - 1;
    memcpy(out, v, vl);
    out[vl] = 0;
    return 0;
}

static int set_oper_mode(int mode)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, DMS_SET_OPER_MODE);
    qmi_tlv_add_u8(&q, 0x01, mode);
    if (qmi_request(&q_dms, &q, &r, 15000, &e))
        return -1;
    if (e && e != 0x1a)                         /* NO_EFFECT is fine */
        ALOGE("set operating mode %d: QMI error %u", mode, e);
    return e && e != 0x1a ? -1 : 0;
}

/* ------------------------------------------------------------------ NAS */

struct sys_info {
    int valid;
    int rat;            /* RIL radio tech */
    int srv;            /* QMI service status: 0 none 1 limited 2 full */
    int domain;         /* 1 CS 2 PS 3 CS+PS */
    int roaming;
    int lac;            /* LAC or TAC */
    unsigned cid;
    char mcc[4], mnc[4];
    int reject_cause;
};

/* Common part of GSM/WCDMA/LTE sys info TLVs (see libqmi). */
static void parse_common(const uint8_t *p, uint16_t l, struct sys_info *s)
{
    if (l < 29)
        return;
    if (p[0]) s->domain = p[1];
    if (p[4]) s->roaming = p[5] == 1;
    if (p[8]) s->lac = qmi_le16(p + 9);
    if (p[11]) s->cid = qmi_le32(p + 12);
    if (p[16]) s->reject_cause = p[18];
    if (p[19]) {
        memcpy(s->mcc, p + 20, 3);
        s->mcc[3] = 0;
        memcpy(s->mnc, p + 23, 3);
        s->mnc[p[25] == 0xff ? 2 : 3] = 0;
    }
}

static int get_sys_info(struct sys_info *s)
{
    struct qmi_msg r;
    uint16_t e;
    memset(s, 0, sizeof(*s));
    s->lac = -1;
    if (qmi_simple(&q_nas, NAS_GET_SYS_INFO, &r, &e) || e)
        return -1;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    /* Prefer LTE, then WCDMA, then GSM: (status TLV, info TLV, RIL rat) */
    static const struct { uint8_t st, info; int rat; } rats[] = {
        { 0x14, 0x19, RADIO_TECH_LTE },
        { 0x13, 0x18, RADIO_TECH_UMTS },
        { 0x12, 0x17, RADIO_TECH_GSM },
    };
    int best = -1, best_srv = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t *st = qmi_tlv_find(t, tl, rats[i].st, &vl);
        if (st && vl >= 1 && st[0] > best_srv) {
            best = i;
            best_srv = st[0];
            if (st[0] == 2)
                break;
        }
    }
    if (best < 0)
        return 0;
    s->srv = best_srv;
    s->rat = rats[best].rat;
    const uint8_t *info = qmi_tlv_find(t, tl, rats[best].info, &vl);
    if (info) {
        parse_common(info, vl, s);
        if (rats[best].rat == RADIO_TECH_LTE && vl >= 32 && info[29])
            s->lac = qmi_le16(info + 30);        /* TAC */
        if (rats[best].rat == RADIO_TECH_UMTS && vl >= 33 && info[31] && info[32])
            s->rat = RADIO_TECH_HSPA;            /* HS indication */
    }
    s->valid = 1;
    return 0;
}

static void registration_state(RIL_Token t, int data)
{
    struct sys_info s;
    get_sys_info(&s);
    int reg;
    int cs = s.domain == 1 || s.domain == 3;
    int ps = s.domain == 2 || s.domain == 3;
    if (s.srv == 2 && (data ? ps : (cs || (ps && s.rat == RADIO_TECH_LTE))))
        reg = s.roaming ? 5 : 1;
    else if (s.srv == 1 || s.srv == 3)
        reg = data ? 2 : 12;        /* searching, emergency calls available */
    else if (s.reject_cause)
        reg = 3;
    else
        reg = data ? 0 : 10;

    static char regs[16], lac[16], cid[16], rat[8], cause[8];
    static char *resp[15];
    memset(resp, 0, sizeof(resp));
    snprintf(regs, sizeof(regs), "%d", reg);
    resp[0] = regs;
    if (s.lac >= 0) {
        snprintf(lac, sizeof(lac), "%04x", s.lac);
        snprintf(cid, sizeof(cid), "%08x", s.cid);
        resp[1] = lac;
        resp[2] = cid;
    }
    snprintf(rat, sizeof(rat), "%d", s.valid ? s.rat : RADIO_TECH_UNKNOWN);
    resp[3] = rat;
    if (data) {
        static char maxcalls[] = "1";
        snprintf(cause, sizeof(cause), "%d", reg == 3 ? s.reject_cause : 0);
        resp[4] = cause;
        resp[5] = maxcalls;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, 6 * sizeof(char *));
    } else {
        snprintf(cause, sizeof(cause), "%d", reg == 3 ? s.reject_cause : 0);
        resp[13] = cause;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, 15 * sizeof(char *));
    }
}

static void decode_name(int enc, const uint8_t *b, int len, char *out, int outmax)
{
    out[0] = 0;
    if (len <= 0)
        return;
    if (enc == 1) {                             /* UCS2 LE */
        int n = 0;
        for (int i = 0; i + 1 < len && n < outmax - 1; i += 2) {
            uint16_t c = b[i] | b[i + 1] << 8;
            out[n++] = c < 0x80 ? c : '?';
        }
        out[n] = 0;
    } else {
        /* 0: GSM default alphabet, unpacked (ASCII for names) */
        int n = len < outmax - 1 ? len : outmax - 1;
        memcpy(out, b, n);
        out[n] = 0;
    }
}

static void plmn_names(const char *mcc, const char *mnc, char *lng, char *shrt, int max)
{
    lng[0] = shrt[0] = 0;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, NAS_GET_PLMN_NAME);
    uint16_t m = atoi(mcc), n = atoi(mnc);
    uint8_t plmn[4] = { m & 0xff, m >> 8, n & 0xff, n >> 8 };
    qmi_tlv_add(&q, 0x01, plmn, 4);
    if (qmi_request(&q_nas, &q, &r, 5000, &e) || e)
        return;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *p = qmi_tlv_find(t, tl, 0x10, &vl);
    if (!p || vl < 2)
        return;
    const uint8_t *end = p + vl;
    int spn_len = p[1];
    p += 2 + spn_len;                           /* skip SPN */
    if (p + 4 > end)
        return;
    int s_enc = p[0], s_len = p[3];
    if (p + 4 + s_len > end)
        return;
    decode_name(s_enc, p + 4, s_len, shrt, max);
    p += 4 + s_len;
    if (p + 4 > end)
        return;
    int l_enc = p[0], l_len = p[3];
    if (p + 4 + l_len <= end)
        decode_name(l_enc, p + 4, l_len, lng, max);
}

static void operator_req(RIL_Token t)
{
    struct sys_info s;
    get_sys_info(&s);
    static char lng[64], shrt[32], num[8];
    static char *resp[3];
    if (s.srv != 2 || !s.mcc[0]) {
        resp[0] = resp[1] = resp[2] = NULL;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
        return;
    }
    snprintf(num, sizeof(num), "%s%s", s.mcc, s.mnc);
    plmn_names(s.mcc, s.mnc, lng, shrt, sizeof(shrt));
    if (!lng[0])
        snprintf(lng, sizeof(lng), "%s", shrt[0] ? shrt : num);
    if (!shrt[0])
        snprintf(shrt, sizeof(shrt), "%s", lng);
    resp[0] = lng;
    resp[1] = shrt;
    resp[2] = num;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
}

static void fill_signal(const uint8_t *t, size_t tl, RIL_SignalStrength_v10 *ss)
{
    memset(ss, 0, sizeof(*ss));
    ss->GW_SignalStrength.signalStrength = 99;
    ss->GW_SignalStrength.bitErrorRate = -1;
    ss->CDMA_SignalStrength.dbm = -1;
    ss->CDMA_SignalStrength.ecio = -1;
    ss->EVDO_SignalStrength.dbm = -1;
    ss->EVDO_SignalStrength.ecio = -1;
    ss->EVDO_SignalStrength.signalNoiseRatio = -1;
    ss->LTE_SignalStrength.signalStrength = 99;
    ss->LTE_SignalStrength.rsrp = INT_MAX;
    ss->LTE_SignalStrength.rsrq = INT_MAX;
    ss->LTE_SignalStrength.rssnr = INT_MAX;
    ss->LTE_SignalStrength.cqi = INT_MAX;
    ss->LTE_SignalStrength.timingAdvance = INT_MAX;
    ss->TD_SCDMA_SignalStrength.rscp = INT_MAX;

    uint16_t vl;
    const uint8_t *v;
    int rssi = 0;
    if ((v = qmi_tlv_find(t, tl, 0x13, &vl)) && vl >= 1)        /* WCDMA */
        rssi = (int8_t)v[0];
    else if ((v = qmi_tlv_find(t, tl, 0x12, &vl)) && vl >= 1)   /* GSM */
        rssi = (int8_t)v[0];
    if (rssi) {
        int asu = (rssi + 113) / 2;
        ss->GW_SignalStrength.signalStrength = asu < 0 ? 0 : asu > 31 ? 31 : asu;
    }
    if ((v = qmi_tlv_find(t, tl, 0x14, &vl)) && vl >= 6) {      /* LTE */
        int lrssi = (int8_t)v[0];
        int asu = (lrssi + 113) / 2;
        ss->LTE_SignalStrength.signalStrength = asu < 0 ? 0 : asu > 31 ? 31 : asu;
        ss->LTE_SignalStrength.rsrq = -(int8_t)v[1];
        ss->LTE_SignalStrength.rsrp = -(int16_t)qmi_le16(v + 2);
        ss->LTE_SignalStrength.rssnr = (int16_t)qmi_le16(v + 4);  /* 0.1 dB */
    }
}

static void signal_req(RIL_Token t)
{
    struct qmi_msg r;
    uint16_t e;
    RIL_SignalStrength_v10 ss;
    if (qmi_simple(&q_nas, NAS_GET_SIG_INFO, &r, &e) || e) {
        fill_signal(NULL, 0, &ss);
    } else {
        size_t tl;
        const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
        fill_signal(tv, tl, &ss);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &ss, sizeof(ss));
}

static uint16_t mode_pref_for(int net)
{
    switch (net) {
    case 1:  return 0x04;           /* GSM only */
    case 2:  return 0x08;           /* WCDMA only */
    case 0:
    case 3:  return 0x0c;           /* GSM/WCDMA */
    case 11: return 0x10;           /* LTE only */
    case 12: return 0x18;           /* LTE/WCDMA */
    default: return 0x1c;           /* LTE/GSM/WCDMA */
    }
}

static int set_mode_pref(int net)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, NAS_SET_SYS_SEL_PREF);
    qmi_tlv_add_u16(&q, 0x11, mode_pref_for(net));
    qmi_tlv_add_u8(&q, 0x17, 1);                /* permanent */
    if (qmi_request(&q_nas, &q, &r, 5000, &e))
        return -1;
    return e ? -1 : 0;
}

static int set_selection_auto(void)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, NAS_SET_SYS_SEL_PREF);
    uint8_t sel[5] = { 0, 0, 0, 0, 0 };          /* automatic */
    qmi_tlv_add(&q, 0x16, sel, 5);
    if (qmi_request(&q_nas, &q, &r, 5000, &e))
        return -1;
    return e ? -1 : 0;
}

static void nas_ind(struct qmi_client *c, uint16_t id, const uint8_t *t, size_t tl, void *user)
{
    (void)c; (void)user;
    switch (id) {
    case NAS_SYS_INFO_IND:
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
        break;
    case NAS_SIG_INFO_IND:
        /* Indications can omit RATs; signal_poller() reports instead. */
        break;
    case NAS_NETWORK_TIME_IND: {
        uint16_t vl;
        const uint8_t *u = qmi_tlv_find(t, tl, 0x01, &vl);
        if (!u || vl < 7)
            break;
        const uint8_t *tz = qmi_tlv_find(t, tl, 0x10, &vl);
        const uint8_t *dst = qmi_tlv_find(t, tl, 0x11, &vl);
        int tzq = tz ? (int8_t)tz[0] : 0;
        char nitz[64];
        snprintf(nitz, sizeof(nitz), "%02d/%02d/%02d,%02d:%02d:%02d%c%d,%d",
                 qmi_le16(u) % 100, u[2], u[3], u[4], u[5], u[6],
                 tzq < 0 ? '-' : '+', tzq < 0 ? -tzq : tzq, dst ? dst[0] : 0);
        ALOGI("NITZ %s", nitz);
        RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED, nitz, strlen(nitz));
        break;
    }
    }
}

/* Push signal changes to the framework (it stops polling after the first
 * unsolicited report). Runs only while the CPU is awake. */
static void *signal_poller(void *arg)
{
    (void)arg;
    RIL_SignalStrength_v10 last;
    memset(&last, 0xff, sizeof(last));
    for (;;) {
        sleep(5);
        if (radio_state != RADIO_STATE_ON)
            continue;
        struct qmi_msg r;
        uint16_t e;
        if (qmi_simple(&q_nas, NAS_GET_SIG_INFO, &r, &e) || e)
            continue;
        size_t tl;
        const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
        RIL_SignalStrength_v10 ss;
        fill_signal(tv, tl, &ss);
        if (memcmp(&ss, &last, sizeof(ss))) {
            last = ss;
            RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &ss, sizeof(ss));
        }
    }
    return NULL;
}

static void nas_register(void)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, NAS_REGISTER_INDS);
    qmi_tlv_add_u8(&q, 0x13, 0);                /* legacy serving system: off */
    qmi_tlv_add_u8(&q, 0x17, 1);                /* network time */
    qmi_tlv_add_u8(&q, 0x18, 1);                /* system info */
    qmi_tlv_add_u8(&q, 0x19, 0);                /* signal info: polled instead */
    qmi_request(&q_nas, &q, &r, 5000, &e);
    if (e)
        ALOGW("NAS register indications: QMI error %u", e);
}

/* ------------------------------------------------------------------ RIL */

static void ok(RIL_Token t) { RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0); }
static void fail(RIL_Token t) { RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0); }
static void unsupported(RIL_Token t) { RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0); }

static void onRequest(int request, void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;
    if (radio_state == RADIO_STATE_UNAVAILABLE &&
        request != RIL_REQUEST_GET_SIM_STATUS) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
    switch (request) {
    /* --- radio / identity --- */
    case RIL_REQUEST_RADIO_POWER: {
        int on = data ? ((int *)data)[0] : 0;
        if (set_oper_mode(on ? 0 : 1)) {
            fail(t);
            return;
        }
        set_radio_state(on ? RADIO_STATE_ON : RADIO_STATE_OFF);
        ok(t);
        return;
    }
    case RIL_REQUEST_SHUTDOWN:
        set_oper_mode(1);
        set_radio_state(RADIO_STATE_OFF);
        ok(t);
        return;
    case RIL_REQUEST_GET_IMEI: {
        static char imei[32];
        if (dms_string(DMS_GET_SERIALS, 0x11, imei, sizeof(imei)))
            fail(t);
        else
            RIL_onRequestComplete(t, RIL_E_SUCCESS, imei, sizeof(char *));
        return;
    }
    case RIL_REQUEST_GET_IMEISV: {
        static char sv[] = "00";
        RIL_onRequestComplete(t, RIL_E_SUCCESS, sv, sizeof(char *));
        return;
    }
    case RIL_REQUEST_DEVICE_IDENTITY: {
        static char imei[32], sv[] = "00";
        static char *resp[4];
        dms_string(DMS_GET_SERIALS, 0x11, imei, sizeof(imei));
        resp[0] = imei;
        resp[1] = sv;
        resp[2] = resp[3] = NULL;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
        return;
    }
    case RIL_REQUEST_BASEBAND_VERSION: {
        static char rev[128];
        if (dms_string(DMS_GET_REVISION, 0x01, rev, sizeof(rev)))
            fail(t);
        else
            RIL_onRequestComplete(t, RIL_E_SUCCESS, rev, sizeof(char *));
        return;
    }

    /* --- SIM --- */
    case RIL_REQUEST_GET_SIM_STATUS:
        sim_get_status(t);
        return;
    case RIL_REQUEST_SIM_IO:
        sim_io(t, data);
        return;
    case RIL_REQUEST_GET_IMSI:
        sim_get_imsi(t);
        return;
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_ENTER_SIM_PUK:
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_ENTER_SIM_PUK2:
    case RIL_REQUEST_CHANGE_SIM_PIN:
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        sim_pin_op(t, request, data);
        return;
    case RIL_REQUEST_QUERY_FACILITY_LOCK:
        sim_query_facility(t, data);
        return;
    case RIL_REQUEST_SET_FACILITY_LOCK:
        sim_set_facility(t, data);
        return;

    /* --- network --- */
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        registration_state(t, 0);
        return;
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
        registration_state(t, 1);
        return;
    case RIL_REQUEST_OPERATOR:
        operator_req(t);
        return;
    case RIL_REQUEST_SIGNAL_STRENGTH:
        signal_req(t);
        return;
    case RIL_REQUEST_VOICE_RADIO_TECH: {
        struct sys_info s;
        get_sys_info(&s);
        int rat = s.valid ? s.rat : RADIO_TECH_UNKNOWN;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &rat, sizeof(rat));
        return;
    }
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE: {
        int mode = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &mode, sizeof(mode));
        return;
    }
    case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
        set_selection_auto() ? fail(t) : ok(t);
        return;
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &pref_network, sizeof(pref_network));
        return;
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE: {
        int net = data ? ((int *)data)[0] : 9;
        if (!q_nas.port) {
            /* Sent on connect, before NAS is open: init applies it. */
            pref_network = net;
            ok(t);
            return;
        }
        if (set_mode_pref(net)) {
            fail(t);
            return;
        }
        pref_network = net;
        ok(t);
        return;
    }
    case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
    case RIL_REQUEST_GET_CELL_INFO_LIST:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
    case RIL_REQUEST_IMS_REGISTRATION_STATE: {
        int resp[2] = { 0, 1 };                     /* not registered, 3GPP */
        RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
        return;
    }

    /* --- calls --- */
    case RIL_REQUEST_GET_CURRENT_CALLS:
        voice_get_current_calls(t);
        return;
    case RIL_REQUEST_DIAL:
    case RIL_REQUEST_ANSWER:
    case RIL_REQUEST_HANGUP:
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
    case RIL_REQUEST_CONFERENCE:
    case RIL_REQUEST_SEPARATE_CONNECTION:
    case RIL_REQUEST_UDUB:
    case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
    case RIL_REQUEST_DTMF:
    case RIL_REQUEST_DTMF_START:
    case RIL_REQUEST_DTMF_STOP:
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        voice_request(request, data, t);
        return;
    case RIL_REQUEST_GET_MUTE: {
        int m = 0;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &m, sizeof(m));
        return;
    }
    case RIL_REQUEST_QUERY_CLIP: {
        int m = 1;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &m, sizeof(m));
        return;
    }

    /* --- SMS --- */
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
    case RIL_REQUEST_GET_SMSC_ADDRESS:
    case RIL_REQUEST_SET_SMSC_ADDRESS:
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
    case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        if (!q_wms.port) {
            RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
            return;
        }
        sms_request(request, data, datalen, t);
        return;

    /* --- data --- */
    case RIL_REQUEST_SETUP_DATA_CALL:
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
    case RIL_REQUEST_DATA_CALL_LIST:
    case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
        data_request(request, data, datalen, t);
        return;

    /* --- accepted without effect --- */
    case RIL_REQUEST_SCREEN_STATE:
    case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
    case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_LOCATION_UPDATES:
    case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
    case RIL_REQUEST_SET_MUTE:
    case RIL_REQUEST_SET_TTY_MODE:
    case RIL_REQUEST_SET_DATA_PROFILE:
        ok(t);
        return;

    default:
        unsupported(t);
    }
}

static RIL_RadioState currentState(void) { return radio_state; }
static int onSupports(int requestCode) { (void)requestCode; return 1; }
static void onCancel(RIL_Token t) { (void)t; }
static const char *getVersion(void) { return "TP1803 QMI RIL 1.0"; }

static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion,
};

static void *init_thread(void *arg)
{
    (void)arg;
    /* The modem may still be booting: retry until its services appear. */
    for (int tries = 0;; tries++) {
        if (!q_dms.port && qmi_client_open(&q_dms, QMI_DMS, 5000, NULL, NULL))
            goto again;
        if (!q_nas.port && qmi_client_open(&q_nas, QMI_NAS, 5000, nas_ind, NULL))
            goto again;
        if (!q_uim.port && sim_init())
            goto again;
        if (!q_voice.port && voice_init())
            goto again;
        if (!q_wms.port && sms_init())
            goto again;
        break;
again:
        ALOGW("modem services not ready (attempt %d)", tries + 1);
        sleep(2);
    }
    nas_register();
    set_mode_pref(pref_network);
    data_init();
    pthread_t sp;
    pthread_create(&sp, NULL, signal_poller, NULL);
    pthread_detach(sp);
    /* Start powered down; the framework turns the radio on. */
    set_oper_mode(1);
    ALOGI("QMI RIL ready");
    set_radio_state(RADIO_STATE_OFF);
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
    return NULL;
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    (void)argc; (void)argv;
    s_rilenv = env;
    qmi_log = log_android;
    /* libril writes to the framework's socket; if the phone process goes
     * away mid-response, that must not kill rild. */
    signal(SIGPIPE, SIG_IGN);
    pthread_t th;
    pthread_create(&th, NULL, init_thread, NULL);
    pthread_detach(th);
    return &s_callbacks;
}
