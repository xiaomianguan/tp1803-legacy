/*
 * Minimal client for Qualcomm's Snapdragon Sensor Core (SEE) on the SLPI:
 * QMI service SNS_CLIENT (400) over QRTR, carrying protobuf messages
 * (sns_client.proto / sns_suid.proto / sns_std_sensor.proto).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSC_SERVICE 400

/* protobuf message ids */
#define SNS_SUID_MSGID_SNS_SUID_REQ               512
#define SNS_SUID_MSGID_SNS_SUID_EVENT             768
#define SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_CONFIG 513
#define SNS_STD_SENSOR_MSGID_SNS_STD_ON_CHANGE_CONFIG 514
#define SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_EVENT 1025
#define SNS_PROXIMITY_MSGID_SNS_PROXIMITY_EVENT   1024
#define SNS_CLIENT_MSGID_SNS_CLIENT_DISABLE_REQ   10
#define SNS_CAL_MSGID_SNS_CAL_EVENT               1022
#define SNS_STD_MSGID_SNS_STD_FLUSH_EVENT         1026
#define SNS_STD_SENSOR_MSGID_SNS_STD_SENSOR_INITIAL_EVENT 775 /* on-change initial state */
#define SNS_PEDOMETER_MSGID_SNS_STEP_EVENT        1028
#define SNS_TRIGGER_EVENT                         774  /* tilt / sig_motion: empty payload */

struct ssc_suid { uint64_t low, high; };

struct ssc;

/* event callback: suid, protobuf msg id, timestamp (QTimer ticks), payload */
typedef void (*ssc_event_cb)(void *ctx, const struct ssc_suid *suid, uint32_t msg_id,
                             uint64_t timestamp, const uint8_t *payload, size_t len);

struct ssc *ssc_open(ssc_event_cb cb, void *ctx);
void ssc_close(struct ssc *s);
/* Resolve a data type ("accel", "gyro", "mag", "ambient_light", "proximity") */
int ssc_lookup(struct ssc *s, const char *data_type, struct ssc_suid *out, int timeout_ms);
/* Start streaming: rate_hz > 0 for streaming sensors, 0 for on-change */
int ssc_enable(struct ssc *s, const struct ssc_suid *suid, float rate_hz);
int ssc_disable(struct ssc *s, const struct ssc_suid *suid);
/* Pump indications; returns after timeout_ms or when an event was handled */
int ssc_poll(struct ssc *s, int timeout_ms);
int ssc_fd(struct ssc *s);
/* Deliver this client's events even while the AP is suspended */
void ssc_set_wakeup(struct ssc *s, int wakeup);

/* protobuf helpers for decoding event payloads */
int pb_floats(const uint8_t *p, size_t len, int field, float *out, int max);
int pb_varint_field(const uint8_t *p, size_t len, int field, uint64_t *out);
