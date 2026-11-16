LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := audio.primary.msmnile
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SRC_FILES := audio_hw.c
LOCAL_C_INCLUDES := external/tinyalsa/include system/media/audio_route/include \
                    $(call include-path-for, audio-utils)
LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa libaudioroute libhardware
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
