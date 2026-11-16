/* Phase-1 check: load the CamX HAL module directly and enumerate cameras. */
#include <dlfcn.h>
#include <stdio.h>
#include <hardware/camera_common.h>
#include <system/camera_metadata.h>

static void dev_status(const struct camera_module_callbacks *cb, int id, int st)
{ (void)cb; printf("callback: camera %d status %d\n", id, st); }
static void torch_status(const struct camera_module_callbacks *cb, const char *id, int st)
{ (void)cb; printf("callback: torch %s status %d\n", id, st); }
static camera_module_callbacks_t cbs = { dev_status, torch_status };

static size_t (*entry_count)(const camera_metadata_t *);

int main(void)
{
    entry_count = dlsym(dlopen("libcamera_metadata.so", RTLD_NOW), "get_camera_metadata_entry_count");
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/system/vendor/lib64/hw/camera.qcom.so", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    camera_module_t *m = dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
    printf("module '%s' api 0x%x\n", m->common.name, m->common.module_api_version);
    if (m->init) printf("init: %d\n", m->init());
    if (m->set_callbacks) printf("set_callbacks: %d\n", m->set_callbacks(&cbs));
    int n = m->get_number_of_cameras();
    printf("cameras: %d\n", n);
    for (int i = 0; i < n; i++) {
        struct camera_info info;
        int r = m->get_camera_info(i, &info);
        printf("camera %d: ret %d facing %d orientation %d device_version 0x%x static entries %zu\n",
               i, r, info.facing, info.orientation, info.device_version,
               info.static_camera_characteristics ?
                   entry_count(info.static_camera_characteristics) : 0);
    }
    return 0;
}
