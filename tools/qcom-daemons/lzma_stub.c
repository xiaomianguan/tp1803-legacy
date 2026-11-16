/* pd-mapper only needs liblzma for .jsn.xz maps; the firmware ships plain .jsn */
#include <errno.h>
int lzma_decomp(const char *file) { (void)file; errno = ENOSYS; return -1; }
