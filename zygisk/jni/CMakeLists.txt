cmake_minimum_required(VERSION 3.22.1)
project(potatogfx CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_SHADOWHOOK "Sử dụng thư viện ByteDance ShadowHook cho inline hook ARM64" ON)

if(ENABLE_SHADOWHOOK)
    include(FetchContent)
    FetchContent_Declare(
        shadowhook
        GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
        GIT_TAG        v1.0.10
    )
    FetchContent_GetProperties(shadowhook)
    if(NOT shadowhook_POPULATED)
        FetchContent_Populate(shadowhook)
        add_subdirectory(${shadowhook_SOURCE_DIR}/shadowhook ${shadowhook_BINARY_DIR}/shadowhook EXCLUDE_FROM_ALL)
    endif()
endif()

add_library(potatogfx SHARED
    main.cpp
)

target_compile_options(potatogfx PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -Oz
    -ffunction-sections
    -fdata-sections
)

# Android 16 (API 36) yêu cầu các file .so căn lề phân đoạn ELF 16KB (-Wl,-z,max-page-size=16384)
target_link_options(potatogfx PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
    -Wl,-z,max-page-size=16384
)

if(ENABLE_SHADOWHOOK AND TARGET shadowhook)
    target_compile_definitions(potatogfx PRIVATE USE_SHADOWHOOK=1)
    target_include_directories(potatogfx PRIVATE
        ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp/include
    )
    target_link_libraries(potatogfx
        shadowhook
        log
        EGL
        GLESv2
        GLESv3
        dl
    )
else()
    target_link_libraries(potatogfx
        log
        EGL
        GLESv2
        GLESv3
        dl
    )
endif()
