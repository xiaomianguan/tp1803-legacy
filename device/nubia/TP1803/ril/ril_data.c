/* Mobile data for the TP1803 QMI RIL: one data call (cid 1) on rmnet_data0,
 * IPv4. See data_path.c for the IPA/rmnet plumbing. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "RIL-TP1803"
#include <utils/Log.h>

#include "ril_qmi.h"
#include "data_path.h"

#define DATA_IF  "rmnet_data0"
#define DATA_CID 1

static struct qmi_client q_wda, q_dpm;
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static int link_ready;

static struct {
    int active;
    uint32_t handle;
    char addresses[64], gateways[64], dnses[64];
    int mtu;
} call;
static int last_fail = PDP_FAIL_NONE;

static void ip4str(char *b, size_t n, uint32_t a)
{
    snprintf(b, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

static void fill_response(RIL_Data_Call_Response_v11 *r, int status)
{
    memset(r, 0, sizeof(*r));
    r->status = status;
    r->suggestedRetryTime = -1;
    r->cid = DATA_CID;
    r->active = call.active ? 2 : 0;
    r->type = "IP";
    r->ifname = DATA_IF;
    r->addresses = call.active ? call.addresses : "";
    r->dnses = call.active ? call.dnses : "";
    r->gateways = call.active ? call.gateways : "";
    r->pcscf = "";
    r->mtu = call.active ? call.mtu : 0;
}

static void teardown_locked(void)
{
    if (!call.active)
        return;
    call.active = 0;
    dp_if_flush(DATA_IF);
    dp_if_up(DATA_IF, 0, 0);
}

static void send_call_list(void)
{
    RIL_Data_Call_Response_v11 r;
    pthread_mutex_lock(&data_lock);
    int active = call.active;
    if (active)
        fill_response(&r, PDP_FAIL_NONE);
    pthread_mutex_unlock(&data_lock);
    RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, active ? &r : NULL,
                              active ? sizeof(r) : 0);
}

static void wds_ind(struct qmi_client *c, uint16_t id, const uint8_t *tlvs, size_t len,
                    void *user)
{
    (void)c; (void)user;
    if (id != 0x0022)                       /* packet service status */
        return;
    uint16_t vl;
    const uint8_t *v = qmi_tlv_find(tlvs, len, 0x01, &vl);
    if (!v || vl < 1)
        return;
    ALOGI("data: packet service status %u", v[0]);
    if (v[0] == 1) {                        /* disconnected */
        /* Runs on the WDS reader thread: never block on a setup that is
         * itself waiting for this thread to deliver its response. */
        if (pthread_mutex_trylock(&data_lock))
            return;
        int was = call.active;
        teardown_locked();
        last_fail = PDP_FAIL_SIGNAL_LOST;
        pthread_mutex_unlock(&data_lock);
        if (was)
            send_call_list();
    }
}

/* Called lazily from the first data request (and from init). */
static int ensure_link(void)
{
    if (link_ready)
        return 0;
    if (!q_wda.port && qmi_client_open(&q_wda, QMI_WDA, 5000, NULL, NULL))
        return -ENODEV;
    if (!q_dpm.port && qmi_client_open(&q_dpm, QMI_DPM, 5000, NULL, NULL))
        ALOGW("data: no DPM service");
    if (!q_wds.port && qmi_client_open(&q_wds, QMI_WDS, 5000, wds_ind, NULL))
        return -ENODEV;
    if (dp_link_setup(&q_wda, q_dpm.port ? &q_dpm : NULL))
        ALOGW("data: link setup reported an error");
    if (dp_mux_setup(0))
        return -EIO;
    if (dp_wds_bind(&q_wds, 0, 4))
        return -EIO;
    link_ready = 1;
    return 0;
}

int data_init(void)
{
    pthread_mutex_lock(&data_lock);
    int r = ensure_link();
    pthread_mutex_unlock(&data_lock);
    if (r)
        ALOGW("data: link not ready yet (%d)", r);
    return 0;
}

static void setup_data_call(RIL_Token t, char **d, size_t datalen)
{
    int n = datalen / sizeof(char *);
    const char *apn = n > 2 && d[2] ? d[2] : "";
    const char *user = n > 3 && d[3] ? d[3] : "";
    const char *pass = n > 4 && d[4] ? d[4] : "";
    int auth = n > 5 && d[5] ? atoi(d[5]) : 0;
    const char *proto = n > 6 && d[6] ? d[6] : "IP";
    RIL_Data_Call_Response_v11 r;

    ALOGI("data: setup apn '%s' proto %s auth %d", apn, proto, auth);
    pthread_mutex_lock(&data_lock);
    if (ensure_link()) {
        pthread_mutex_unlock(&data_lock);
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
    if (call.active) {                       /* single PDN: hand back the live one */
        fill_response(&r, PDP_FAIL_NONE);
        pthread_mutex_unlock(&data_lock);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &r, sizeof(r));
        return;
    }
    uint32_t h = 0, end = 0;
    int e = dp_wds_start(&q_wds, apn, user, pass, auth, 4, &h, &end);
    struct dp_settings s;
    if (!e)
        e = dp_wds_settings(&q_wds, &s);
    if (e || !s.v4_addr) {
        last_fail = end ? (int)(end & 0xffff) : PDP_FAIL_ERROR_UNSPECIFIED;
        if (!e)
            dp_wds_stop(&q_wds, h);
        ALOGW("data: setup failed (%d, end 0x%x)", e, end);
        fill_response(&r, last_fail ? last_fail : PDP_FAIL_ERROR_UNSPECIFIED);
        r.suggestedRetryTime = -1;
        pthread_mutex_unlock(&data_lock);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &r, sizeof(r));
        return;
    }
    char a[16], g[16], d1[16], d2[16];
    ip4str(a, sizeof(a), s.v4_addr);
    ip4str(g, sizeof(g), s.v4_gw);
    ip4str(d1, sizeof(d1), s.v4_dns[0]);
    ip4str(d2, sizeof(d2), s.v4_dns[1]);
    snprintf(call.addresses, sizeof(call.addresses), "%s/%d", a, s.v4_prefix);
    snprintf(call.gateways, sizeof(call.gateways), "%s", s.v4_gw ? g : "");
    if (s.v4_dns[0] && s.v4_dns[1])
        snprintf(call.dnses, sizeof(call.dnses), "%s %s", d1, d2);
    else
        snprintf(call.dnses, sizeof(call.dnses), "%s", s.v4_dns[0] ? d1 : "");
    call.mtu = s.mtu ? (int)s.mtu : 1500;
    call.handle = h;
    dp_if_up("rmnet_ipa0", 1, 0);
    dp_if_flush(DATA_IF);
    dp_if_up(DATA_IF, 1, call.mtu);
    int ar = dp_if_set_ipv4(DATA_IF, s.v4_addr, s.v4_prefix);
    call.active = 1;
    last_fail = PDP_FAIL_NONE;
    ALOGI("data: up %s gw %s dns %s mtu %d (addr %d)", call.addresses, call.gateways,
          call.dnses, call.mtu, ar);
    fill_response(&r, PDP_FAIL_NONE);
    pthread_mutex_unlock(&data_lock);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &r, sizeof(r));
}

static void deactivate_data_call(RIL_Token t)
{
    pthread_mutex_lock(&data_lock);
    if (call.active) {
        dp_wds_stop(&q_wds, call.handle);
        teardown_locked();
    }
    pthread_mutex_unlock(&data_lock);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

void data_request(int request, void *data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_SETUP_DATA_CALL:
        setup_data_call(t, data, datalen);
        break;
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        deactivate_data_call(t);
        break;
    case RIL_REQUEST_DATA_CALL_LIST: {
        RIL_Data_Call_Response_v11 r;
        pthread_mutex_lock(&data_lock);
        int active = call.active;
        if (active)
            fill_response(&r, PDP_FAIL_NONE);
        pthread_mutex_unlock(&data_lock);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, active ? &r : NULL, active ? sizeof(r) : 0);
        break;
    }
    case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE: {
        int c = last_fail;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &c, sizeof(c));
        break;
    }
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
    }
}
