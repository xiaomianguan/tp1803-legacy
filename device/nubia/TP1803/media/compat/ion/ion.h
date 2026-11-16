/*
 * The three libion calls the OMX components make, implemented for the 4.14
 * kernel's ION ABI (ALLOC returns a dma-buf fd) in compat/ion_compat.c.
 * CM13's libion speaks the pre-4.12 handle ABI.
 */
#ifndef __TP1803_ION_COMPAT_H
#define __TP1803_ION_COMPAT_H
#include <stddef.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>

#ifdef __cplusplus
extern "C" {
#endif
int ion_open(void);
int ion_close(int fd);
int ion_alloc_fd(int fd, size_t len, size_t align, unsigned int heap_mask,
                 unsigned int flags, int *handle_fd);
#ifdef __cplusplus
}
#endif
#endif
