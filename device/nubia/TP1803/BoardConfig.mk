#
# CyanogenMod 13 (Android 6.0.1) board config for Nubia Mini 5G (TP1803, SM8150/msmnile)
#
# The kernel is Lineage's prebuilt 4.14 Image.gz + dtb. The boot image is v2,
# which CM13's mkbootimg can't make, so it is repacked by
# scripts/mkboot.sh with a modern host mkbootimg.
#

DEVICE_PATH := device/nubia/TP1803

# Architecture
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_ABI2 :=
TARGET_CPU_VARIANT := generic

TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv7-a-neon
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := cortex-a53

TARGET_USES_64_BIT_BINDER := true

# Platform. msmnile isn't known to any CM13 qcom HAL, so none of them get built.
TARGET_BOARD_PLATFORM := msmnile
TARGET_BOOTLOADER_BOARD_NAME := msmnile
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Kernel (prebuilt from LineageOS TP1803)
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/Image.gz
# AOSPA's build/core/tasks/kernel.mk installs $(TARGET_PREBUILT_INT_KERNEL),
# which it only sets when building the kernel from source.
TARGET_PREBUILT_INT_KERNEL := $(TARGET_PREBUILT_KERNEL)
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_KERNEL_CMDLINE := \
    androidboot.console=ttyMSM0 \
    androidboot.hardware=qcom \
    androidboot.usbcontroller=a600000.dwc3 \
    console=ttyMSM0,115200n8 \
    earlycon=msm_geni_serial,0xa90000 \
    loop.max_part=7 \
    lpm_levels.sleep_disabled=1 \
    service_locator.enable=1 \
    androidboot.selinux=disabled download_mode=0

# Partitions
BOARD_FLASH_BLOCK_SIZE := 131072
BOARD_BOOTIMAGE_PARTITION_SIZE := 100663296
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 67108864
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3221225472
BOARD_USERDATAIMAGE_PARTITION_SIZE := 55588106240
BOARD_CACHEIMAGE_PARTITION_SIZE := 268435456
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_USERIMAGES_USE_EXT4 := true

# Graphics: software GL (libagl) + KMS gralloc (hardware/tp1803/gralloc)
USE_OPENGL_RENDERER := true
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK := true

# Audio (stub HAL for bring-up)
BOARD_USES_GENERIC_AUDIO := true
USE_XML_AUDIO_POLICY_CONF := 0

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/rootdir/fstab.qcom
TARGET_RECOVERY_PIXEL_FORMAT := "RGBX_8888"

# SELinux (permissive via cmdline during bring-up)
BOARD_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy

# Misc
TARGET_SYSTEM_PROP := $(DEVICE_PATH)/system.prop
MALLOC_SVELTE := false

# Wi-Fi: WCN3990-class chip behind icnss, built-in qca_cld (CLD3) driver
BOARD_HAS_QCOM_WLAN := true
BOARD_WLAN_DEVICE := qcwcn
BOARD_HOSTAPD_DRIVER := NL80211
BOARD_HOSTAPD_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
WPA_SUPPLICANT_VERSION := VER_0_8_X
WIFI_DRIVER_FW_PATH_STA := "sta"
WIFI_DRIVER_FW_PATH_AP := "ap"
WIFI_DRIVER_FW_PATH_P2P := "p2p"
# The driver is built in; writing ON/OFF to /dev/wlan loads/unloads it
WIFI_DRIVER_STATE_CTRL_PARAM := "/dev/wlan"
WIFI_DRIVER_STATE_ON := "ON"
WIFI_DRIVER_STATE_OFF := "OFF"

# Bluetooth: WCN399x over UART, own libbt-vendor (bluetooth/)
BOARD_HAVE_BLUETOOTH := true
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := device/nubia/TP1803/bluetooth
