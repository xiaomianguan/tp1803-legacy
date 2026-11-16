#
# CyanogenMod 13 device makefile for Nubia Mini 5G (TP1803)
#

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
# Dalvik heap for 6-8 GB phones (frameworks/native's phone-xxxhdpi-4096
# config, which AOSPA's frameworks/native doesn't ship)
PRODUCT_PROPERTY_OVERRIDES += \
    dalvik.vm.heapstartsize=8m \
    dalvik.vm.heapgrowthlimit=384m \
    dalvik.vm.heapsize=1024m \
    dalvik.vm.heaptargetutilization=0.75 \
    dalvik.vm.heapminfree=4m \
    dalvik.vm.heapmaxfree=16m

LOCAL_PATH := device/nubia/TP1803

DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay

PRODUCT_CHARACTERISTICS := nosdcard
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xxhdpi

# Screen
TARGET_SCREEN_HEIGHT := 2160
TARGET_SCREEN_WIDTH := 1080

# Ramdisk
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/fstab.qcom:root/fstab.qcom \
    $(LOCAL_PATH)/rootdir/init.qcom.rc:root/init.qcom.rc \
    $(LOCAL_PATH)/rootdir/init.qcom.usb.rc:root/init.qcom.usb.rc \
    $(LOCAL_PATH)/rootdir/ueventd.qcom.rc:root/ueventd.qcom.rc

# Input
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/himax-touchscreen.idc:system/usr/idc/himax-touchscreen.idc \
    $(LOCAL_PATH)/configs/gpio-keys.kl:system/usr/keylayout/gpio-keys.kl \
    $(LOCAL_PATH)/configs/qpnp_pon.kl:system/usr/keylayout/qpnp_pon.kl

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:system/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:system/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:system/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepcounter.xml:system/etc/permissions/android.hardware.sensor.stepcounter.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepdetector.xml:system/etc/permissions/android.hardware.sensor.stepdetector.xml

# Graphics: KMS/ION gralloc + Mesa freedreno (Gallium, KGSL backend) on the
# Adreno 640. Mesa is built out of tree with the NDK: tools/mesa/build-mesa.sh,
# then tools/mesa/install-prebuilts.sh.
PRODUCT_PACKAGES += \
    gralloc.msmnile

MESA_PREBUILT := $(LOCAL_PATH)/prebuilt/mesa
PRODUCT_COPY_FILES += \
    $(foreach l,lib lib64,\
        $(MESA_PREBUILT)/$(l)/egl/libEGL_mesa.so:system/$(l)/egl/libEGL_mesa.so \
        $(MESA_PREBUILT)/$(l)/egl/libGLESv1_CM_mesa.so:system/$(l)/egl/libGLESv1_CM_mesa.so \
        $(MESA_PREBUILT)/$(l)/egl/libGLESv2_mesa.so:system/$(l)/egl/libGLESv2_mesa.so \
        $(MESA_PREBUILT)/$(l)/libglapi.so:system/$(l)/libglapi.so \
        $(MESA_PREBUILT)/$(l)/dri/kgsl_dri.so:system/$(l)/dri/kgsl_dri.so)

# Adreno 640 firmware (SQE microcode, GMU, zap shader) from the stock vendor
PRODUCT_COPY_FILES += \
    $(foreach f,a630_sqe.fw a640_gmu.bin a640_zap.b00 a640_zap.b01 a640_zap.b02 a640_zap.elf a640_zap.mdt ipa_fws.mdt ipa_fws.elf ipa_fws.b00 ipa_fws.b01 ipa_fws.b02 ipa_fws.b03 ipa_fws.b04,\
        $(LOCAL_PATH)/proprietary/firmware/$(f):system/vendor/firmware/$(f))

# Lights (backlight)
PRODUCT_PACKAGES += \
    lights.msmnile

# Vibrator (qti-haptics force-feedback input device)
PRODUCT_PACKAGES += \
    vibrator.msmnile

# Sensors: SEE (SLPI) accel/gyro/mag over QRTR + Nubia tmd2725 light/prox.
# hexagonrpcd (tools/hexagonrpc/build.sh) serves the SLPI its registry/config.
PRODUCT_PACKAGES += \
    sensors.msmnile

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/prebuilt/hexagonrpc/hexagonrpcd:system/bin/hexagonrpcd \
    $(LOCAL_PATH)/proprietary/sensors/sns_reg_config:system/etc/sensors/sns_reg_config \
    $(foreach f,$(notdir $(wildcard $(LOCAL_PATH)/proprietary/sensors/config/*.json)),\
        $(LOCAL_PATH)/proprietary/sensors/config/$(f):system/etc/sensors/config/$(f))

# Camera: stock Qualcomm CamX HAL (tools/camera/extract-blobs.sh), running on
# CM13 with bionic/linker additions and two stub libraries. CamX is 64-bit
# only, so the camera service runs in its own 64-bit cameraserver.
CAMX_BLOBS := $(LOCAL_PATH)/proprietary/camera
PRODUCT_COPY_FILES += \
    $(foreach f,$(shell cd $(CAMX_BLOBS) && find . -type f -printf '%P\n'),\
        $(CAMX_BLOBS)/$(f):system/vendor/$(f))

PRODUCT_PROPERTY_OVERRIDES += \
    ro.hardware.camera=tp1803 \
    camera.disable_zsl_mode=1

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:system/etc/permissions/android.hardware.camera.front.xml

PRODUCT_PACKAGES += \
    camera.tp1803 \
    cameraserver \
    libqdMetaData \
    libcdsprpc \
    libhidlbase \
    libhidltransport \
    vendor.qti.hardware.vpp@1.1 \
    vendor.qti.hardware.vpp@1.2

# Audio: tinyalsa + audio_route HAL on the stock tavil mixer paths
PRODUCT_PACKAGES += \
    audio.primary.msmnile \
    audio.r_submix.default \
    audio_policy.default

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/proprietary/audio/mixer_paths_tavil.xml:system/etc/mixer_paths_tavil.xml

# ACDB calibration (stock 32-bit loader, dlopened by the HAL for voice calls;
# libio2 = Q libion renamed so it doesn't clash with M's libion)
PRODUCT_COPY_FILES += \
    $(foreach f,$(notdir $(wildcard $(LOCAL_PATH)/proprietary/audio/lib/*.so)),\
        $(LOCAL_PATH)/proprietary/audio/lib/$(f):system/vendor/lib/$(f)) \
    $(LOCAL_PATH)/proprietary/audio/acdbdata/adsp_avs_config.acdb:system/vendor/etc/acdbdata/adsp_avs_config.acdb \
    $(foreach f,$(notdir $(wildcard $(LOCAL_PATH)/proprietary/audio/acdbdata/MTP/*)),\
        $(LOCAL_PATH)/proprietary/audio/acdbdata/MTP/$(f):system/vendor/etc/acdbdata/MTP/$(f))

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/audio_policy.conf:system/etc/audio_policy.conf

# Fingerprint: Sunwave sensor via the stock HAL (QSEE trustlet sw_fp from
# /firmware/image) behind our HAL 2.0 shim for fingerprintd; stock qseecomd
# + listener libs serve the trustlet's file access
PRODUCT_PACKAGES += \
    fingerprintd \
    fingerprint.qcom
PRODUCT_COPY_FILES += \
    $(foreach f,$(notdir $(wildcard $(LOCAL_PATH)/proprietary/fingerprint/lib64/*.so)),\
        $(LOCAL_PATH)/proprietary/fingerprint/lib64/$(f):system/vendor/lib64/$(f)) \
    $(LOCAL_PATH)/proprietary/fingerprint/bin/qseecomd:system/vendor/bin/qseecomd \
    $(LOCAL_PATH)/proprietary/fingerprint/etc/sw_config.xml:system/vendor/etc/sw_config.xml \
    $(LOCAL_PATH)/proprietary/fingerprint/etc/sf-keys.kl:system/usr/keylayout/sf-keys.kl \
    $(LOCAL_PATH)/proprietary/fingerprint/etc/sf-keys.idc:system/usr/idc/sf-keys.idc \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:system/etc/permissions/android.hardware.fingerprint.xml

# Media codecs: msm_vidc hardware video codecs (device/.../media, Qualcomm
# OMX on V4L2) plus the Google software codecs
PRODUCT_PACKAGES += \
    libOmxCore \
    libOmxVdec \
    libOmxVenc \
    libstagefrighthw

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/media_codecs.xml:system/etc/media_codecs.xml \
    $(LOCAL_PATH)/configs/media_profiles.xml:system/etc/media_profiles.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:system/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_telephony.xml:system/etc/media_codecs_google_telephony.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:system/etc/media_codecs_google_video.xml

# Bluetooth: WCN399x (firmware from /bt_firmware), A2DP via the software
# audio module
PRODUCT_PACKAGES += \
    libbt-vendor \
    audio.a2dp.default

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:system/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:system/etc/permissions/android.hardware.bluetooth_le.xml

# Telephony: own RIL on the SDM855 modem over QMI/QRTR (ril/)
PRODUCT_PACKAGES += \
    libril-tp1803

PRODUCT_PROPERTY_OVERRIDES += \
    rild.libpath=/system/vendor/lib64/libril-tp1803.so \
    ro.telephony.default_network=9 \
    telephony.lteOnCdmaDevice=0

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml

# Wi-Fi
PRODUCT_PACKAGES += \
    wpa_supplicant \
    wpa_supplicant.conf \
    hostapd \
    libwpa_client \
    dhcpcd.conf

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/wifi/wpa_supplicant_overlay.conf:system/etc/wifi/wpa_supplicant_overlay.conf

PRODUCT_PROPERTY_OVERRIDES += \
    wifi.interface=wlan0 \
    wifi.supplicant_scan_interval=15

# Modem/WLAN support daemons (open source linux-msm tools + our own helpers,
# built with the NDK: tools/qcom-daemons/build.sh)
PRODUCT_COPY_FILES += \
    $(foreach b,qrtr-ns rmtfs pd-mapper tqftpserv modem-holder mdm-boot mdm-ctl wlfw-bdf,\
        $(LOCAL_PATH)/prebuilt/qcom-daemons/$(b):system/bin/$(b)) \
    $(LOCAL_PATH)/proprietary/wlan/WCNSS_qcom_cfg.ini:system/vendor/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini

# Phone apps, including telephony/camera/FM/NFC apps that have no working
# hardware backend yet. AOSPA equivalents of CM's apps: SnapdragonCamera
# (CM: Snap). No FM app: CAF builds FM2 only with BOARD_HAVE_QCOM_FM.
PRODUCT_PACKAGES += \
    Dialer \
    messaging \
    Stk \
    CellBroadcastReceiver \
    CarrierConfig \
    SnapdragonCamera \
    Nfc \
    Tag \
    SmartCardService \
    BluetoothExt

# Bring-up helpers
PRODUCT_PACKAGES += \
    strace \
    tinymix \
    tinyplay \
    tinycap \
    toybox

PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=420 \
    ro.opengles.version=196610 \
    persist.sys.usb.config=mtp,adb \
    ro.adb.secure=0 \
    ro.secure=0 \
    persist.service.adb.enable=1 \
    debug.sf.no_hw_vsync=1
