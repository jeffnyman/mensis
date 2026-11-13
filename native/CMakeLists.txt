# SPDX-License-Identifier: GPL-3.0-or-later
#
# The native loader: Bloodborne's executable run directly on the CPU (docs/the-loader.md).
# x86-64 only, on Windows or Linux.
#
# Windows (Visual Studio 2022 Build Tools with "C++ Clang tools for Windows", and the Vulkan
# SDK, which the window draws through):
#
#     pwsh native/build.ps1
#
# Linux (or WSL2), with clang or g++, CMake and Ninja:
#
#     cmake -S native -B work/native/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build work/native/linux

cmake_minimum_required(VERSION 3.20)
project(mensis_loader LANGUAGES CXX ASM)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(FATAL_ERROR "the native loader runs the game's x86-64 code directly: it needs an x86-64 machine")
endif()
if(WIN32)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "build with clang (native/build.ps1 does): the assembly is GNU syntax")
    endif()
    set(PLATFORM_SOURCES src/platform_windows.cpp src/enter_windows.S src/vulkan.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PLATFORM_SOURCES src/platform_linux.cpp src/enter_linux.S)
else()
    message(FATAL_ERROR "the native loader runs on Windows or Linux")
endif()

# compile_commands.json, for editors: the exact flags and headers each file is built with
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mensis-loader src/main.cpp src/standins.cpp src/threads.cpp src/files.cpp src/gnm.cpp src/libc.cpp src/audio.cpp src/gpu.cpp src/gcn.cpp src/shade.cpp src/bc7.cpp src/draw.cpp src/video.cpp ${PLATFORM_SOURCES})
target_compile_options(mensis-loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)

if(WIN32)
    target_compile_definitions(mensis-loader PRIVATE _CRT_SECURE_NO_WARNINGS)
    find_package(Vulkan REQUIRED)  # from the Vulkan SDK (its installer sets VULKAN_SDK)
    target_link_libraries(mensis-loader PRIVATE user32 gdi32 Vulkan::Vulkan)
    # Vulkan's structures are filled field by field after their type
    set_source_files_properties(src/vulkan.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mensis-loader PRIVATE Threads::Threads)
    # Position-independent, so nothing of ours lands at 0x400000 where the game must go.
    set_target_properties(mensis-loader PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_options(mensis-loader PRIVATE -pie)
endif()
