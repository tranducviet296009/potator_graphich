cmake_minimum_required(VERSION 3.10)
project(potatogfx CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

target_link_libraries(potatogfx
    log
    EGL
    GLESv2
    GLESv3
    dl
)
