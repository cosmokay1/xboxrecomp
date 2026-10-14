cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# Windows uses real Win32 primitives plus guarded guest-memory copies. Linux
# also carries the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(WIN32)
    add_library(platform STATIC guest_memory.c)
    target_include_directories(platform PUBLIC ${XBOXRECOMP_SRC_DIR})
else()
    add_library(platform STATIC win32_compat.c)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(platform PRIVATE guest_memory.c)
    endif()
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads m)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
