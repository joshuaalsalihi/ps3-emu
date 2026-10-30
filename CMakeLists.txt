cmake_minimum_required(VERSION 3.24)
project(ps3emu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(PS3EMU_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
option(PS3EMU_BUILD_TESTS "Build the unit tests" ON)

# Compiler settings shared by every target in this project (not third-party code).
add_library(ps3emu_options INTERFACE)
target_compile_options(ps3emu_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
if(PS3EMU_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(sanitizers -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_compile_options(ps3emu_options INTERFACE ${sanitizers})
    target_link_options(ps3emu_options INTERFACE ${sanitizers})
endif()

add_subdirectory(src)

if(PS3EMU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
