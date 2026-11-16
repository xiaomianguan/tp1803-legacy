# Must come before anything that pulls in core_minimal (ro.zygote=zygote32)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

# Bring-up: open adb with root. cm.mk is evaluated before the makefiles it
# inherits and the first value of a default prop wins, so this beats
# vendor/cm/config/common.mk forcing ro.adb.secure=1.
ADDITIONAL_DEFAULT_PROPERTIES += \
    ro.adb.secure=0 \
    ro.secure=0 \
    persist.sys.usb.config=mtp \
    ro.zygote=zygote64_32

# Full CM phone configuration (includes telephony.mk: Messaging, Stk, ...)
$(call inherit-product, vendor/cm/config/common_full_phone.mk)

$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)
$(call inherit-product, device/nubia/TP1803/device.mk)

PRODUCT_NAME := cm_TP1803
PRODUCT_DEVICE := TP1803
PRODUCT_BRAND := nubia
PRODUCT_MODEL := Nubia Mini 5G
PRODUCT_MANUFACTURER := nubia

PRODUCT_GMS_CLIENTID_BASE := android-zte

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRODUCT_NAME=TP1803 \
    BUILD_FINGERPRINT=nubia/cm_TP1803/TP1803:6.0.1/MOB31K/cm13:userdebug/test-keys \
    PRIVATE_BUILD_DESC="cm_TP1803-userdebug 6.0.1 MOB31K cm13 test-keys"
