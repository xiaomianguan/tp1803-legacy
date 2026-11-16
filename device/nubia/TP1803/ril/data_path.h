/* Mobile data path on SM8150: modem -> IPA -> rmnet_ipa0 (QMAP) -> rmnet
 * core mux -> rmnet_dataN. This is what Qualcomm's netmgrd does. */
#pragma once
#include <stdint.h>
#include "qmi.h"

#define DP_PHYS_IF   "rmnet_ipa0"
#define DP_EP_TYPE   4      /* QMI_DATA_ENDPOINT_TYPE_EMBEDDED */
#define DP_EP_IFACE  1

/* One-time link setup (IPA data formats, DPM port, WDA data format). */
int dp_link_setup(struct qmi_client *wda, struct qmi_client *dpm);

/* Create rmnet_data<n> (mux id n+1) on rmnet_ipa0 and register the mux id
 * with IPA. Idempotent. */
int dp_mux_setup(int n);

/* Interface helpers (ioctl/netlink). addr/gw in host order. */
int dp_if_up(const char *ifname, int up, int mtu);
int dp_if_set_ipv4(const char *ifname, uint32_t addr, int prefix);
int dp_if_flush(const char *ifname);

struct dp_settings {
    uint32_t v4_addr, v4_gw, v4_mask, v4_dns[2];
    int v4_prefix;
    uint8_t v6_addr[16], v6_gw[16], v6_dns[2][16];
    int v6_prefix, has_v6;
    uint32_t mtu;
};

/* WDS client bound to mux n: bind + (optionally) set IP family. */
int dp_wds_bind(struct qmi_client *wds, int n, int ip_family);
/* Start network; returns packet data handle via *handle; on failure fills
 * *call_end (verbose reason type<<16 | reason) when available. */
int dp_wds_start(struct qmi_client *wds, const char *apn, const char *user,
                 const char *pass, int auth, int ip_family,
                 uint32_t *handle, uint32_t *call_end);
int dp_wds_stop(struct qmi_client *wds, uint32_t handle);
int dp_wds_settings(struct qmi_client *wds, struct dp_settings *s);
