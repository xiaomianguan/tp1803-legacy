/* qdutils display queries used by the decoder to pick a downscaler target.
 * Only the built-in 1080x2160 panel exists on this port. */
#pragma once
#include <stdint.h>

namespace qdutils {
enum { DISPLAY_PRIMARY = 0, DISPLAY_EXTERNAL, DISPLAY_VIRTUAL };

struct DisplayAttributes {
    uint32_t vsync_period;
    uint32_t xres;
    uint32_t yres;
    float xdpi;
    float ydpi;
    int panel_type;
    bool is_yuv;
};

static inline int getActiveConfig(int dpy) { return dpy == DISPLAY_PRIMARY ? 0 : -1; }

static inline DisplayAttributes getDisplayAttributes(int config, int dpy) {
    DisplayAttributes a = {};
    if (dpy == DISPLAY_PRIMARY && config == 0) {
        a.vsync_period = 16666666;
        a.xres = 1080;
        a.yres = 2160;
        a.xdpi = a.ydpi = 420.0f;
    }
    return a;
}
}  // namespace qdutils
