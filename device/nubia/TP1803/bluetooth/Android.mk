LOCAL_PATH := $(call my-dir)

# libbt-vendor for the WCN399x (Cherokee) Bluetooth SoC: see qca_wcn399x.c
include $(CLEAR_VARS)
LOCAL_MODULE := libbt-vendor
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_SRC_FILES := bt_vendor_tp1803.c qca_wcn399x.c ibs_proxy.c
LOCAL_C_INCLUDES := $(LOCAL_PATH) system/bt/hci/include
LOCAL_CFLAGS := -std=gnu99 -Wall -Werror -Wno-unused-parameter -DANDROID_LOG
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
