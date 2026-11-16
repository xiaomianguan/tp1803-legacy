/* Shims for libc functions Mesa uses that bionic only gained after API 23
 * (Android 6.0). Force-included into every Mesa translation unit. */
#pragma once
#if defined(__ANDROID__) && __ANDROID_API__ < 24
#include <string.h>
static inline char *mesa_compat_strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c)
        s++;
    return (char *)s;
}
#define strchrnul mesa_compat_strchrnul
#endif

/* SysV shm headers exist in the NDK but the functions are API 26+ (and
 * always fail on Android anyway). */
#if defined(__ANDROID__) && __ANDROID_API__ < 26
#undef HAVE_SYS_SHM_H
#endif
