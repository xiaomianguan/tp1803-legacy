/* Bring up an IPv4 data call by hand: data-test <apn> [seconds]
 * Leaves rmnet_data0 configured with a default route in table "dt" while
 * running, so `ping -I rmnet_data0` works. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "data_path.h"

static struct qmi_client wds, wda, dpm;

static const char *ip4(uint32_t a)
{
    static char b[4][16];
    static int i;
    i = (i + 1) & 3;
    snprintf(b[i], 16, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return b[i];
}

int main(int argc, char **argv)
{
    const char *apn = argc > 1 ? argv[1] : "internet";
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    if (qmi_client_open(&wda, QMI_WDA, 3000, NULL, NULL)) { printf("no WDA\n"); return 1; }
    if (qmi_client_open(&dpm, QMI_DPM, 3000, NULL, NULL)) printf("no DPM (continuing)\n");
    if (qmi_client_open(&wds, QMI_WDS, 3000, NULL, NULL)) { printf("no WDS\n"); return 1; }
    if (dp_link_setup(&wda, &dpm)) printf("link setup failed (continuing)\n");
    if (dp_mux_setup(0)) { printf("mux setup failed\n"); return 1; }
    if (dp_wds_bind(&wds, 0, 4)) printf("bind failed (continuing)\n");
    uint32_t h = 0, end = 0;
    if (dp_wds_start(&wds, apn, NULL, NULL, -1, 4, &h, &end)) {
        printf("start network failed, end reason 0x%x\n", end);
        return 1;
    }
    struct dp_settings s;
    if (dp_wds_settings(&wds, &s)) { printf("no settings\n"); return 1; }
    printf("IP %s/%d gw %s dns %s %s mtu %u handle 0x%x\n", ip4(s.v4_addr), s.v4_prefix,
           ip4(s.v4_gw), ip4(s.v4_dns[0]), ip4(s.v4_dns[1]), s.mtu, h);
    dp_if_up("rmnet_ipa0", 1, 0);
    dp_if_up("rmnet_data0", 1, s.mtu ? (int)s.mtu : 1500);
    printf("addr: %d\n", dp_if_set_ipv4("rmnet_data0", s.v4_addr, s.v4_prefix));
    fflush(stdout);
    sleep(secs);
    dp_wds_stop(&wds, h);
    dp_if_up("rmnet_data0", 0, 0);
    return 0;
}
