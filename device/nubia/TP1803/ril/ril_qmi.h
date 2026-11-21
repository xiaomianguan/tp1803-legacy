/* Shared state of the TP1803 QMI RIL. */
#pragma once
#include <telephony/ril.h>
#include "qmi.h"

extern const struct RIL_Env *s_rilenv;
#define RIL_onRequestComplete(t, e, response, len) \
    s_rilenv->OnRequestComplete(t, e, response, len)
#define RIL_onUnsolicitedResponse(a, b, c) \
    s_rilenv->OnUnsolicitedResponse(a, b, c)

extern struct qmi_client q_dms, q_nas, q_uim, q_voice, q_wms, q_wds;

/* ril_sim.c */
int sim_init(void);
int sim_refresh(void);
int sim_ready(void);
int sim_present(void);
void sim_get_status(RIL_Token t);
void sim_io(RIL_Token t, RIL_SIM_IO_v6 *io);
void sim_get_imsi(RIL_Token t);
int sim_read_iccid(char *out, size_t n);
void sim_pin_op(RIL_Token t, int request, char **data);
void sim_query_facility(RIL_Token t, char **data);
void sim_set_facility(RIL_Token t, char **data);

/* ril.c */
void ril_sim_changed(void);

/* ril_voice.c */
int voice_init(void);
void voice_get_current_calls(RIL_Token t);
void voice_request(int request, void *data, RIL_Token t);

/* ril_data.c */
int data_init(void);
void data_request(int request, void *data, size_t datalen, RIL_Token t);

/* ril_sms.c */
int sms_init(void);
void sms_request(int request, void *data, size_t datalen, RIL_Token t);
