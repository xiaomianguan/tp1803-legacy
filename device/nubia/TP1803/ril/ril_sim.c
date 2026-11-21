/*
 * SIM side of the TP1803 QMI RIL: card status, SIM file I/O, IMSI, PIN.
 * QMI UIM message formats follow libqmi's qmi-service-uim.json.
 */
#include "ril_qmi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UIM messages */
#define UIM_READ_TRANSPARENT  0x0020
#define UIM_READ_RECORD       0x0021
#define UIM_WRITE_TRANSPARENT 0x0022
#define UIM_WRITE_RECORD      0x0023
#define UIM_GET_FILE_ATTRS    0x0024
#define UIM_SET_PIN_PROT      0x0025
#define UIM_VERIFY_PIN        0x0026
#define UIM_UNBLOCK_PIN       0x0027
#define UIM_CHANGE_PIN        0x0028
#define UIM_REGISTER_EVENTS   0x002E
#define UIM_GET_CARD_STATUS   0x002F
#define UIM_STATUS_CHANGE_IND 0x0032

/* session types */
#define SESSION_PRIMARY_GW  0x00
#define SESSION_CARD_SLOT1  0x06

struct uim_app {
    int type;               /* QMI: 1 SIM 2 USIM 3 RUIM 4 CSIM 5 ISIM */
    int state;              /* QMI app state */
    int perso_state;
    uint8_t aid[32];
    int aid_len;
    int pin1_state, pin1_retries, puk1_retries;
    int pin2_state, pin2_retries, puk2_retries;
    int univ_pin;
};

static struct {
    int valid;
    int card_state;         /* 0 absent 1 present 2 error */
    int upin_state;
    int gw_index;           /* index of primary GW app (slot<<8|app) or -1 */
    int num_apps;
    struct uim_app apps[8];
} card;

static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;

static int parse_card_status(const uint8_t *tlvs, size_t len)
{
    uint16_t vl;
    const uint8_t *p = qmi_tlv_find(tlvs, len, 0x10, &vl);
    if (!p || vl < 9)
        return -1;
    const uint8_t *end = p + vl;
    uint16_t gw = qmi_le16(p);
    p += 8;
    int slots = *p++;
    memset(&card, 0, sizeof(card));
    card.gw_index = gw == 0xffff ? -1 : gw;
    if (slots < 1)
        return -1;
    /* slot 1 only */
    if (p + 6 > end)
        return -1;
    card.card_state = p[0];
    card.upin_state = p[1];
    int napps = p[5];
    p += 6;
    for (int i = 0; i < napps && p + 7 <= end; i++) {
        struct uim_app *a = i < 8 ? &card.apps[i] : NULL;
        int aid_len = p[6];
        const uint8_t *q = p + 7 + aid_len;
        if (q + 7 > end)            /* univ_pin + pin1/pin2 state & retries */
            break;
        if (a) {
            a->type = p[0];
            a->state = p[1];
            a->perso_state = p[2];
            a->aid_len = aid_len > 32 ? 32 : aid_len;
            memcpy(a->aid, p + 7, a->aid_len);
            a->univ_pin = q[0];
            a->pin1_state = q[1];
            a->pin1_retries = q[2];
            a->puk1_retries = q[3];
            a->pin2_state = q[4];
            a->pin2_retries = q[5];
            a->puk2_retries = q[6];
            card.num_apps = i + 1;
        }
        p = q + 7;
    }
    card.valid = 1;
    return 0;
}

int sim_refresh(void)
{
    struct qmi_msg r;
    uint16_t e;
    if (qmi_simple(&q_uim, UIM_GET_CARD_STATUS, &r, &e) || e)
        return -1;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&r, &tl);
    pthread_mutex_lock(&sim_lock);
    int ret = parse_card_status(t, tl);
    pthread_mutex_unlock(&sim_lock);
    return ret;
}

static struct uim_app *gw_app(void)
{
    if (card.gw_index < 0)
        return NULL;
    int app = card.gw_index & 0xff;
    return app < card.num_apps ? &card.apps[app] : NULL;
}

/* QMI app/pin states -> RIL */
static RIL_AppType ril_app_type(int t)
{
    switch (t) {
    case 1: return RIL_APPTYPE_SIM;
    case 2: return RIL_APPTYPE_USIM;
    case 3: return RIL_APPTYPE_RUIM;
    case 4: return RIL_APPTYPE_CSIM;
    case 5: return RIL_APPTYPE_ISIM;
    default: return RIL_APPTYPE_UNKNOWN;
    }
}

static RIL_AppState ril_app_state(int s)
{
    switch (s) {
    case 1: return RIL_APPSTATE_DETECTED;
    case 2: return RIL_APPSTATE_PIN;
    case 3: return RIL_APPSTATE_PUK;
    case 4: return RIL_APPSTATE_SUBSCRIPTION_PERSO;
    case 5: return RIL_APPSTATE_PUK;        /* PIN1 blocked */
    case 7: return RIL_APPSTATE_READY;
    default: return RIL_APPSTATE_UNKNOWN;
    }
}

static RIL_PinState ril_pin_state(int s)
{
    switch (s) {
    case 1: return RIL_PINSTATE_ENABLED_NOT_VERIFIED;
    case 2: return RIL_PINSTATE_ENABLED_VERIFIED;
    case 3: return RIL_PINSTATE_DISABLED;
    case 4: return RIL_PINSTATE_ENABLED_BLOCKED;
    case 5: return RIL_PINSTATE_ENABLED_PERM_BLOCKED;
    default: return RIL_PINSTATE_UNKNOWN;
    }
}

int sim_ready(void)
{
    pthread_mutex_lock(&sim_lock);
    struct uim_app *a = gw_app();
    int ready = card.valid && card.card_state == 1 && a && a->state == 7;
    pthread_mutex_unlock(&sim_lock);
    return ready;
}

int sim_present(void)
{
    return card.valid && card.card_state == 1;
}

void sim_get_status(RIL_Token t)
{
    if (!q_uim.port) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
    sim_refresh();
    static RIL_CardStatus_v6 cs;
    static char aids[8][65];
    pthread_mutex_lock(&sim_lock);
    memset(&cs, 0, sizeof(cs));
    cs.card_state = !card.valid ? RIL_CARDSTATE_ABSENT
                  : card.card_state == 1 ? RIL_CARDSTATE_PRESENT
                  : card.card_state == 2 ? RIL_CARDSTATE_ERROR : RIL_CARDSTATE_ABSENT;
    cs.universal_pin_state = ril_pin_state(card.upin_state);
    cs.gsm_umts_subscription_app_index = (card.gw_index >= 0) ? (card.gw_index & 0xff) : -1;
    cs.cdma_subscription_app_index = -1;
    cs.ims_subscription_app_index = -1;
    cs.num_applications = card.num_apps;
    for (int i = 0; i < card.num_apps && i < RIL_CARD_MAX_APPS; i++) {
        struct uim_app *a = &card.apps[i];
        RIL_AppStatus *s = &cs.applications[i];
        s->app_type = ril_app_type(a->type);
        s->app_state = ril_app_state(a->state);
        s->perso_substate = RIL_PERSOSUBSTATE_UNKNOWN;
        for (int k = 0; k < a->aid_len; k++)
            sprintf(aids[i] + 2 * k, "%02x", a->aid[k]);
        aids[i][2 * a->aid_len] = 0;
        s->aid_ptr = aids[i];
        s->app_label_ptr = NULL;
        s->pin1_replaced = a->univ_pin;
        s->pin1 = ril_pin_state(a->pin1_state);
        s->pin2 = ril_pin_state(a->pin2_state);
        if (a->type == 5)
            cs.ims_subscription_app_index = i;
    }
    pthread_mutex_unlock(&sim_lock);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &cs, sizeof(cs));
}

/* ---- file access ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex2bin(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s && s[0] && s[1] && n < max) {
        int h = hexval(s[0]), l = hexval(s[1]);
        if (h < 0 || l < 0)
            break;
        out[n++] = h << 4 | l;
        s += 2;
    }
    return n;
}

static char *bin2hex(const uint8_t *b, int n)
{
    char *s = malloc(2 * n + 1);
    for (int i = 0; i < n; i++)
        sprintf(s + 2 * i, "%02x", b[i]);
    s[2 * n] = 0;
    return s;
}

/* Session + file id TLVs. Files directly under MF (e.g. EF_ICCID 2FE2,
 * EF_DIR 2F00) are read through the card session, everything else through
 * the primary GW provisioning (USIM/SIM) session. */
static void add_file_tlvs(struct qmi_msg *m, int fileid, const char *path)
{
    uint8_t pb[16];
    int pl = hex2bin(path, pb, sizeof(pb));
    if (pl == 0 || (pl % 2)) {
        /* Default paths as the framework would use them */
        pb[0] = 0x3F; pb[1] = 0x00; pl = 2;
        if ((fileid & 0xff00) != 0x2f00) {
            pb[2] = 0x7F; pb[3] = 0xFF; pl = 4;
        }
    }
    int card_level = (pl == 2);         /* path is just MF */
    uint8_t sess[2] = { card_level ? SESSION_CARD_SLOT1 : SESSION_PRIMARY_GW, 0 };
    qmi_tlv_add(m, 0x01, sess, 2);
    uint8_t f[3 + 16];
    f[0] = fileid & 0xff;
    f[1] = fileid >> 8;
    f[2] = pl;
    /* path is a sequence of 16-bit ids, little endian each */
    for (int i = 0; i + 1 < pl; i += 2) {
        f[3 + i] = pb[i + 1];
        f[4 + i] = pb[i];
    }
    qmi_tlv_add(m, 0x02, f, 3 + pl);
}

static void card_result(const struct qmi_msg *r, int *sw1, int *sw2)
{
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(r, &tl);
    uint16_t vl;
    const uint8_t *cr = qmi_tlv_find(t, tl, 0x10, &vl);
    *sw1 = cr && vl >= 2 ? cr[0] : 0x6f;
    *sw2 = cr && vl >= 2 ? cr[1] : 0x00;
}

static void sim_io_complete(RIL_Token t, int err, int sw1, int sw2, char *hex)
{
    RIL_SIM_IO_Response resp = { sw1, sw2, hex };
    RIL_onRequestComplete(t, err ? RIL_E_GENERIC_FAILURE : RIL_E_SUCCESS, &resp, sizeof(resp));
    free(hex);
}

/* Build the 2G-style GET RESPONSE the framework's IccFileHandler parses
 * from QMI file attributes. */
static char *get_response_from_attrs(const uint8_t *a, uint16_t len)
{
    if (len < 13)
        return NULL;
    uint16_t size = qmi_le16(a);
    uint16_t fid = qmi_le16(a + 2);
    uint8_t type = a[4];        /* 0 transparent 1 cyclic 2 linear 3 DF 4 MF */
    uint16_t rec_size = qmi_le16(a + 5);
    uint8_t g[15] = {0};
    g[2] = size >> 8;
    g[3] = size & 0xff;
    g[4] = fid >> 8;
    g[5] = fid & 0xff;
    g[6] = (type == 3) ? 2 : (type == 4) ? 1 : 4;   /* DF / MF / EF */
    g[13] = (type == 0) ? 0 : (type == 1) ? 3 : 1;  /* transparent/cyclic/linear */
    g[14] = rec_size & 0xff;
    return bin2hex(g, 15);
}

void sim_io(RIL_Token t, RIL_SIM_IO_v6 *io)
{
    struct qmi_msg q, r;
    uint16_t e;
    int sw1 = 0x6f, sw2 = 0;
    switch (io->command) {
    case 0xC0: {                                    /* GET RESPONSE */
        qmi_msg_init(&q, UIM_GET_FILE_ATTRS);
        add_file_tlvs(&q, io->fileid, io->path);
        if (qmi_request(&q_uim, &q, &r, 5000, &e) || e) {
            sim_io_complete(t, 0, 0x6a, 0x82, NULL);  /* file not found */
            return;
        }
        card_result(&r, &sw1, &sw2);
        size_t tl;
        const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
        uint16_t vl;
        const uint8_t *a = qmi_tlv_find(tv, tl, 0x11, &vl);
        char *hex = a ? get_response_from_attrs(a, vl) : NULL;
        sim_io_complete(t, !hex, hex ? 0x90 : sw1, hex ? 0x00 : sw2, hex);
        return;
    }
    case 0xB0:                                      /* READ BINARY */
    case 0xB2: {                                    /* READ RECORD */
        int binary = io->command == 0xB0;
        qmi_msg_init(&q, binary ? UIM_READ_TRANSPARENT : UIM_READ_RECORD);
        add_file_tlvs(&q, io->fileid, io->path);
        uint8_t rd[4];
        if (binary) {
            uint16_t off = io->p1 << 8 | io->p2;
            rd[0] = off & 0xff; rd[1] = off >> 8;
        } else {
            rd[0] = io->p1 & 0xff; rd[1] = 0;
        }
        rd[2] = io->p3 & 0xff; rd[3] = io->p3 >> 8;
        qmi_tlv_add(&q, 0x03, rd, 4);
        if (qmi_request(&q_uim, &q, &r, 5000, &e)) {
            sim_io_complete(t, 1, 0x6f, 0, NULL);
            return;
        }
        card_result(&r, &sw1, &sw2);
        size_t tl;
        const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
        uint16_t vl;
        const uint8_t *d = qmi_tlv_find(tv, tl, 0x11, &vl);
        char *hex = (d && vl >= 2) ? bin2hex(d + 2, qmi_le16(d)) : NULL;
        if (e && !hex) {
            sim_io_complete(t, 0, sw1 == 0x90 ? 0x6a : sw1, sw1 == 0x90 ? 0x82 : sw2, NULL);
            return;
        }
        sim_io_complete(t, 0, sw1, sw2, hex);
        return;
    }
    case 0xD6:                                      /* UPDATE BINARY */
    case 0xDC: {                                    /* UPDATE RECORD */
        int binary = io->command == 0xD6;
        uint8_t data[256];
        int dl = hex2bin(io->data, data, sizeof(data));
        qmi_msg_init(&q, binary ? UIM_WRITE_TRANSPARENT : UIM_WRITE_RECORD);
        add_file_tlvs(&q, io->fileid, io->path);
        uint8_t w[4 + 256];
        if (binary) {
            uint16_t off = io->p1 << 8 | io->p2;
            w[0] = off & 0xff; w[1] = off >> 8;
        } else {
            w[0] = io->p1 & 0xff; w[1] = 0;
        }
        w[2] = dl & 0xff; w[3] = dl >> 8;
        memcpy(w + 4, data, dl);
        qmi_tlv_add(&q, 0x03, w, 4 + dl);
        if (qmi_request(&q_uim, &q, &r, 5000, &e)) {
            sim_io_complete(t, 1, 0x6f, 0, NULL);
            return;
        }
        card_result(&r, &sw1, &sw2);
        sim_io_complete(t, 0, e ? sw1 : 0x90, e ? sw2 : 0, NULL);
        return;
    }
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
    }
}

/* ICCID from EF_ICCID (2FE2 under MF): swapped-nibble BCD, 0xF padded.
 * Returns 0 and a NUL-terminated digit string on success. */
int sim_read_iccid(char *out, size_t n)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, UIM_READ_TRANSPARENT);
    add_file_tlvs(&q, 0x2FE2, "3F00");
    uint8_t rd[4] = { 0, 0, 10, 0 };            /* offset 0, 10 bytes */
    qmi_tlv_add(&q, 0x03, rd, 4);
    if (qmi_request(&q_uim, &q, &r, 5000, &e) || e)
        return -1;
    size_t tl;
    const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *d = qmi_tlv_find(tv, tl, 0x11, &vl);
    if (!d || vl < 2)
        return -1;
    size_t len = qmi_le16(d), o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        uint8_t lo = d[2 + i] & 0x0f, hi = d[2 + i] >> 4;
        if (lo > 9)
            break;
        out[o++] = '0' + lo;
        if (hi > 9)
            break;
        out[o++] = '0' + hi;
    }
    out[o] = 0;
    return o ? 0 : -1;
}

/* IMSI from EF_IMSI (6F07): byte0 = length, then BCD with parity nibble. */
void sim_get_imsi(RIL_Token t)
{
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, UIM_READ_TRANSPARENT);
    add_file_tlvs(&q, 0x6F07, NULL);
    uint8_t rd[4] = { 0, 0, 0, 0 };
    qmi_tlv_add(&q, 0x03, rd, 4);
    if (qmi_request(&q_uim, &q, &r, 5000, &e) || e) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    size_t tl;
    const uint8_t *tv = qmi_msg_tlvs(&r, &tl);
    uint16_t vl;
    const uint8_t *d = qmi_tlv_find(tv, tl, 0x11, &vl);
    if (!d || vl < 4) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    int n = qmi_le16(d);
    const uint8_t *b = d + 2;
    char imsi[20];
    int k = 0;
    int len = b[0];
    for (int i = 1; i <= len && i < n && k < 18; i++) {
        int lo = b[i] & 0xf, hi = b[i] >> 4;
        if (i > 1 && lo <= 9)
            imsi[k++] = '0' + lo;
        if (hi <= 9)
            imsi[k++] = '0' + hi;
    }
    imsi[k] = 0;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, imsi, sizeof(char *));
}

/* ---- PIN ---- */

static void pin_session(struct qmi_msg *m)
{
    uint8_t sess[2] = { SESSION_PRIMARY_GW, 0 };
    qmi_tlv_add(m, 0x01, sess, 2);
}

static int pin_retries(int pin_id)
{
    sim_refresh();
    struct uim_app *a = gw_app();
    if (!a)
        return -1;
    return pin_id == 2 ? a->pin2_retries : a->pin1_retries;
}

/* data[0] = pin (or puk), data[1] = new pin / aid */
void sim_pin_op(RIL_Token t, int request, char **data)
{
    struct qmi_msg q, r;
    uint16_t e;
    int pin_id = 1;
    switch (request) {
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_ENTER_SIM_PUK2:
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        pin_id = 2;
    }
    switch (request) {
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_ENTER_SIM_PIN2: {
        qmi_msg_init(&q, UIM_VERIFY_PIN);
        pin_session(&q);
        uint8_t v[2 + 16];
        int pl = strlen(data[0]);
        if (pl > 16) pl = 16;
        v[0] = pin_id; v[1] = pl;
        memcpy(v + 2, data[0], pl);
        qmi_tlv_add(&q, 0x02, v, 2 + pl);
        break;
    }
    case RIL_REQUEST_ENTER_SIM_PUK:
    case RIL_REQUEST_ENTER_SIM_PUK2: {
        qmi_msg_init(&q, UIM_UNBLOCK_PIN);
        pin_session(&q);
        uint8_t v[3 + 32];
        int kl = strlen(data[0]), nl = strlen(data[1]);
        if (kl > 16) kl = 16;
        if (nl > 16) nl = 16;
        v[0] = pin_id; v[1] = kl;
        memcpy(v + 2, data[0], kl);
        v[2 + kl] = nl;
        memcpy(v + 3 + kl, data[1], nl);
        qmi_tlv_add(&q, 0x02, v, 3 + kl + nl);
        break;
    }
    case RIL_REQUEST_CHANGE_SIM_PIN:
    case RIL_REQUEST_CHANGE_SIM_PIN2: {
        qmi_msg_init(&q, UIM_CHANGE_PIN);
        pin_session(&q);
        uint8_t v[3 + 32];
        int ol = strlen(data[0]), nl = strlen(data[1]);
        if (ol > 16) ol = 16;
        if (nl > 16) nl = 16;
        v[0] = pin_id; v[1] = ol;
        memcpy(v + 2, data[0], ol);
        v[2 + ol] = nl;
        memcpy(v + 3 + ol, data[1], nl);
        qmi_tlv_add(&q, 0x02, v, 3 + ol + nl);
        break;
    }
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    int ok = !qmi_request(&q_uim, &q, &r, 10000, &e) && !e;
    int retries = pin_retries(pin_id);
    RIL_onRequestComplete(t, ok ? RIL_E_SUCCESS : RIL_E_PASSWORD_INCORRECT, &retries, sizeof(retries));
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
}

/* Facility lock "SC" = SIM PIN1 enabled state; data: facility, password,
 * service class, aid. */
void sim_query_facility(RIL_Token t, char **data)
{
    int status = 0;
    if (data && data[0] && !strcmp(data[0], "SC")) {
        sim_refresh();
        struct uim_app *a = gw_app();
        status = a && (a->pin1_state == 1 || a->pin1_state == 2 || a->pin1_state == 4);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &status, sizeof(status));
}

void sim_set_facility(RIL_Token t, char **data)
{
    if (!data || !data[0] || strcmp(data[0], "SC")) {
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, UIM_SET_PIN_PROT);
    pin_session(&q);
    int pl = strlen(data[2] ? data[2] : "");
    if (pl > 16) pl = 16;
    uint8_t v[3 + 16];
    v[0] = 1;                                   /* PIN1 */
    v[1] = data[1] && data[1][0] == '1';        /* enable */
    v[2] = pl;
    memcpy(v + 3, data[2], pl);
    qmi_tlv_add(&q, 0x02, v, 3 + pl);
    int ok = !qmi_request(&q_uim, &q, &r, 10000, &e) && !e;
    int retries = pin_retries(1);
    RIL_onRequestComplete(t, ok ? RIL_E_SUCCESS : RIL_E_PASSWORD_INCORRECT, &retries, sizeof(retries));
}

/* ---- indications ---- */

static void uim_ind(struct qmi_client *c, uint16_t id, const uint8_t *tlvs, size_t len, void *user)
{
    (void)c; (void)user;
    if (id == UIM_STATUS_CHANGE_IND) {
        pthread_mutex_lock(&sim_lock);
        parse_card_status(tlvs, len);
        pthread_mutex_unlock(&sim_lock);
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        ril_sim_changed();
    }
}

int sim_init(void)
{
    if (qmi_client_open(&q_uim, QMI_UIM, 10000, uim_ind, NULL))
        return -1;
    struct qmi_msg q, r;
    uint16_t e;
    qmi_msg_init(&q, UIM_REGISTER_EVENTS);
    qmi_tlv_add_u32(&q, 0x01, 0x1);             /* card status */
    qmi_request(&q_uim, &q, &r, 5000, &e);
    sim_refresh();
    return 0;
}
