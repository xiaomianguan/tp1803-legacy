/*
 * Run hexagonrpcd (written for the mainline fastrpc driver) on the downstream
 * msm-4.14 "adsprpc" driver. Linked with
 *   -Wl,--wrap=open -Wl,--wrap=ioctl
 *
 * open("/dev/fastrpc-{adsp,mdsp,sdsp,cdsp}") opens /dev/adsprpc-smd-secure
 * (attaching to a static PD is refused on the non-secure node) and remembers
 * the channel id; the mainline ioctls are translated to the downstream ones.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <misc/fastrpc.h>

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, int req, ...);

/* downstream adsprpc_shared.h */
struct ds_remote_buf { void *pv; size_t len; };
struct ds_invoke { uint32_t handle; uint32_t sc; struct ds_remote_buf *pra; };
struct ds_invoke_fd { struct ds_invoke inv; int *fds; };
struct ds_init {
	uint32_t flags; uintptr_t file; uint32_t filelen; int32_t filefd;
	uintptr_t mem; uint32_t memlen; int32_t memfd;
};
struct ds_mmap_64 { int fd; uint32_t flags; uint64_t vaddrin; size_t size; uint64_t vaddrout; };

#define DS_IOCTL_INVOKE_FD _IOWR('R', 4, struct ds_invoke_fd)
#define DS_IOCTL_INIT      _IOWR('R', 6, struct ds_init)
#define DS_IOCTL_GETINFO   _IOWR('R', 8, uint32_t)
#define DS_IOCTL_MMAP_64   _IOWR('R', 14, struct ds_mmap_64)
#define DS_INIT_ATTACH         0
#define DS_INIT_CREATE_STATIC  2
#define DS_INIT_ATTACH_SENSORS 3

#define MAX_FDS 1024
static signed char chan_of_fd[MAX_FDS];

int __wrap_open(const char *path, int flags, ...)
{
	static const char *const names[] = { "adsp", "mdsp", "sdsp", "cdsp" };
	mode_t mode = 0;
	va_list ap;

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = va_arg(ap, int);
	va_end(ap);

	if (!strncmp(path, "/dev/fastrpc-", 13)) {
		for (int cid = 0; cid < 4; cid++) {
			if (strcmp(path + 13, names[cid]))
				continue;
			int fd = __real_open("/dev/adsprpc-smd-secure", flags);
			if (fd >= 0 && fd < MAX_FDS)
				chan_of_fd[fd] = cid + 1;
			return fd;
		}
	}
	return __real_open(path, flags, mode);
}

static int set_channel(int fd)
{
	uint32_t cid;

	if (fd < 0 || fd >= MAX_FDS || !chan_of_fd[fd]) {
		errno = EINVAL;
		return -1;
	}
	cid = chan_of_fd[fd] - 1;
	return __real_ioctl(fd, DS_IOCTL_GETINFO, &cid);
}

static int do_invoke(int fd, struct fastrpc_invoke *in)
{
	struct fastrpc_invoke_args *a = (void *)(uintptr_t)in->args;
	unsigned n = ((in->sc >> 16) & 0xff) + ((in->sc >> 8) & 0xff) +
		     ((in->sc >> 4) & 0xf) + (in->sc & 0xf);
	struct ds_remote_buf pra[n ? n : 1];
	int fds[n ? n : 1];
	struct ds_invoke_fd inv = { { in->handle, in->sc, pra }, fds };

	for (unsigned i = 0; i < n; i++) {
		pra[i].pv = (void *)(uintptr_t)a[i].ptr;
		pra[i].len = a[i].length;
		fds[i] = a[i].fd > 0 ? a[i].fd : -1;
	}
	return __real_ioctl(fd, DS_IOCTL_INVOKE_FD, &inv);
}

int __wrap_ioctl(int fd, int req, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	switch ((unsigned)req) {
	case FASTRPC_IOCTL_INIT_ATTACH:
	case FASTRPC_IOCTL_INIT_ATTACH_SNS: {
		struct ds_init init = { .flags = (unsigned)req == FASTRPC_IOCTL_INIT_ATTACH ?
					DS_INIT_ATTACH : DS_INIT_ATTACH_SENSORS,
					.filefd = -1, .memfd = -1 };
		if (set_channel(fd))
			return -1;
		return __real_ioctl(fd, DS_IOCTL_INIT, &init);
	}
	case FASTRPC_IOCTL_INIT_CREATE_STATIC: {
		/* downstream: name in file/filelen, remote heap size in memlen
		 * (the kernel allocates it from the remote-heap carveout) */
		struct fastrpc_init_create_static *st = arg;
		struct ds_init init = { .flags = DS_INIT_CREATE_STATIC,
					.file = (uintptr_t)st->name, .filelen = st->namelen,
					.filefd = -1, .memlen = st->memlen, .memfd = -1 };
		if (set_channel(fd))
			return -1;
		return __real_ioctl(fd, DS_IOCTL_INIT, &init);
	}
	case FASTRPC_IOCTL_INVOKE:
		return do_invoke(fd, arg);
	case FASTRPC_IOCTL_MMAP: {
		struct fastrpc_req_mmap *m = arg;
		struct ds_mmap_64 d = { m->fd, m->flags, m->vaddrin, m->size, 0 };
		int ret = __real_ioctl(fd, DS_IOCTL_MMAP_64, &d);
		if (!ret)
			m->vaddrout = d.vaddrout;
		return ret;
	}
	case FASTRPC_IOCTL_INIT_CREATE:
	case FASTRPC_IOCTL_ALLOC_DMA_BUFF:
		errno = EOPNOTSUPP;
		return -1;
	default:
		return __real_ioctl(fd, req, arg);
	}
}
