# Emulator library: everything except the frontend, so tests can link against it.
add_library(ps3emu_core STATIC
    loader/elf.cpp
)
target_include_directories(ps3emu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps3emu_core PUBLIC ps3emu_options)

add_executable(ps3emu frontend/cli/main.cpp)
target_link_libraries(ps3emu PRIVATE ps3emu_core)
