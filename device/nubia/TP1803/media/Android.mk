#
# Hardware video codecs: Qualcomm's open-source OMX components (CAF
# hardware/qcom/media, LineageOS lineage-17.1-caf-sm8150 @ 4313552) driving the
# kernel's msm_vidc V4L2 driver (/dev/video32 decoder, /dev/video33 encoder),
# built for both ABIs (mediaserver is 64-bit on this port, see device.mk).
#
# The stock vendor image only ships Codec2 (HIDL) codecs, which M can't host.
#
# compat/ supplies what the Android 10 code expects but M lacks:
#   gralloc_priv.h + gr_priv_handle.h  QTI names over OUR gralloc's handle
#   ion/ion.h + ion_compat.c           libion calls for the 4.14 ION ABI
#   qdMetaData.h / color_metadata.h    API of our stub libqdMetaData
#   c2d/C2DColorConverter.h            no-op converter (no Adreno C2D blob)
#   display_config.h                   qdutils display attributes
#   media/hardware/*, nativebase/      Android 10 OMX extension headers
# kernel-headers/ holds the running kernel's UAPI (msm_vidc, ION, V4L2).
#
LOCAL_PATH := $(call my-dir)

TP_MEDIA := $(LOCAL_PATH)

tp-media-includes := \
    $(TP_MEDIA)/compat \
    $(TP_MEDIA)/compat/c2d \
    $(TP_MEDIA)/kernel-headers \
    $(TP_MEDIA)/mm-core/inc \
    $(TP_MEDIA)/mm-video-v4l2/vidc/common/inc \
    $(TP_MEDIA)/libplatformconfig \
    $(TP_MEDIA)/libarbitrarybytes/inc \
    $(TP_MEDIA)/libstagefrighthw

tp-media-cflags := \
    -include $(TP_MEDIA)/compat/tp_compat.h \
    -D_ANDROID_ -D_ANDROID_ICS_ -DUSE_ION -DTARGET_ION_ABI_VERSION=2 \
    -D__alignx\(x\)=__attribute__\(\(__aligned__\(x\)\)\) -D__align=__alignx \
    -Dinline=__inline -DT_ARM \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-missing-field-initializers -Wno-parentheses -Wno-sign-compare \
    -Wno-unused-private-field -Wno-format -Wno-sometimes-uninitialized \
    -Wno-gnu-designator -Wno-reorder -Wno-unused-label

# ---- libOmxCore: OMX IL core / component registry ----
include $(CLEAR_VARS)
LOCAL_MODULE := libOmxCore
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_C_INCLUDES := $(TP_MEDIA)/mm-core/src/common $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags) -D_NILE_ -fno-short-enums -U_ENABLE_QC_MSG_LOG_
LOCAL_SRC_FILES := \
    mm-core/src/common/omx_core_cmp.cpp \
    mm-core/src/common/qc_omx_core.c \
    mm-core/src/registry_table_android.c
LOCAL_SHARED_LIBRARIES := liblog libdl libcutils libplatformconfig
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)

# ---- libstagefrighthw: the OMX plugin mediaserver's OMXMaster dlopens ----
include $(CLEAR_VARS)
LOCAL_MODULE := libstagefrighthw
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
# M's own OMXPluginBase.h (frameworks/native/include) defines the ABI here.
LOCAL_C_INCLUDES := $(TP_MEDIA)/mm-core/inc frameworks/native/include/media/openmax
LOCAL_CFLAGS := -Wno-unused-parameter
LOCAL_SRC_FILES := libstagefrighthw/QComOMXPlugin.cpp
LOCAL_SHARED_LIBRARIES := libutils libcutils libdl liblog
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)

# ---- ION shim (static) ----
include $(CLEAR_VARS)
LOCAL_MODULE := libion_tp1803
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_C_INCLUDES := $(tp-media-includes)
LOCAL_SRC_FILES := compat/ion_compat.c
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_STATIC_LIBRARY)

# ---- libOmxVidcCommon (static) ----
include $(CLEAR_VARS)
LOCAL_MODULE := libOmxVidcCommon
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_C_INCLUDES := $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags)
LOCAL_SRC_FILES := \
    mm-video-v4l2/vidc/common/src/vidc_common.cpp \
    mm-video-v4l2/vidc/common/src/vidc_vendor_extensions.cpp
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_STATIC_LIBRARY)

# ---- libplatformconfig: XML config only (no HIDL configstore on M) ----
include $(CLEAR_VARS)
LOCAL_MODULE := libplatformconfig
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_C_INCLUDES := external/expat/lib $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags) -fexceptions -Wno-deprecated-declarations
LOCAL_SRC_FILES := libplatformconfig/PlatformConfig.cpp libplatformconfig/ConfigParser.cpp libplatformconfig/ConfigStore.cpp
LOCAL_SHARED_LIBRARIES := libexpat liblog libcutils libutils
LOCAL_STATIC_LIBRARIES := libOmxVidcCommon
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)

# ---- libarbitrarybytes: bitstream frame parser used by the decoder ----
include $(CLEAR_VARS)
LOCAL_MODULE := libarbitrarybytes
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_C_INCLUDES := $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags)
LOCAL_SRC_FILES := \
    libarbitrarybytes/src/frameparser.cpp \
    libarbitrarybytes/src/h264_utils.cpp \
    libarbitrarybytes/src/hevc_utils.cpp
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)

# ---- libOmxVdec: msm_vidc decoder ----
include $(CLEAR_VARS)
LOCAL_MODULE := libOmxVdec
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_C_INCLUDES := $(TP_MEDIA)/mm-video-v4l2/vidc/vdec/inc $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags) \
    -DIMAGE_APPS_PROC -DCDECL -DNO_ARM_CLZ -DENABLE_DEBUG_ERROR \
    -DPROCESS_EXTRADATA_IN_OUTPUT_PORT -DADAPTIVE_PLAYBACK_SUPPORTED \
    -DFLEXYUV_SUPPORTED -DALLOCATE_OUTPUT_NATIVEHANDLE
LOCAL_SRC_FILES := \
    mm-video-v4l2/vidc/vdec/src/ts_parser.cpp \
    mm-video-v4l2/vidc/vdec/src/omx_vdec_v4l2.cpp
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libdl libqdMetaData libplatformconfig libarbitrarybytes
LOCAL_STATIC_LIBRARIES := libOmxVidcCommon libion_tp1803
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)

# ---- libOmxVenc: msm_vidc encoder ----
include $(CLEAR_VARS)
LOCAL_MODULE := libOmxVenc
LOCAL_MODULE_TAGS := optional
LOCAL_MULTILIB := both
LOCAL_MODULE_PATH_32 := $(TARGET_OUT_VENDOR)/lib
LOCAL_MODULE_PATH_64 := $(TARGET_OUT_VENDOR)/lib64
LOCAL_C_INCLUDES := $(TP_MEDIA)/mm-video-v4l2/vidc/venc/inc $(tp-media-includes)
LOCAL_CFLAGS := $(tp-media-cflags) \
    -DENABLE_DEBUG_ERROR -USINGLE_ENCODER_INSTANCE -DUSE_NATIVE_HANDLE_SOURCE \
    -DUSE_CAMERA_METABUFFER_UTILS
LOCAL_SRC_FILES := \
    mm-video-v4l2/vidc/venc/src/omx_video_base.cpp \
    mm-video-v4l2/vidc/venc/src/omx_video_encoder.cpp \
    mm-video-v4l2/vidc/venc/src/video_encoder_device_v4l2.cpp
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libdl libqdMetaData libplatformconfig libui
LOCAL_STATIC_LIBRARIES := libOmxVidcCommon libion_tp1803
LOCAL_CLANG := true
LOCAL_CONLYFLAGS += -std=gnu99
LOCAL_CPPFLAGS += -std=c++14
include $(BUILD_SHARED_LIBRARY)
