LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := sensors.msmnile
LOCAL_MODULE_RELATIVE_PATH := hw
# Only the 64-bit system_server loads it; uses the arm64 generic timer.
LOCAL_MULTILIB := 64
LOCAL_SRC_FILES := sensors.c ssc.c
LOCAL_CFLAGS := -std=gnu99 -Wall -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := liblog libcutils libhardware
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
