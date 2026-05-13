cmake_minimum_required(VERSION 3.22)
project("runbeat")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    # 自动获取当前编译器(NDK clang)的 resource-dir
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
        OUTPUT_VARIABLE NDK_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # 强制将该路径注入到所有文件的编译选项中
    add_compile_options("-resource-dir=${NDK_RESOURCE_DIR}")
endif()

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
