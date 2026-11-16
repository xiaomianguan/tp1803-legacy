/*
 * private_handle_t for the OMX components, in the QTI field names they are
 * written against but with the memory layout of OUR gralloc
 * (device/nubia/TP1803/gralloc/gralloc_priv.h). Keep the two in sync.
 *
 *   ours      QTI name here       meaning
 *   width     unaligned_width     width the client asked for
 *   height    unaligned_height    height the client asked for
 *   stride    width               aligned width (pixels)
 *   vstride   height              aligned height (luma rows)
 *
 * Our gralloc has no metadata fd, buffer_type, usage or id fields; the OMX
 * code only reads fd/size/offset/flags/format and the dimensions.
 * Flag values are QTI's: our gralloc sets only 0x2 (ION) and 0x4 (cached),
 * which none of the QTI flags tested by the OMX code overlap.
 */
#ifndef __GR_PRIV_HANDLE_H__
#define __GR_PRIV_HANDLE_H__

#include <stdint.h>
#include <cutils/native_handle.h>
#include <hardware/gralloc.h>

#ifdef __cplusplus
struct private_handle_t : public native_handle_t {
#else
struct private_handle_t {
    native_handle_t nativeHandle;
#endif
  enum {
    PRIV_FLAGS_FRAMEBUFFER = 0x00000001,
    PRIV_FLAGS_USES_ION = 0x00000002,       /* ours */
    PRIV_FLAGS_CACHED = 0x00000004,         /* ours */
    PRIV_FLAGS_NEEDS_FLUSH = 0x00000020,
    PRIV_FLAGS_INTERNAL_ONLY = 0x00000040,
    PRIV_FLAGS_NON_CPU_WRITER = 0x00000080,
    PRIV_FLAGS_SECURE_BUFFER = 0x00000400,
    PRIV_FLAGS_EXTERNAL_ONLY = 0x00002000,
    PRIV_FLAGS_PROTECTED_BUFFER = 0x00004000,
    PRIV_FLAGS_VIDEO_ENCODER = 0x00010000,
    PRIV_FLAGS_CAMERA_WRITE = 0x00020000,
    PRIV_FLAGS_CAMERA_READ = 0x00040000,
    PRIV_FLAGS_HW_COMPOSER = 0x00080000,
    PRIV_FLAGS_HW_TEXTURE = 0x00100000,
    PRIV_FLAGS_ITU_R_601 = 0x00200000,
    PRIV_FLAGS_ITU_R_601_FR = 0x00400000,
    PRIV_FLAGS_ITU_R_709 = 0x00800000,
    PRIV_FLAGS_SECURE_DISPLAY = 0x01000000,
    PRIV_FLAGS_TILE_RENDERED = 0x02000000,
    PRIV_FLAGS_CPU_RENDERED = 0x04000000,
    PRIV_FLAGS_UBWC_ALIGNED = 0x08000000,
    PRIV_FLAGS_DISP_CONSUMER = 0x10000000,
    PRIV_FLAGS_CLIENT_ALLOCATED = 0x20000000,
    PRIV_FLAGS_UBWC_ALIGNED_PI = 0x40000000,
  };

  int fd;
  int magic;
  int flags;
  int size;
  int offset;
  int format;
  int unaligned_width;
  int unaligned_height;
  int width;
  int height;
  uint64_t base __attribute__((aligned(8)));
  int pid;

#ifdef __cplusplus
  static const int kNumFds = 1;
  static const int kMagic = 0x3141593;

  static inline int NumInts() {
    return ((sizeof(private_handle_t) - sizeof(native_handle_t)) / sizeof(int)) - kNumFds;
  }

  static int validate(const native_handle *h) {
    const private_handle_t *hnd = (const private_handle_t *)h;
    if (!h || h->version != sizeof(native_handle) || h->numInts != NumInts() ||
        h->numFds != kNumFds || hnd->magic != kMagic)
      return -1;
    return 0;
  }

  static private_handle_t *dynamicCast(const native_handle *in) {
    return validate(in) == 0 ? (private_handle_t *)in : NULL;
  }
#endif
};

#endif /* __GR_PRIV_HANDLE_H__ */
