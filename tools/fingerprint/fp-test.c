/* Drive the fingerprint HAL directly: fp-test [auth|enroll|enum|remove] [seconds]
 * Loads /system/lib64/hw/fingerprint.qcom.so (our 2.0 shim). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t {
    uint32_t tag; uint32_t version; hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *);
} hw_device_t;
typedef struct { int (*open)(const hw_module_t *, const char *, hw_device_t **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t mv, hv; const char *id, *name, *author;
    hw_module_methods_t *methods; void *dso; uint64_t reserved[32 - 7];
};
typedef struct { uint32_t gid, fid; } fid_t;
typedef struct __attribute__((packed)) {
    uint8_t version; uint64_t challenge, user_id, authenticator_id;
    uint32_t authenticator_type; uint64_t timestamp; uint8_t hmac[32];
} hat_t;
typedef struct {
    int type;
    union {
        int error;
        struct { fid_t finger; uint32_t samples_remaining; uint64_t msg; } enroll;
        struct { fid_t finger; } removed;
        struct { int info; } acquired;
        struct { fid_t finger; hat_t hat; } auth;
    } data;
} msg_t;
typedef void (*notify_t)(const msg_t *);
typedef struct {
    hw_device_t common;
    notify_t notify;
    int (*set_notify)(void *, notify_t);
    uint64_t (*pre_enroll)(void *);
    int (*enroll)(void *, const hat_t *, uint32_t gid, uint32_t timeout);
    int (*post_enroll)(void *);
    uint64_t (*get_authenticator_id)(void *);
    int (*cancel)(void *);
    int (*enumerate)(void *, fid_t *, uint32_t *);
    int (*remove)(void *, uint32_t gid, uint32_t fid);
    int (*set_active_group)(void *, uint32_t gid, const char *path);
    int (*authenticate)(void *, uint64_t op, uint32_t gid);
} fp_dev_t;

static void notify(const msg_t *m)
{
    switch (m->type) {
    case -1: printf("ERROR %d\n", m->data.error); break;
    case 1: printf("ACQUIRED %d\n", m->data.acquired.info); break;
    case 3: printf("ENROLLING fid %u remaining %u\n", m->data.enroll.finger.fid,
                   m->data.enroll.samples_remaining); break;
    case 4: printf("REMOVED fid %u\n", m->data.removed.finger.fid); break;
    case 5: printf("AUTHENTICATED fid %u (hat version %u)\n", m->data.auth.finger.fid,
                   m->data.auth.hat.version); break;
    default: printf("msg type %d\n", m->type);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "auth";
    int secs = argc > 2 ? atoi(argv[2]) : 20;
    void *l = dlopen("/system/lib64/hw/fingerprint.qcom.so", RTLD_NOW);
    if (!l) { printf("dlopen: %s\n", dlerror()); return 1; }
    hw_module_t *m = dlsym(l, "HMI");
    hw_device_t *hd = NULL;
    int r = m->methods->open(m, "fingerprint", &hd);
    printf("open: %d version 0x%x\n", r, hd ? hd->version : 0);
    if (r) return 1;
    fp_dev_t *d = (fp_dev_t *)hd;
    printf("set_notify: %d\n", d->set_notify(d, notify));
    mkdir("/data/system/users/0/fpdata", 0700);
    printf("set_active_group: %d\n", d->set_active_group(d, 0, "/data/system/users/0/fpdata"));
    printf("authenticator id: 0x%llx\n", (unsigned long long)d->get_authenticator_id(d));
    if (!strcmp(cmd, "enum")) {
        fid_t ids[32]; uint32_t n = 32;
        r = d->enumerate(d, ids, &n);
        printf("enumerate: %d\n", r);
        for (uint32_t i = 0; i < n; i++) printf("  fid %u\n", ids[i].fid);
        return 0;
    }
    if (!strcmp(cmd, "remove")) {
        printf("remove all: %d\n", d->remove(d, 0, 0));
        sleep(2);
        return 0;
    }
    if (!strcmp(cmd, "enroll")) {
        uint64_t ch = d->pre_enroll(d);
        printf("pre_enroll challenge 0x%llx\n", (unsigned long long)ch);
        hat_t hat; memset(&hat, 0, sizeof(hat));
        hat.challenge = ch;
        r = d->enroll(d, &hat, 0, secs);
        printf("enroll (unsigned token): %d\n", r);
    } else {
        r = d->authenticate(d, 0, 0);
        printf("authenticate: %d\n", r);
    }
    fflush(stdout);
    sleep(secs);
    d->cancel(d);
    return 0;
}
