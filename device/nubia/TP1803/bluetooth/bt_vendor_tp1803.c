/*
 * libbt-vendor for the TP1803's WCN399x: the whole chip bring-up (power,
 * firmware, baud rate) happens in BT_VND_OP_USERIAL_OPEN. The stack then
 * gets an H4 socket served by the IBS relay (ibs_proxy.c), which runs
 * Qualcomm's in-band sleep so the UART clocks - and the system - can sleep
 * while Bluetooth is idle. persist.bt.tp1803.ibs=0 falls back to a plain
 * UART with IBS off (works, but blocks suspend while Bluetooth is on).
 */
#define LOG_TAG "bt_vendor_tp1803"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/properties.h>

#include "bt_vendor_lib.h"
#include "ibs_proxy.h"
#include "qca_wcn399x.h"

#define OPER_BAUD 3000000

static const bt_vendor_callbacks_t *cb;
static unsigned char bdaddr[6];
static int have_bdaddr;
static int uart_fd = -1;
static int stack_fd = -1;     /* IBS relay socket handed to the stack */

static void shutdown_uart(void)
{
    if (stack_fd >= 0) {
        ibs_proxy_stop();
        close(stack_fd);
        stack_fd = -1;
    }
    if (uart_fd >= 0) {
        qca_wcn399x_close(uart_fd);
        uart_fd = -1;
    }
}

static void log_android(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LOG_PRI_VA(ANDROID_LOG_INFO, LOG_TAG, fmt, ap);
    va_end(ap);
}

static int vnd_init(const bt_vendor_callbacks_t *p_cb, unsigned char *local_bdaddr)
{
    cb = p_cb;
    qca_log = log_android;
    have_bdaddr = 0;
    if (local_bdaddr) {
        memcpy(bdaddr, local_bdaddr, sizeof(bdaddr));
        int i;
        for (i = 0; i < 6; i++)
            if (bdaddr[i])
                have_bdaddr = 1;
    }
    ALOGI("init, bdaddr %02x:%02x:%02x:%02x:%02x:%02x", bdaddr[0], bdaddr[1], bdaddr[2],
          bdaddr[3], bdaddr[4], bdaddr[5]);
    return 0;
}

static int vnd_op(bt_vendor_opcode_t opcode, void *param)
{
    switch (opcode) {
    case BT_VND_OP_POWER_CTRL: {
        int state = param ? *(int *)param : BT_VND_PWR_OFF;
        ALOGI("power %s", state == BT_VND_PWR_ON ? "on" : "off");
        if (state == BT_VND_PWR_OFF)
            shutdown_uart();
        /* Power-on happens in USERIAL_OPEN together with firmware download. */
        return 0;
    }
    case BT_VND_OP_USERIAL_OPEN: {
        int (*fds)[CH_MAX] = param;
        int ibs = property_get_bool("persist.bt.tp1803.ibs", 1);
        shutdown_uart();
        uart_fd = qca_wcn399x_open(OPER_BAUD, have_bdaddr ? bdaddr : NULL, ibs);
        if (uart_fd < 0) {
            ALOGE("controller bring-up failed: %d", uart_fd);
            return -1;
        }
        int fd = uart_fd;
        if (ibs) {
            stack_fd = ibs_proxy_start(uart_fd);
            if (stack_fd < 0) {
                ALOGE("IBS relay failed: %d", stack_fd);
                shutdown_uart();
                return -1;
            }
            fd = stack_fd;
        }
        int ch;
        for (ch = 0; ch < CH_MAX; ch++)
            (*fds)[ch] = fd;
        return 1;                       /* one fd: H4 */
    }
    case BT_VND_OP_USERIAL_CLOSE:
        shutdown_uart();
        return 0;
    case BT_VND_OP_FW_CFG:
        /* Firmware was downloaded in USERIAL_OPEN. */
        if (cb)
            cb->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_SCO_CFG:
        if (cb)
            cb->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
        if (param)
            *(uint32_t *)param = 3000;
        return 0;
    case BT_VND_OP_LPM_SET_MODE:
        if (cb)
            cb->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_LPM_WAKE_SET_STATE:
        return 0;
    case BT_VND_OP_SET_AUDIO_STATE:
        if (cb && cb->audio_state_cb)
            cb->audio_state_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_EPILOG:
        if (cb)
            cb->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    default:
        ALOGW("unhandled op %d", opcode);
        return -1;
    }
}

static void vnd_cleanup(void)
{
    shutdown_uart();
    cb = NULL;
}

static void vnd_ssr_cleanup(int reason)
{
    (void)reason;
    vnd_cleanup();
}

const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    vnd_init,
    vnd_op,
    vnd_cleanup,
    vnd_ssr_cleanup,
};
