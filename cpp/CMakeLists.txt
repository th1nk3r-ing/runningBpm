cmake_minimum_required(VERSION 3.22)
project("runbeat")

option(RUNBEAT_TSAN "Build C++ tests with ThreadSanitizer" OFF)
if(RUNBEAT_TSAN AND ANDROID)
    message(FATAL_ERROR "RUNBEAT_TSAN is for host-side C++ tests, not Android builds")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND ANDROID)
    # 自动获取当前编译器(NDK clang)的 resource-dir
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
        OUTPUT_VARIABLE NDK_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # 强制将该路径注入到所有文件的编译选项中
    add_compile_options("-resource-dir=${NDK_RESOURCE_DIR}")
endif()

# 核心引擎库 — 纯 C++17 头文件库
add_library(runbeat_core INTERFACE)
target_include_directories(runbeat_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
)
target_compile_features(runbeat_core INTERFACE cxx_std_17)

if(ANDROID)
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
else()
    include(CTest)
    if(BUILD_TESTING)
        find_package(Threads REQUIRED)
        if(RUNBEAT_TSAN)
            if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
                message(FATAL_ERROR "RUNBEAT_TSAN requires Clang or GCC")
            endif()
            add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=thread)
        endif()
        set(RUNBEAT_TESTS
            clock_test
            clock_preserve_phase_test
            sample_player_test
            mixer_test
            limiter_test
            engine_test
            wav_loader_test
            log_test
        )
        foreach(test_name IN LISTS RUNBEAT_TESTS)
            add_executable(${test_name} test/${test_name}.cpp)
            target_link_libraries(${test_name} PRIVATE runbeat_core Threads::Threads)
            add_test(NAME ${test_name} COMMAND ${test_name})
        endforeach()
    endif()
endif()
