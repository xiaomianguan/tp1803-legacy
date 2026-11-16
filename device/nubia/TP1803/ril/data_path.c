/* Mobile data path: see data_path.h */
#include "data_path.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_addr.h>

extern void (*qmi_log)(const char *fmt, ...);
#define LOG(...) qmi_log(__VA_ARGS__)

/* ---- rmnet_ipa0 private ioctls (uapi msm_rmnet.h) ---- */
#define RMNET_IOCTL_EXTENDED                0x89FD
#define RMNET_IOCTL_ADD_MUX_CHANNEL         0x0005
#define RMNET_IOCTL_SET_EGRESS_DATA_FORMAT  0x0006
#define RMNET_IOCTL_SET_INGRESS_DATA_FORMAT 0x0007
#define RMNET_IOCTL_GET_EP_PAIR             0x0010
#define RMNET_IOCTL_EGRESS_FORMAT_MAP       (1 << 1)
#define RMNET_IOCTL_EGRESS_FORMAT_MUXING    (1 << 3)
#define RMNET_IOCTL_INGRESS_FORMAT_MAP      (1 << 1)
#define RMNET_IOCTL_INGRESS_FORMAT_DEMUXING (1 << 3)
#define RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA (1 << 5)
/* DL aggregation (stock msmnile netmgr: 31 packets / 16 KiB) */
#define DL_AGG_PKTS 31
#define DL_AGG_SIZE 16384

struct rmnet_ioctl_extended_s {
    uint32_t extended_ioctl;
    union {
        uint32_t data;
        int8_t if_name[IFNAMSIZ];
        struct { uint32_t mux_id; int8_t vchannel_name[IFNAMSIZ]; } rmnet_mux_val;
        struct { uint32_t consumer_pipe_num, producer_pipe_num; } ipa_ep_pair;
        struct { uint32_t data, agg_size, agg_count; } ingress_format;
    } u;
};

static int rmnet_ext(struct rmnet_ioctl_extended_s *e)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, DP_PHYS_IF, IFNAMSIZ - 1);
    ifr.ifr_ifru.ifru_data = (void *)e;
    int r = ioctl(fd, RMNET_IOCTL_EXTENDED, &ifr) < 0 ? -errno : 0;
    close(fd);
    return r;
}

/* ---- netlink ---- */

struct nlreq { struct nlmsghdr n; union { struct ifinfomsg i; struct ifaddrmsg a; } u; char buf[512]; };

static struct rtattr *nla_add(struct nlmsghdr *n, int type, const void *d, int len)
{
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    if (len)
        memcpy(RTA_DATA(rta), d, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}
static void nla_end(struct nlmsghdr *n, struct rtattr *nest)
{
    nest->rta_len = (char *)n + NLMSG_ALIGN(n->nlmsg_len) - (char *)nest;
}

static int nl_talk(struct nlmsghdr *n)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    n->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    n->nlmsg_seq = 1;
    int r = 0;
    if (sendto(fd, n, n->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        r = -errno;
    } else {
        char buf[1024];
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        if (len < 0)
            r = -errno;
        else if (NLMSG_OK(h, (size_t)len) && h->nlmsg_type == NLMSG_ERROR)
            r = ((struct nlmsgerr *)NLMSG_DATA(h))->error;
    }
    close(fd);
    return r;
}

#define IFLA_RMNET_MUX_ID 1
#define IFLA_RMNET_FLAGS  2
#define RMNET_FLAGS_INGRESS_DEAGGREGATION (1U << 0)
#define RMNET_FLAGS_INGRESS_MAP_COMMANDS  (1U << 1)

static int rmnet_newlink(const char *name, int mux_id)
{
    struct nlreq r;
    memset(&r, 0, sizeof(r));
    r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    r.n.nlmsg_type = RTM_NEWLINK;
    r.n.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL;
    r.u.i.ifi_family = AF_UNSPEC;
    uint32_t link = if_nametoindex(DP_PHYS_IF);
    if (!link)
        return -ENODEV;
    nla_add(&r.n, IFLA_LINK, &link, 4);
    nla_add(&r.n, IFLA_IFNAME, name, strlen(name) + 1);
    struct rtattr *li = nla_add(&r.n, IFLA_LINKINFO, NULL, 0);
    nla_add(&r.n, IFLA_INFO_KIND, "rmnet", 6);
    struct rtattr *data = nla_add(&r.n, IFLA_INFO_DATA, NULL, 0);
    uint16_t mux = mux_id;
    nla_add(&r.n, IFLA_RMNET_MUX_ID, &mux, 2);
    struct { uint32_t flags, mask; } fl = {
        RMNET_FLAGS_INGRESS_DEAGGREGATION | RMNET_FLAGS_INGRESS_MAP_COMMANDS, 0xffffffff };
    nla_add(&r.n, IFLA_RMNET_FLAGS, &fl, sizeof(fl));
    nla_end(&r.n, data);
    nla_end(&r.n, li);
    return nl_talk(&r.n);
}

int dp_if_up(const char *ifname, int up, int mtu)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    int r = 0;
    if (mtu > 0) {
        ifr.ifr_mtu = mtu;
        if (ioctl(fd, SIOCSIFMTU, &ifr) < 0)
            LOG("data: mtu %d on %s: %s", mtu, ifname, strerror(errno));
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        r = -errno;
    } else {
        if (up)
            ifr.ifr_flags |= IFF_UP;
        else
            ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            r = -errno;
    }
    close(fd);
    return r;
}

int dp_if_set_ipv4(const char *ifname, uint32_t addr, int prefix)
{
    struct nlreq r;
    memset(&r, 0, sizeof(r));
    r.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    r.n.nlmsg_type = RTM_NEWADDR;
    r.n.nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE;
    r.u.a.ifa_family = AF_INET;
    r.u.a.ifa_prefixlen = prefix;
    r.u.a.ifa_index = if_nametoindex(ifname);
    if (!r.u.a.ifa_index)
        return -ENODEV;
    uint32_t a = htonl(addr);
    nla_add(&r.n, IFA_LOCAL, &a, 4);
    nla_add(&r.n, IFA_ADDRESS, &a, 4);
    return nl_talk(&r.n);
}

int dp_if_flush(const char *ifname)
{
    /* Bring the link down/up: drops IPv4 addresses on rmnet (no carrier
     * semantics), simplest portable flush for our purposes. */
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = 0;
    int r = ioctl(fd, SIOCSIFADDR, &ifr) < 0 ? -errno : 0;
    close(fd);
    return r;
}

/* ---- link setup ---- */

int dp_link_setup(struct qmi_client *wda, struct qmi_client *dpm)
{
    struct rmnet_ioctl_extended_s e;
    int r;

    memset(&e, 0, sizeof(e));
    e.extended_ioctl = RMNET_IOCTL_SET_INGRESS_DATA_FORMAT;
    e.u.ingress_format.data = RMNET_IOCTL_INGRESS_FORMAT_MAP |
        RMNET_IOCTL_INGRESS_FORMAT_DEMUXING | RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA;
    e.u.ingress_format.agg_size = DL_AGG_SIZE;
    e.u.ingress_format.agg_count = DL_AGG_PKTS;
    if ((r = rmnet_ext(&e)))
        LOG("data: ingress format: %s", strerror(-r));

    memset(&e, 0, sizeof(e));
    e.extended_ioctl = RMNET_IOCTL_SET_EGRESS_DATA_FORMAT;
    e.u.data = RMNET_IOCTL_EGRESS_FORMAT_MAP | RMNET_IOCTL_EGRESS_FORMAT_MUXING;
    if ((r = rmnet_ext(&e)))
        LOG("data: egress format: %s", strerror(-r));

    memset(&e, 0, sizeof(e));
    e.extended_ioctl = RMNET_IOCTL_GET_EP_PAIR;
    if ((r = rmnet_ext(&e))) {
        LOG("data: get ep pair: %s", strerror(-r));
        return r;
    }
    uint32_t cons = e.u.ipa_ep_pair.consumer_pipe_num;
    uint32_t prod = e.u.ipa_ep_pair.producer_pipe_num;
    LOG("data: IPA ep pair consumer %u producer %u", cons, prod);

    uint16_t err;
    struct qmi_msg req, resp;
    if (dpm && dpm->port) {
        /* hardware data port: ep type, iface, rx ep, tx ep */
        uint8_t hw[1 + 16];
        uint32_t v[4] = { DP_EP_TYPE, DP_EP_IFACE, cons, prod };
        hw[0] = 1;
        memcpy(hw + 1, v, 16);              /* little-endian host */
        qmi_msg_init(&req, 0x0020);
        qmi_tlv_add(&req, 0x11, hw, sizeof(hw));
        r = qmi_request(dpm, &req, &resp, 5000, &err);
        LOG("data: DPM open port: r=%d err=0x%x", r, err);
    }

    qmi_msg_init(&req, 0x0020);             /* WDA set data format */
    qmi_tlv_add_u8(&req, 0x10, 0);          /* no QoS header */
    qmi_tlv_add_u32(&req, 0x11, 2);         /* raw IP */
    qmi_tlv_add_u32(&req, 0x12, 5);         /* UL QMAP */
    qmi_tlv_add_u32(&req, 0x13, 5);         /* DL QMAP */
    qmi_tlv_add_u32(&req, 0x15, DL_AGG_PKTS);  /* DL max datagrams */
    qmi_tlv_add_u32(&req, 0x16, DL_AGG_SIZE);  /* DL max size */
    uint32_t ep[2] = { DP_EP_TYPE, DP_EP_IFACE };
    qmi_tlv_add(&req, 0x17, ep, sizeof(ep));
    r = qmi_request(wda, &req, &resp, 5000, &err);
    LOG("data: WDA set data format: r=%d err=0x%x", r, err);
    if (r)
        return r;
    return err ? -EIO : 0;
}

int dp_mux_setup(int n)
{
    char name[IFNAMSIZ];
    snprintf(name, sizeof(name), "rmnet_data%d", n);
    struct rmnet_ioctl_extended_s e;
    memset(&e, 0, sizeof(e));
    e.extended_ioctl = RMNET_IOCTL_ADD_MUX_CHANNEL;
    e.u.rmnet_mux_val.mux_id = n + 1;
    strncpy((char *)e.u.rmnet_mux_val.vchannel_name, name, IFNAMSIZ - 1);
    int r = rmnet_ext(&e);
    if (r)
        LOG("data: add mux channel %d: %s", n + 1, strerror(-r));
    if (if_nametoindex(name))
        return 0;
    r = rmnet_newlink(name, n + 1);
    LOG("data: create %s (mux %d): %d", name, n + 1, r);
    return r;
}

/* ---- WDS ---- */

int dp_wds_bind(struct qmi_client *wds, int n, int ip_family)
{
    struct qmi_msg req, resp;
    uint16_t err;
    qmi_msg_init(&req, 0x00A2);
    uint32_t ep[2] = { DP_EP_TYPE, DP_EP_IFACE };
    qmi_tlv_add(&req, 0x10, ep, sizeof(ep));
    qmi_tlv_add_u8(&req, 0x11, n + 1);
    int r = qmi_request(wds, &req, &resp, 5000, &err);
    LOG("data: WDS bind mux %d: r=%d err=0x%x", n + 1, r, err);
    if (r || err)
        return r ? r : -EIO;
    if (ip_family) {
        qmi_msg_init(&req, 0x004D);
        qmi_tlv_add_u8(&req, 0x01, ip_family);
        r = qmi_request(wds, &req, &resp, 5000, &err);
        LOG("data: WDS set ip family %d: r=%d err=0x%x", ip_family, r, err);
    }
    return 0;
}

int dp_wds_start(struct qmi_client *wds, const char *apn, const char *user,
                 const char *pass, int auth, int ip_family,
                 uint32_t *handle, uint32_t *call_end)
{
    struct qmi_msg req, resp;
    uint16_t err;
    qmi_msg_init(&req, 0x0020);
    if (apn && *apn)
        qmi_tlv_add(&req, 0x14, apn, strlen(apn));
    if (user && *user)
        qmi_tlv_add(&req, 0x17, user, strlen(user));
    if (pass && *pass)
        qmi_tlv_add(&req, 0x18, pass, strlen(pass));
    if (auth >= 0 && ((user && *user) || (pass && *pass)))
        qmi_tlv_add_u8(&req, 0x16, auth);
    if (ip_family)
        qmi_tlv_add_u8(&req, 0x19, ip_family);
    int r = qmi_request(wds, &req, &resp, 60000, &err);
    *call_end = 0;
    if (r)
        return r;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&resp, &tl);
    uint16_t vl;
    const uint8_t *v;
    if ((v = qmi_tlv_find(t, tl, 0x11, &vl)) && vl >= 4)
        *call_end = (uint32_t)qmi_le16(v) << 16 | qmi_le16(v + 2);
    else if ((v = qmi_tlv_find(t, tl, 0x10, &vl)) && vl >= 2)
        *call_end = qmi_le16(v);
    LOG("data: WDS start network apn '%s' fam %d: err=0x%x end=0x%x", apn ? apn : "",
        ip_family, err, *call_end);
    if (err)
        return err == 0x1a /* NO_EFFECT: already up */ ? 0 : -EIO;
    if ((v = qmi_tlv_find(t, tl, 0x01, &vl)) && vl >= 4)
        *handle = qmi_le32(v);
    return 0;
}

int dp_wds_stop(struct qmi_client *wds, uint32_t handle)
{
    struct qmi_msg req, resp;
    uint16_t err;
    qmi_msg_init(&req, 0x0021);
    qmi_tlv_add_u32(&req, 0x01, handle);
    int r = qmi_request(wds, &req, &resp, 10000, &err);
    LOG("data: WDS stop network: r=%d err=0x%x", r, err);
    return r ? r : err ? -EIO : 0;
}

static int mask_to_prefix(uint32_t m)
{
    int p = 0;
    while (m & 0x80000000u) {
        p++;
        m <<= 1;
    }
    return p;
}

int dp_wds_settings(struct qmi_client *wds, struct dp_settings *s)
{
    struct qmi_msg req, resp;
    uint16_t err;
    memset(s, 0, sizeof(*s));
    qmi_msg_init(&req, 0x002D);
    /* DNS | IP address | gateway | MTU | IP family */
    qmi_tlv_add_u32(&req, 0x10, (1u << 4) | (1u << 8) | (1u << 9) | (1u << 13) | (1u << 15));
    int r = qmi_request(wds, &req, &resp, 5000, &err);
    if (r || err)
        return r ? r : -EIO;
    size_t tl;
    const uint8_t *t = qmi_msg_tlvs(&resp, &tl);
    uint16_t vl;
    const uint8_t *v;
    if ((v = qmi_tlv_find(t, tl, 0x1E, &vl)) && vl >= 4) s->v4_addr = qmi_le32(v);
    if ((v = qmi_tlv_find(t, tl, 0x20, &vl)) && vl >= 4) s->v4_gw = qmi_le32(v);
    if ((v = qmi_tlv_find(t, tl, 0x21, &vl)) && vl >= 4) s->v4_mask = qmi_le32(v);
    if ((v = qmi_tlv_find(t, tl, 0x15, &vl)) && vl >= 4) s->v4_dns[0] = qmi_le32(v);
    if ((v = qmi_tlv_find(t, tl, 0x16, &vl)) && vl >= 4) s->v4_dns[1] = qmi_le32(v);
    if ((v = qmi_tlv_find(t, tl, 0x29, &vl)) && vl >= 4) s->mtu = qmi_le32(v);
    s->v4_prefix = s->v4_mask ? mask_to_prefix(s->v4_mask) : 32;
    if ((v = qmi_tlv_find(t, tl, 0x25, &vl)) && vl >= 17) {
        memcpy(s->v6_addr, v, 16);      /* network order */
        s->v6_prefix = v[16];
        s->has_v6 = 1;
    }
    if ((v = qmi_tlv_find(t, tl, 0x26, &vl)) && vl >= 16) memcpy(s->v6_gw, v, 16);
    if ((v = qmi_tlv_find(t, tl, 0x27, &vl)) && vl >= 16) memcpy(s->v6_dns[0], v, 16);
    if ((v = qmi_tlv_find(t, tl, 0x28, &vl)) && vl >= 16) memcpy(s->v6_dns[1], v, 16);
    return 0;
}
