/* Bring the WCN399x up from the shell (as root), print what it reports,
 * then power it down. usage: bt-init-test [oper_baud] */
#include <stdio.h>
#include <stdlib.h>
#include "qca_wcn399x.h"

int main(int argc, char **argv)
{
    unsigned int baud = argc > 1 ? (unsigned int)atoi(argv[1]) : 3000000;
    int fd = qca_wcn399x_open(baud, NULL, 0);
    if (fd < 0) {
        fprintf(stderr, "bring-up failed: %d\n", fd);
        return 1;
    }
    fprintf(stderr, "OK, fd %d\n", fd);
    qca_wcn399x_close(fd);
    return 0;
}
