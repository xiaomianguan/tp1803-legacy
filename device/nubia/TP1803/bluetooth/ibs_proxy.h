#pragma once
/* Start the IBS relay on an initialised WCN399x UART (opened with IBS
 * enabled). Returns the fd the Bluetooth stack should use as its H4 UART,
 * or -errno. */
int ibs_proxy_start(int uart_fd);
/* Stop the relay (the stack-side fd is closed by the caller). */
void ibs_proxy_stop(void);
