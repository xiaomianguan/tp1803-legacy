/*
 * kmstest: freestanding (no libc) aarch64 Linux test of legacy KMS on msm SDE DRM.
 * Opens /dev/dri/card0, finds a connected connector, creates two dumb buffers,
 * SetCrtc to the first, then page-flips between them a few times.
 * Build: see build.sh. Output goes to stdout (fd 1).
 */
#include <stdint.h>
#include <stddef.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

/* ---- raw syscalls (aarch64) ---- */
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_ioctl 29
#define SYS_mmap 222
#define SYS_nanosleep 101
#define SYS_exit_group 94
#define SYS_ppoll 73

static int sys_open(const char *p, int fl) { return sc6(SYS_openat, -100, (long)p, fl, 0, 0, 0); }
static long sys_ioctl(int fd, unsigned long req, void *arg) { return sc6(SYS_ioctl, fd, req, (long)arg, 0, 0, 0); }
static void *sys_mmap(void *a, size_t l, int prot, int fl, int fd, long off) { return (void *)sc6(SYS_mmap, (long)a, l, prot, fl, fd, off); }
static long sys_write(int fd, const void *b, size_t n) { return sc6(SYS_write, fd, (long)b, n, 0, 0, 0); }
static long sys_read(int fd, void *b, size_t n) { return sc6(SYS_read, fd, (long)b, n, 0, 0, 0); }
static void msleep(long ms) { struct { long s, ns; } ts = { ms / 1000, (ms % 1000) * 1000000 }; sc6(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0); }

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static void puts_(const char *s) { size_t n = 0; while (s[n]) n++; sys_write(1, s, n); }
static void putn(const char *k, long v) {
    char b[32]; int i = 31; b[i] = 0; int neg = v < 0; unsigned long u = neg ? -v : v;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (neg) b[--i] = '-';
    puts_(k); puts_(&b[i]); puts_("\n");
}

static uint32_t conn_ids[16], crtc_ids[16], enc_ids[16];
static struct drm_mode_modeinfo modes[32];
static uint32_t encs[16];

struct buf { uint32_t handle, pitch, fb; uint64_t size; uint32_t *map; };

static int mkbuf(int fd, int w, int h, struct buf *b) {
    struct drm_mode_create_dumb c = { .height = h, .width = w, .bpp = 32 };
    long r = sys_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c);
    if (r) { putn("CREATE_DUMB failed ", r); return -1; }
    b->handle = c.handle; b->pitch = c.pitch; b->size = c.size;
    struct drm_mode_fb_cmd2 f = { .width = w, .height = h, .pixel_format = 0x34325258 /* XR24 */ };
    f.handles[0] = c.handle; f.pitches[0] = c.pitch;
    r = sys_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f);
    if (r) { putn("ADDFB2 failed ", r); return -1; }
    b->fb = f.fb_id;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    r = sys_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
    if (r) { putn("MAP_DUMB failed ", r); return -1; }
    b->map = sys_mmap(0, c.size, 3, 1 /*MAP_SHARED*/, fd, m.offset);
    if ((long)b->map < 0 && (long)b->map > -4096) { putn("mmap failed ", (long)b->map); return -1; }
    putn("buf pitch=", c.pitch); putn("buf fb=", b->fb);
    return 0;
}

static void fill(struct buf *b, int w, int h, int phase) {
    static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((char *)b->map + (size_t)y * b->pitch);
        for (int x = 0; x < w; x++) row[x] = bars[((x * 8) / w + phase) & 7];
    }
}

int main(void) {
    int fd = sys_open("/dev/dri/card0", 2 /*O_RDWR*/ | 02000000 /*O_CLOEXEC*/);
    if (fd < 0) { putn("open card0 failed ", fd); return 1; }
    putn("SET_MASTER ", sys_ioctl(fd, DRM_IOCTL_SET_MASTER, 0));
    struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0 };

    struct drm_mode_card_res res = {0};
    sys_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    putn("crtcs ", res.count_crtcs); putn("conns ", res.count_connectors); putn("encs ", res.count_encoders);
    if (res.count_connectors > 16 || res.count_crtcs > 16 || res.count_encoders > 16) return 2;
    res.connector_id_ptr = (uint64_t)conn_ids; res.crtc_id_ptr = (uint64_t)crtc_ids; res.encoder_id_ptr = (uint64_t)enc_ids;
    res.count_fbs = 0;
    putn("GETRESOURCES ", sys_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res));
    (void)cap;

    uint32_t conn = 0, enc = 0; struct drm_mode_modeinfo mode;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c = { .connector_id = conn_ids[i] };
        sys_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        putn("connector ", conn_ids[i]); putn("  type ", c.connector_type); putn("  connection ", c.connection);
        putn("  modes ", c.count_modes); putn("  encoder_id ", c.encoder_id);
        if (c.count_modes > 32) c.count_modes = 32;
        if (c.count_encoders > 16) c.count_encoders = 16;
        c.modes_ptr = (uint64_t)modes; c.encoders_ptr = (uint64_t)encs; c.count_props = 0;
        sys_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c);
        if (c.connection == 1 && c.count_modes > 0 && !conn) {
            conn = conn_ids[i]; mode = modes[0]; enc = c.encoder_id ? c.encoder_id : encs[0];
            for (uint32_t m = 0; m < c.count_modes; m++) { putn("  mode h=", modes[m].hdisplay); putn("       v=", modes[m].vdisplay); putn("       vrefresh=", modes[m].vrefresh); }
        }
    }
    if (!conn) { puts_("no connected connector\n"); return 3; }
    struct drm_mode_get_encoder e = { .encoder_id = enc };
    putn("GETENCODER ", sys_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e));
    putn("encoder crtc ", e.crtc_id); putn("possible_crtcs ", e.possible_crtcs);
    uint32_t crtc = e.crtc_id;
    if (!crtc) for (uint32_t i = 0; i < res.count_crtcs; i++) if (e.possible_crtcs & (1u << i)) { crtc = crtc_ids[i]; break; }
    putn("using crtc ", crtc);

    int w = mode.hdisplay, h = mode.vdisplay;
    struct buf b[2];
    if (mkbuf(fd, w, h, &b[0]) || mkbuf(fd, w, h, &b[1])) return 4;
    fill(&b[0], w, h, 0); fill(&b[1], w, h, 4);

    struct drm_mode_crtc sc = { .crtc_id = crtc, .fb_id = b[0].fb, .set_connectors_ptr = (uint64_t)&conn, .count_connectors = 1, .mode_valid = 1, .mode = mode };
    putn("SETCRTC ", sys_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc));
    msleep(2000);
    for (int i = 0; i < 6; i++) {
        struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc, .fb_id = b[(i + 1) & 1].fb, .flags = DRM_MODE_PAGE_FLIP_EVENT };
        long r = sys_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
        putn("PAGE_FLIP ", r);
        if (r == 0) { char ev[256]; long n = sys_read(fd, ev, sizeof ev); putn("  event bytes ", n); }
        msleep(700);
    }
    /* Also try SETCRTC-only flipping as a fallback path */
    for (int i = 0; i < 4; i++) { sc.fb_id = b[i & 1].fb; putn("SETCRTC flip ", sys_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)); msleep(700); }
    puts_("done\n");
    return 0;
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("bl main\n mov x8, #94\n svc 0\n");
}
