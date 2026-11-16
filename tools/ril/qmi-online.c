/* Set DMS operating mode (default: online) and report the result. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "qmi.h"
int main(int argc, char **argv)
{
    struct qmi_client dms; struct qmi_msg q, r; uint16_t e;
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    if (qmi_client_open(&dms, QMI_DMS, 3000, NULL, NULL)) return 1;
    qmi_msg_init(&q, 0x002E); qmi_tlv_add_u8(&q, 0x01, mode);
    if (!qmi_request(&dms, &q, &r, 10000, &e)) printf("set mode %d -> err %u\n", mode, e);
    for (int i = 0; i < 5; i++) {
        sleep(1);
        if (!qmi_simple(&dms, 0x002D, &r, &e)) {
            size_t tl; const uint8_t *t = qmi_msg_tlvs(&r, &tl); uint16_t vl;
            const uint8_t *v = qmi_tlv_find(t, tl, 0x01, &vl);
            const uint8_t *off = qmi_tlv_find(t, tl, 0x10, &vl);
            printf("mode now %d offline-reason %s\n", v ? v[0] : -1, off ? "present" : "-");
        }
    }
    return 0;
}
