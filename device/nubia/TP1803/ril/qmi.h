/*
 * Minimal QMI client over QRTR (AF_QIPCRTR) for the TP1803 RIL.
 *
 * Each QMI service (NAS, DMS, UIM, VOICE, WMS, WDS, ...) is reached through
 * its own QRTR socket; a reader thread per client matches responses to
 * pending requests by transaction id and hands indications to a callback.
 *
 * Message layout on QRTR (no QMUX header):
 *   u8 flags (0 request, 2 response, 4 indication) | u16 txn | u16 msg id |
 *   u16 TLV length | TLVs: u8 type, u16 length, value
 */
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* QMI service ids */
#define QMI_WDS   0x01
#define QMI_DMS   0x02
#define QMI_NAS   0x03
#define QMI_WMS   0x05
#define QMI_VOICE 0x09
#define QMI_UIM   0x0b
#define QMI_WDA   0x1a
#define QMI_DPM   0x2f

#define QMI_MAX_MSG 8192

struct qmi_msg {
    uint8_t buf[QMI_MAX_MSG];
    size_t len;            /* bytes used, header included */
};

struct qmi_client;
typedef void (*qmi_ind_cb)(struct qmi_client *c, uint16_t msg_id,
                           const uint8_t *tlvs, size_t len, void *user);

struct qmi_client {
    int fd;
    uint32_t service, node, port;
    uint16_t next_txn;
    pthread_t reader;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    /* the one request waiting for its response (requests are serialised) */
    uint16_t wait_txn;
    int wait_done;
    struct qmi_msg *wait_resp;
    pthread_mutex_t req_lock;
    qmi_ind_cb ind_cb;
    void *ind_user;
    int running;
};

/* Look the service up on the QRTR name service and open a client.
 * Returns 0 or -errno. */
int qmi_client_open(struct qmi_client *c, uint32_t service, int timeout_ms,
                    qmi_ind_cb cb, void *user);
void qmi_client_close(struct qmi_client *c);

/* Build a request. */
void qmi_msg_init(struct qmi_msg *m, uint16_t msg_id);
int qmi_tlv_add(struct qmi_msg *m, uint8_t type, const void *data, uint16_t len);
int qmi_tlv_add_u8(struct qmi_msg *m, uint8_t type, uint8_t v);
int qmi_tlv_add_u16(struct qmi_msg *m, uint8_t type, uint16_t v);
int qmi_tlv_add_u32(struct qmi_msg *m, uint8_t type, uint32_t v);

/* Send a request and wait for its response. Returns 0 when the transport
 * worked; *qmi_err (if non-NULL) receives the QMI error code from the
 * result TLV (0 = success). resp may be NULL. */
int qmi_request(struct qmi_client *c, struct qmi_msg *req, struct qmi_msg *resp,
                int timeout_ms, uint16_t *qmi_err);

/* Convenience: request with no TLVs. */
int qmi_simple(struct qmi_client *c, uint16_t msg_id, struct qmi_msg *resp,
               uint16_t *qmi_err);

/* TLV access on a received message (response or indication payload). */
const uint8_t *qmi_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, uint16_t *vlen);
const uint8_t *qmi_msg_tlvs(const struct qmi_msg *m, size_t *len);
uint16_t qmi_msg_id(const struct qmi_msg *m);

static inline uint16_t qmi_le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static inline uint32_t qmi_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

extern void (*qmi_log)(const char *fmt, ...);
