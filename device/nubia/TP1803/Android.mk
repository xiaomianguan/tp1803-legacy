LOCAL_PATH := $(call my-dir)
ifeq ($(TARGET_DEVICE),TP1803)

# Prebuilt kernel: CM expects kernel UAPI headers here; bionic's are used instead.
$(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr:
	mkdir -p $@

include $(call all-makefiles-under,$(LOCAL_PATH))

endif
