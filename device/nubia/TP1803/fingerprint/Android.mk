LOCAL_PATH := $(call my-dir)

# HAL 2.0 shim around the stock Sunwave HAL 2.1 (libsunwave_fp.so).
include $(CLEAR_VARS)
LOCAL_MODULE := fingerprint.qcom
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MULTILIB := 64
LOCAL_SRC_FILES := fingerprint_shim.c
LOCAL_SHARED_LIBRARIES := liblog libdl
LOCAL_CFLAGS := -Wall -Werror
LOCAL_MODULE_TAGS := optional
# The stock HAL loads its trustlet from /vendor/firmware_mnt/image.
LOCAL_POST_INSTALL_CMD := mkdir -p $(TARGET_OUT_VENDOR) && \
    ln -sfn /firmware $(TARGET_OUT_VENDOR)/firmware_mnt
include $(BUILD_SHARED_LIBRARY)
