LOCAL_PATH := $(call my-dir)

# RIL for the SDM855 modem: QMI over QRTR (see ril.c)
include $(CLEAR_VARS)
LOCAL_MODULE := libril-tp1803
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_SRC_FILES := ril.c ril_sim.c ril_voice.c ril_data.c ril_sms.c data_path.c qmi.c
LOCAL_C_INCLUDES := $(LOCAL_PATH) hardware/ril/include
LOCAL_CFLAGS := -std=gnu99 -Wall -Werror -Wno-unused-parameter -DRIL_SHLIB
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
