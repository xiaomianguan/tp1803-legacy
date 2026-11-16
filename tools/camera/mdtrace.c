/* LD_PRELOAD debugging aid: tracks camera metadata buffer allocation/free
 * and, when update_camera_metadata_entry() gets a buffer that fails
 * validation, logs its header plus who allocated and freed it. */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct camera_metadata camera_metadata_t;
#define L(...) __android_log_print(ANDROID_LOG_ERROR, "mdtrace", __VA_ARGS__)

#define N 4096
static struct { void *p; void *alloc_ra; void *free_ra; int freed; } tab[N];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void *sym(const char *n) { return dlsym(RTLD_NEXT, n); }

static void where(void *ra, char *buf, size_t len)
{
    Dl_info di = {0};
    if (ra && dladdr(ra, &di) && di.dli_fname)
        snprintf(buf, len, "%s+0x%lx", strrchr(di.dli_fname, '/') + 1,
                 (unsigned long)((char *)ra - (char *)di.dli_fbase));
    else
        snprintf(buf, len, "%p", ra);
}

static void note_alloc(void *p, void *ra)
{
    if (!p) return;
    pthread_mutex_lock(&mu);
    size_t i = ((uintptr_t)p >> 4) % N;
    tab[i].p = p; tab[i].alloc_ra = ra; tab[i].free_ra = NULL; tab[i].freed = 0;
    pthread_mutex_unlock(&mu);
}

static void note_free(void *p, void *ra)
{
    if (!p) return;
    pthread_mutex_lock(&mu);
    size_t i = ((uintptr_t)p >> 4) % N;
    if (tab[i].p == p) { tab[i].free_ra = ra; tab[i].freed = 1; }
    else { tab[i].p = p; tab[i].alloc_ra = NULL; tab[i].free_ra = ra; tab[i].freed = 1; }
    pthread_mutex_unlock(&mu);
}

camera_metadata_t *allocate_camera_metadata(size_t e, size_t d)
{
    static camera_metadata_t *(*real)(size_t, size_t);
    if (!real) real = sym("allocate_camera_metadata");
    camera_metadata_t *p = real(e, d);
    note_alloc(p, __builtin_return_address(0));
    return p;
}

camera_metadata_t *clone_camera_metadata(const camera_metadata_t *s)
{
    static camera_metadata_t *(*real)(const camera_metadata_t *);
    if (!real) real = sym("clone_camera_metadata");
    camera_metadata_t *p = real(s);
    note_alloc(p, __builtin_return_address(0));
    return p;
}

void free_camera_metadata(camera_metadata_t *m)
{
    static void (*real)(camera_metadata_t *);
    if (!real) real = sym("free_camera_metadata");
    note_free(m, __builtin_return_address(0));
    real(m);
}

int update_camera_metadata_entry(camera_metadata_t *dst, size_t index, const void *data,
                                 size_t data_count, void *updated_entry)
{
    static int (*real)(camera_metadata_t *, size_t, const void *, size_t, void *);
    static int (*validate)(const camera_metadata_t *, const size_t *);
    if (!real) {
        real = sym("update_camera_metadata_entry");
        validate = sym("validate_camera_metadata_structure");
    }
    int bad = validate(dst, NULL) != 0;
    int r = bad ? -22 : real(dst, index, data, data_count, updated_entry);
    if (!bad && r != 0)
        bad = validate(dst, NULL) != 0;   /* went bad during the call */
    if (bad) {
        const uint32_t *h = (const uint32_t *)dst;
        L("hdr now %08x %08x %08x %08x", h[0], h[1], h[2], h[3]);
        usleep(2000);
        L("hdr +2ms %08x %08x %08x %08x valid=%d", h[0], h[1], h[2], h[3], validate(dst, NULL) == 0);
        char c[128], a[128], f[128];
        where(__builtin_return_address(0), c, sizeof(c));
        pthread_mutex_lock(&mu);
        size_t i = ((uintptr_t)dst >> 4) % N;
        int known = tab[i].p == (void *)dst;
        where(known ? tab[i].alloc_ra : NULL, a, sizeof(a));
        where(known ? tab[i].free_ra : NULL, f, sizeof(f));
        int freed = known && tab[i].freed;
        pthread_mutex_unlock(&mu);
        L("bad dst %p caller %s | known %d allocated by %s | freed %d by %s", dst, c, known, a, freed, f);
    }
    return r;
}
