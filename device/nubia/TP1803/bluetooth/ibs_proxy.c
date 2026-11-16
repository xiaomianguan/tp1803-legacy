/*
 * Qualcomm in-band sleep (IBS) relay between Android 6's Bluetooth stack
 * and the WCN399x UART.
 *
 * The stack talks plain H4 on one end of a socketpair; this thread frames
 * H4 packets in both directions and runs the IBS protocol on the UART
 * between packets (as Linux hci_qca.c does in the kernel):
 *
 *   0xFD WAKE_IND   either side announces it is about to send
 *   0xFC WAKE_ACK   reply to WAKE_IND
 *   0xFE SLEEP_IND  sender has gone idle
 *
 * When both directions are asleep the UART clock vote is dropped, which lets
 * the system suspend; the GENI driver wakes on RX activity and injects the
 * chip's WAKE_IND (0xFD "wakeup byte").
 */
#define _GNU_SOURCE
#include "ibs_proxy.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "qca_wcn399x.h"

#define LOG(...) qca_log(__VA_ARGS__)

#define IBS_SLEEP_IND 0xFE
#define IBS_WAKE_IND  0xFD
#define IBS_WAKE_ACK  0xFC

#define WAKE_RETRANS_MS 100
#define TX_IDLE_MS      2000

enum tx_state { TX_ASLEEP, TX_WAKING, TX_AWAKE };

/* Incremental H4 framer: tells which bytes belong to packets. */
struct h4_framer {
    int need;           /* bytes still missing in the current stage */
    int stage;          /* 0 = type, 1 = header, 2 = payload */
    uint8_t type;
    uint8_t hdr[4];
    int hdr_len, hdr_got;
};

struct ibs {
    int uart, sock, stop_pipe[2];
    pthread_t thread;
    int running;

    enum tx_state tx;
    int rx_awake;
    int clock_on;
    long long tx_deadline;          /* idle timeout or wake retransmit */

    /* stack -> chip data held while waking the chip */
    uint8_t txbuf[65536];
    size_t txlen;
    struct h4_framer tx_frame, rx_frame;
};

static struct ibs g;

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void framer_reset(struct h4_framer *f)
{
    memset(f, 0, sizeof(*f));
}

/* Feed one byte; returns 1 if it is part of a packet, 0 if it arrived
 * between packets (an IBS byte or noise). */
static int framer_feed(struct h4_framer *f, uint8_t b)
{
    switch (f->stage) {
    case 0:
        switch (b) {
        case 0x01: f->hdr_len = 3; break;   /* command: opcode(2) plen(1) */
        case 0x02: f->hdr_len = 4; break;   /* ACL: handle(2) len(2) */
        case 0x03: f->hdr_len = 3; break;   /* SCO: handle(2) len(1) */
        case 0x04: f->hdr_len = 2; break;   /* event: code(1) plen(1) */
        default: return 0;
        }
        f->type = b;
        f->hdr_got = 0;
        f->stage = 1;
        return 1;
    case 1:
        f->hdr[f->hdr_got++] = b;
        if (f->hdr_got == f->hdr_len) {
            switch (f->type) {
            case 0x01: f->need = f->hdr[2]; break;
            case 0x02: f->need = f->hdr[2] | f->hdr[3] << 8; break;
            case 0x03: f->need = f->hdr[2]; break;
            case 0x04: f->need = f->hdr[1]; break;
            }
            f->stage = f->need ? 2 : 0;
        }
        return 1;
    default:
        if (--f->need == 0)
            f->stage = 0;
        return 1;
    }
}

static int at_boundary(const struct h4_framer *f) { return f->stage == 0; }

static void clock_update(void)
{
    int want = g.tx != TX_ASLEEP || g.rx_awake;
    if (want != g.clock_on) {
        qca_uart_clock(g.uart, want);
        g.clock_on = want;
    }
}

static int uart_write(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(g.uart, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                usleep(500);
                continue;
            }
            return -errno;
        }
        p += w;
        n -= w;
    }
    return 0;
}

static void send_ibs(uint8_t b)
{
    uart_write(&b, 1);
}

static void flush_tx(void)
{
    if (g.txlen) {
        uart_write(g.txbuf, g.txlen);
        g.txlen = 0;
    }
    g.tx_deadline = now_ms() + TX_IDLE_MS;
}

/* Stack wants to send: make sure the chip is awake. */
static void tx_wake(void)
{
    switch (g.tx) {
    case TX_AWAKE:
        g.tx_deadline = now_ms() + TX_IDLE_MS;
        break;
    case TX_ASLEEP:
        g.tx = TX_WAKING;
        clock_update();
        send_ibs(IBS_WAKE_IND);
        g.tx_deadline = now_ms() + WAKE_RETRANS_MS;
        break;
    case TX_WAKING:
        break;
    }
}

static void on_timer(void)
{
    if (g.tx == TX_WAKING) {
        send_ibs(IBS_WAKE_IND);
        g.tx_deadline = now_ms() + WAKE_RETRANS_MS;
    } else if (g.tx == TX_AWAKE) {
        if (g.txlen || !at_boundary(&g.tx_frame)) {
            g.tx_deadline = now_ms() + TX_IDLE_MS;
            return;
        }
        send_ibs(IBS_SLEEP_IND);
        g.tx = TX_ASLEEP;
        g.tx_deadline = 0;
        clock_update();
    }
}

static void on_uart_byte_outside_packet(uint8_t b)
{
    switch (b) {
    case IBS_WAKE_IND:
        if (!g.rx_awake) {
            g.rx_awake = 1;
            clock_update();
        }
        send_ibs(IBS_WAKE_ACK);
        break;
    case IBS_SLEEP_IND:
        g.rx_awake = 0;
        clock_update();
        break;
    case IBS_WAKE_ACK:
        if (g.tx == TX_WAKING) {
            g.tx = TX_AWAKE;
            flush_tx();
        }
        break;
    default:
        LOG("ibs: stray byte 0x%02x from controller", b);
    }
}

static void *relay(void *arg)
{
    (void)arg;
    uint8_t buf[4096], out[4096];
    while (g.running) {
        struct pollfd pf[3] = {
            { g.uart, POLLIN, 0 },
            { g.sock, (g.txlen < sizeof(g.txbuf) - sizeof(buf)) ? POLLIN : 0, 0 },
            { g.stop_pipe[0], POLLIN, 0 },
        };
        int timeout = -1;
        if (g.tx_deadline) {
            long long left = g.tx_deadline - now_ms();
            timeout = left > 0 ? (int)left : 0;
        }
        int r = poll(pf, 3, timeout);
        if (r < 0 && errno != EINTR)
            break;
        if (pf[2].revents)
            break;
        if (g.tx_deadline && now_ms() >= g.tx_deadline)
            on_timer();

        if (pf[0].revents & POLLIN) {
            ssize_t n = read(g.uart, buf, sizeof(buf));
            if (n > 0) {
                size_t o = 0;
                for (ssize_t i = 0; i < n; i++) {
                    if (framer_feed(&g.rx_frame, buf[i]))
                        out[o++] = buf[i];
                    else
                        on_uart_byte_outside_packet(buf[i]);
                }
                /* Data from an asleep chip implies it is awake now. */
                if (o) {
                    if (!g.rx_awake) {
                        g.rx_awake = 1;
                        clock_update();
                    }
                    size_t w = 0;
                    while (w < o) {
                        ssize_t k = write(g.sock, out + w, o - w);
                        if (k < 0) {
                            if (errno == EINTR || errno == EAGAIN)
                                continue;
                            break;
                        }
                        w += k;
                    }
                }
            }
        }

        if (pf[1].revents & (POLLHUP | POLLERR))
            break;
        if (pf[1].revents & POLLIN) {
            ssize_t n = read(g.sock, buf, sizeof(buf));
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; i++)
                framer_feed(&g.tx_frame, buf[i]);
            tx_wake();
            if (g.tx == TX_AWAKE) {
                uart_write(buf, n);
                g.tx_deadline = now_ms() + TX_IDLE_MS;
            } else {
                memcpy(g.txbuf + g.txlen, buf, n);
                g.txlen += n;
            }
        }
    }
    LOG("ibs: relay stopped");
    return NULL;
}

int ibs_proxy_start(int uart_fd)
{
    int sv[2];
    memset(&g, 0, sizeof(g));
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return -errno;
    if (pipe2(g.stop_pipe, O_CLOEXEC) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -errno;
    }
    g.uart = uart_fd;
    g.sock = sv[0];
    /* The open UART holds a clock vote and both sides are awake after the
     * bring-up; the idle timer will put the TX side to sleep. */
    g.tx = TX_AWAKE;
    g.rx_awake = 1;
    g.clock_on = 1;
    g.tx_deadline = now_ms() + TX_IDLE_MS;
    g.running = 1;
    if (pthread_create(&g.thread, NULL, relay, NULL)) {
        close(sv[0]);
        close(sv[1]);
        return -EAGAIN;
    }
    LOG("ibs: relay started");
    return sv[1];
}

void ibs_proxy_stop(void)
{
    if (!g.running)
        return;
    g.running = 0;
    write(g.stop_pipe[1], "x", 1);
    pthread_join(g.thread, NULL);
    close(g.sock);
    close(g.stop_pipe[0]);
    close(g.stop_pipe[1]);
}
