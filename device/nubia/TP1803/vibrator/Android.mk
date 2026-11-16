LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := vibrator.msmnile
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SRC_FILES := vibrator.c
LOCAL_SHARED_LIBRARIES := liblog libhardware
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
