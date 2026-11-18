LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := potatogfx
LOCAL_SRC_FILES := main.cpp
LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -fvisibility=hidden -Oz -ffunction-sections -fdata-sections
LOCAL_CPPFLAGS := -std=c++20
LOCAL_LDFLAGS := -Wl,--gc-sections -Wl,--strip-all -Wl,-z,max-page-size=16384
LOCAL_LDLIBS := -llog -lEGL -lGLESv2 -lGLESv3 -ldl
include $(BUILD_SHARED_LIBRARY)
