/* Dump NAS signal info (0x004F) TLVs. */
#include <stdio.h>
#include "qmi.h"
int main(void)
{
    struct qmi_client nas; struct qmi_msg r; uint16_t e;
    if (qmi_client_open(&nas, QMI_NAS, 3000, NULL, NULL)) return 1;
    unsigned ids[] = { 0x004F, 0x0020 };
    for (int k = 0; k < 2; k++) {
        if (qmi_simple(&nas, ids[k], &r, &e)) continue;
        printf("msg 0x%04x err %u\n", ids[k], e);
        size_t tl; const uint8_t *t = qmi_msg_tlvs(&r, &tl); size_t off = 0;
        while (off + 3 <= tl) {
            uint16_t l = qmi_le16(t + off + 1);
            printf("  tlv 0x%02x [%u]:", t[off], l);
            for (int i = 0; i < l && i < 32; i++) printf(" %02x", t[off + 3 + i]);
            printf("\n"); off += 3 + l;
        }
    }
    return 0;
}
