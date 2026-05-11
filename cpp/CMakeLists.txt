cmake_minimum_required(VERSION 3.22)
project("runbeat")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 核心引擎库 — 纯 C++17 头文件库（现阶段仅占位）
add_library(runbeat_core INTERFACE)
target_include_directories(runbeat_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# AAudio 驱动 + JNI 桥接
add_library(runbeat SHARED aaudio_engine.cpp)
target_link_libraries(runbeat
    PRIVATE
    runbeat_core
    aaudio
    android
    log
)
target_include_directories(runbeat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
