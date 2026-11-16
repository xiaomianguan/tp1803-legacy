# TP1803 gralloc: ashmem buffers + KMS scanout (software rendering bring-up)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_SRC_FILES := gralloc.cpp framebuffer.cpp mapper.cpp
LOCAL_MODULE := gralloc.msmnile
LOCAL_CFLAGS := -DLOG_TAG=\"gralloc.tp1803\" -Wno-missing-field-initializers
include $(BUILD_SHARED_LIBRARY)
