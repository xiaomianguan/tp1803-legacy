/*
 * Bring-up of the Qualcomm WCN399x ("Cherokee") Bluetooth SoC on the
 * TP1803: power via the downstream bt_power driver (/dev/btpower), HCI over
 * the GENI high-speed UART (/dev/ttyHS0), rampatch + NVM download with
 * Qualcomm's EDL vendor commands. Follows Linux btqca.c/hci_qca.c.
 * In-band sleep (IBS) is either enabled in the NVM (then ibs_proxy.c must
 * run the IBS protocol on the UART) or disabled (plain H4, but the UART
 * clock vote then blocks system suspend while Bluetooth is on).
 */
#pragma once
#include <stdint.h>

#define QCA_BT_POWER_DEV  "/dev/btpower"
#define QCA_BT_UART_DEV   "/dev/ttyHS0"
#define QCA_BT_FW_DIR     "/bt_firmware/image"

/* Power the SoC up, download firmware, reset it and switch the UART to
 * oper_baud. bdaddr (6 bytes, most significant first) may be NULL to keep
 * the NVM default. Returns an fd ready for H4 traffic, or -errno. */
int qca_wcn399x_open(unsigned int oper_baud, const uint8_t *bdaddr, int ibs);

/* Vote the GENI UART clocks on/off (TIOCPMGET/TIOCPMPUT). The open UART
 * holds one vote on return from qca_wcn399x_open(). */
int qca_uart_clock(int fd, int on);

/* Close the UART and power the SoC down. */
void qca_wcn399x_close(int fd);

/* Optional logger hook (defaults to stderr / Android log). */
extern void (*qca_log)(const char *fmt, ...);
