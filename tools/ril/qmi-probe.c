/* Query basic modem state over QMI/QRTR. */
#include <stdio.h>
#include <string.h>
#include "qmi.h"

static void str_tlv(const struct qmi_msg *m, uint8_t t, const char *name)
{
    size_t tl; const uint8_t *tlv = qmi_msg_tlvs(m, &tl); uint16_t vl;
    const uint8_t *v = qmi_tlv_find(tlv, tl, t, &vl);
    if (v) printf("  %s: %.*s\n", name, vl, v);
}

static void dump(const struct qmi_msg *m)
{
    size_t tl; const uint8_t *t = qmi_msg_tlvs(m, &tl); size_t off = 0;
    while (off + 3 <= tl) {
        uint16_t l = qmi_le16(t + off + 1);
        printf("  tlv 0x%02x [%u]:", t[off], l);
        for (int i = 0; i < l && i < 48; i++) printf(" %02x", t[off + 3 + i]);
        printf("\n");
        off += 3 + l;
    }
}

int main(void)
{
    struct qmi_client dms, nas, uim;
    struct qmi_msg r; uint16_t e;
    if (qmi_client_open(&dms, QMI_DMS, 3000, NULL, NULL)) { printf("no DMS\n"); return 1; }
    printf("DMS node %u port %u\n", dms.node, dms.port);
    if (!qmi_simple(&dms, 0x0025, &r, &e)) { printf("serials err %u\n", e); str_tlv(&r, 0x11, "IMEI"); str_tlv(&r, 0x12, "MEID"); }
    if (!qmi_simple(&dms, 0x0023, &r, &e)) { printf("revision err %u\n", e); str_tlv(&r, 0x01, "rev"); }
    if (!qmi_simple(&dms, 0x002D, &r, &e)) { printf("operating mode err %u\n", e); dump(&r); }
    if (!qmi_client_open(&uim, QMI_UIM, 3000, NULL, NULL)) {
        if (!qmi_simple(&uim, 0x002F, &r, &e)) { printf("UIM card status err %u\n", e); dump(&r); }
    }
    if (!qmi_client_open(&nas, QMI_NAS, 3000, NULL, NULL)) {
        if (!qmi_simple(&nas, 0x0024, &r, &e)) { printf("NAS serving system err %u\n", e); dump(&r); }
        if (!qmi_simple(&nas, 0x004D, &r, &e)) { printf("NAS sys info err %u\n", e); dump(&r); }
    }
    return 0;
}
