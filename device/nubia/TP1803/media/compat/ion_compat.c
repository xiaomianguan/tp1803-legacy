#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ion/ion.h>

int ion_open(void)
{
    int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

int ion_close(int fd)
{
    return close(fd) < 0 ? -errno : 0;
}

int ion_alloc_fd(int fd, size_t len, size_t align, unsigned int heap_mask,
                 unsigned int flags, int *handle_fd)
{
    (void)align;
    struct ion_allocation_data d = {
        .len = len, .heap_id_mask = heap_mask, .flags = flags,
    };
    if (ioctl(fd, ION_IOC_ALLOC, &d) < 0)
        return -errno;
    *handle_fd = (int)d.fd;
    return 0;
}
