# AOSPA (Paranoid Android 6, marshmallow-caf) product for the Nubia Mini 5G.
# Modelled on vendor/pa/products/*/pa_<device>.mk; device/qcom/common is not
# included because its CAF 8996 HALs are replaced by our own (gralloc/Mesa,
# audio, camera, RIL).

# Must come before anything that pulls in core_minimal (ro.zygote=zygote32)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

# Bring-up: open adb with root (first value of a default prop wins).
ADDITIONAL_DEFAULT_PROPERTIES += \
    ro.adb.secure=0 \
    ro.secure=0 \
    persist.sys.usb.config=mtp \
    ro.zygote=zygote64_32

$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base_telephony.mk)

TARGET_BOOT_ANIMATION_RES := 1080

# CAF's okhttp/apache-http implement com.quicinc.tcmiface.DpmTcmIface, which
# device/qcom/common (not included, see above) puts on the boot classpath.
# Without it dex2oat aborts building the boot image and zygote never starts.
PRODUCT_PACKAGES += tcmiface
PRODUCT_BOOT_JARS += tcmiface

$(call inherit-product, device/nubia/TP1803/device.mk)

include vendor/pa/main.mk

PRODUCT_NAME := pa_TP1803
PRODUCT_DEVICE := TP1803
PRODUCT_BRAND := nubia
PRODUCT_MODEL := Nubia Mini 5G
PRODUCT_MANUFACTURER := nubia

PRODUCT_GMS_CLIENTID_BASE := android-zte

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRODUCT_NAME=TP1803 \
    BUILD_FINGERPRINT=nubia/pa_TP1803/TP1803:6.0.1/MMB29M/aospa6:userdebug/test-keys \
    PRIVATE_BUILD_DESC="pa_TP1803-userdebug 6.0.1 MMB29M aospa6 test-keys"
