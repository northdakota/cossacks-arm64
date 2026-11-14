cmake_minimum_required(VERSION 3.20)
project(cossacks LANGUAGES CXX)

# Host for the original Cossacks engine (dmcr.exe), derived from zeus-poseidon-arm64
# (https://github.com/nic0lasm/zeus-poseidon-arm64, GPL-2.0-or-later). See README.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)

# Everything that replaces Win32/DirectX around the original code (x86 emulated by Unicorn, GPL-2.0).
add_library(cossacks_win32 STATIC
    src/win32/audio.cpp
    src/win32/cpu.cpp
    src/win32/crash_report.cpp
    src/win32/ddraw.cpp
    src/win32/desktop.cpp
    src/win32/dsound.cpp
    src/win32/kernel32.cpp
    src/win32/kernel32_files.cpp
    src/win32/kernel32_memory.cpp
    src/win32/modules.cpp
    src/win32/native.cpp
    src/win32/pe_image.cpp
    src/win32/process.cpp
    src/win32/system.cpp
    src/win32/text.cpp
    src/win32/user32.cpp
    src/win32/vfs.cpp
    src/win32/winmm.cpp)
target_include_directories(cossacks_win32 PUBLIC src)
target_link_libraries(cossacks_win32 PUBLIC PkgConfig::UNICORN PkgConfig::FFMPEG SDL2::SDL2)
if(APPLE)
    target_link_libraries(cossacks_win32 PUBLIC "-framework CoreFoundation")
endif()

# The commit the launcher is built from, for crash reports (generated/version.h, COSSACKS_VERSION).
add_custom_target(cossacks_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/version.h
            -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version.h)

add_executable(cossacks-run src/tools/cossacks_run/main.cpp)
target_link_libraries(cossacks-run PRIVATE cossacks_win32)
target_include_directories(cossacks-run PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(cossacks-run cossacks_version)
