/* Dump NAS Get PLMN Name (0x0044) for an MCC/MNC. usage: qmi-plmn 260 06 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qmi.h"
int main(int argc, char **argv)
{
    struct qmi_client nas; struct qmi_msg q, r; uint16_t e;
    if (argc < 3 || qmi_client_open(&nas, QMI_NAS, 3000, NULL, NULL)) return 1;
    uint16_t m = atoi(argv[1]), n = atoi(argv[2]);
    for (int pcs = 0; pcs < 2; pcs++) {
        qmi_msg_init(&q, 0x0044);
        uint8_t plmn[4] = { m & 0xff, m >> 8, n & 0xff, n >> 8 };
        qmi_tlv_add(&q, 0x01, plmn, 4);
        if (pcs) { uint8_t p[2] = { 1, strlen(argv[2]) == 3 }; qmi_tlv_add(&q, 0x16, p, 2); }
        if (qmi_request(&nas, &q, &r, 5000, &e)) continue;
        printf("pcs tlv %d: err %u\n", pcs, e);
        size_t tl; const uint8_t *t = qmi_msg_tlvs(&r, &tl); size_t off = 0;
        while (off + 3 <= tl) {
            uint16_t l = qmi_le16(t + off + 1);
            printf("  tlv 0x%02x [%u]:", t[off], l);
            for (int i = 0; i < l && i < 64; i++) printf(" %02x", t[off + 3 + i]);
            printf("\n"); off += 3 + l;
        }
    }
    return 0;
}
