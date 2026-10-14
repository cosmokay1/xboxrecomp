cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_overlay.c
    kernel_thread.c
    xbox_ggl.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})
target_link_libraries(xbox_kernel PUBLIC SDL3::SDL3)

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    # /Zi: private symbols, so profiles and crash reports name static
    # functions instead of the nearest public one.
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy /Zi /FS)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

# PUBLIC: icall_feedback.c's g_icall_seen array and the dump/init functions
# are compiled under this guard. Anything linking xbox_kernel (the generated
# recompiled code included) reads/writes g_icall_seen through the
# RECOMP_ICALL_OBSERVE macro in recomp_icall_feedback.h, so the define has to
# be visible on both sides of that link, not just inside this library.
# Matches config/game.cmake, which defines the same macro for the NFL2K5
# target itself.
target_compile_definitions(xbox_kernel PUBLIC RECOMP_ICALL_FEEDBACK)
