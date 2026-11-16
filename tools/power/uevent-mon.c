/*
 * Netlink monitor for suspend debugging: logs kernel uevents, rtnetlink
 * notifications and generic-netlink multicasts (nl80211, cld80211, ...) with
 * CLOCK_BOOTTIME timestamps, so they can be lined up with "PM: suspend entry"
 * lines in dmesg.
 *
 * usage: uevent-mon [logfile]
 */
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static FILE *out;

static void stamp(const char *what)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    fprintf(out, "[%ld.%03ld] %s", (long)ts.tv_sec, ts.tv_nsec / 1000000, what);
}

/* Subscribe a genl socket to every multicast group of every family. */
static void genl_join_all(int s)
{
    char buf[32768];
    struct { struct nlmsghdr n; struct genlmsghdr g; } req = {
        .n = { .nlmsg_len = sizeof(req), .nlmsg_type = GENL_ID_CTRL,
               .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 1 },
        .g = { .cmd = CTRL_CMD_GETFAMILY, .version = 1 },
    };
    send(s, &req, sizeof(req), 0);
    for (;;) {
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) return;
            struct nlattr *a = (void *)((char *)NLMSG_DATA(h) + GENL_HDRLEN);
            int len = h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
            const char *fam = "?";
            for (; len >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN;
                 len -= NLA_ALIGN(a->nla_len), a = (void *)((char *)a + NLA_ALIGN(a->nla_len))) {
                if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_NAME)
                    fam = (char *)a + NLA_HDRLEN;
                if ((a->nla_type & NLA_TYPE_MASK) != CTRL_ATTR_MCAST_GROUPS)
                    continue;
                struct nlattr *g = (void *)((char *)a + NLA_HDRLEN);
                int glen = a->nla_len - NLA_HDRLEN;
                for (; glen >= (int)NLA_HDRLEN && g->nla_len >= NLA_HDRLEN;
                     glen -= NLA_ALIGN(g->nla_len), g = (void *)((char *)g + NLA_ALIGN(g->nla_len))) {
                    struct nlattr *f = (void *)((char *)g + NLA_HDRLEN);
                    int flen = g->nla_len - NLA_HDRLEN;
                    const char *gname = "?";
                    unsigned id = 0;
                    for (; flen >= (int)NLA_HDRLEN && f->nla_len >= NLA_HDRLEN;
                         flen -= NLA_ALIGN(f->nla_len), f = (void *)((char *)f + NLA_ALIGN(f->nla_len))) {
                        if ((f->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_MCAST_GRP_ID)
                            id = *(unsigned *)((char *)f + NLA_HDRLEN);
                        if ((f->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_MCAST_GRP_NAME)
                            gname = (char *)f + NLA_HDRLEN;
                    }
                    if (id && setsockopt(s, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &id, sizeof(id)) == 0)
                        fprintf(out, "genl: joined %s/%s (%u)\n", fam, gname, id);
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "a") : stdout;
    if (!out) return 1;
    setvbuf(out, NULL, _IOLBF, 0);

    int ue = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = ~0u };
    bind(ue, (struct sockaddr *)&a, sizeof(a));

    int rt = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    struct sockaddr_nl r = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_NEIGH };
    bind(rt, (struct sockaddr *)&r, sizeof(r));

    int ge = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_GENERIC);
    struct sockaddr_nl g = { .nl_family = AF_NETLINK };
    bind(ge, (struct sockaddr *)&g, sizeof(g));
    genl_join_all(ge);

    stamp("monitor started\n");
    struct pollfd p[3] = { { ue, POLLIN, 0 }, { rt, POLLIN, 0 }, { ge, POLLIN, 0 } };
    char buf[16384];
    for (;;) {
        if (poll(p, 3, -1) <= 0) continue;
        if (p[0].revents) {
            ssize_t n = recv(ue, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                stamp("uevent ");
                fputs(buf, out);
                for (char *q = buf + strlen(buf) + 1; q < buf + n; q += strlen(q) + 1)
                    if (!strncmp(q, "SUBSYSTEM=", 10) || !strncmp(q, "POWER_SUPPLY_NAME=", 18) || !strncmp(q, "DEVNAME=", 8))
                        fprintf(out, " %s", q);
                fputc('\n', out);
            }
        }
        if (p[1].revents) {
            ssize_t n = recv(rt, buf, sizeof(buf), 0);
            for (struct nlmsghdr *h = (void *)buf; n > 0 && NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
                char t[64];
                snprintf(t, sizeof(t), "rtnl type %u\n", h->nlmsg_type);
                stamp(t);
            }
        }
        if (p[2].revents) {
            ssize_t n = recv(ge, buf, sizeof(buf), 0);
            for (struct nlmsghdr *h = (void *)buf; n > 0 && NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
                struct genlmsghdr *gh = NLMSG_DATA(h);
                char t[64];
                snprintf(t, sizeof(t), "genl family %u cmd %u\n", h->nlmsg_type, gh->cmd);
                stamp(t);
            }
        }
    }
}
