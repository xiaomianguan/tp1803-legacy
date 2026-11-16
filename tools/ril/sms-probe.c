/* WMS state dump and indication listener: sms-probe [listen-seconds] */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "qmi.h"

static struct qmi_client wms;

static void dump(const char *what, const uint8_t *t, size_t len)
{
    printf("%s:", what);
    for (size_t i = 0; i < len; i++)
        printf(" %02x", t[i]);
    printf("\n");
    fflush(stdout);
}

static void ind(struct qmi_client *c, uint16_t id, const uint8_t *t, size_t len, void *u)
{
    (void)c; (void)u;
    char b[32];
    snprintf(b, sizeof(b), "IND 0x%04x", id);
    dump(b, t, len);
}

static void req(const char *what, struct qmi_msg *q)
{
    struct qmi_msg r;
    uint16_t e = 0;
    int x = qmi_request(&wms, q, &r, 5000, &e);
    size_t tl;
    const uint8_t *t = x ? NULL : qmi_msg_tlvs(&r, &tl);
    printf("%s: r=%d err=0x%x\n", what, x, e);
    if (t)
        dump("  tlvs", t, tl);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    if (qmi_client_open(&wms, QMI_WMS, 3000, ind, NULL)) {
        printf("no WMS\n");
        return 1;
    }
    struct qmi_msg q;
    qmi_msg_init(&q, 0x0033); req("get routes", &q);
    qmi_msg_init(&q, 0x0030); req("get message protocol", &q);
    for (int st = 0; st < 2; st++) {
        qmi_msg_init(&q, 0x0031);
        qmi_tlv_add_u8(&q, 0x01, st);
        qmi_tlv_add_u8(&q, 0x12, 1);
        req(st ? "list NV" : "list UIM", &q);
    }
    qmi_msg_init(&q, 0x0034); req("get smsc", &q);
    qmi_msg_init(&q, 0x004c); req("get transport layer", &q);
    qmi_msg_init(&q, 0x0051); req("get domain pref", &q);
    if (secs) {
        qmi_msg_init(&q, 0x0001);
        qmi_tlv_add_u8(&q, 0x10, 1);
        req("set event report", &q);
        qmi_msg_init(&q, 0x0047);
        qmi_tlv_add_u8(&q, 0x10, 1);
        qmi_tlv_add_u8(&q, 0x11, 1);
        req("indication register", &q);
        printf("listening %d s\n", secs);
        fflush(stdout);
        sleep(secs);
    }
    return 0;
}
