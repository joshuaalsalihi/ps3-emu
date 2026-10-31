# Emulator library: everything except the frontend, so tests can link against it.
add_library(ps3emu_core STATIC
    core/memory.cpp
    core/system.cpp
    cpu/ppu/assembler.cpp
    cpu/ppu/branch.cpp
    cpu/ppu/disassembler.cpp
    cpu/ppu/integer.cpp
    cpu/ppu/interpreter.cpp
    cpu/ppu/load_store.cpp
    cpu/ppu/opcodes.cpp
    cpu/ppu/operands.cpp
    loader/elf.cpp
    loader/program.cpp
    lv2/kernel.cpp
    platform/host_memory_posix.cpp
)
target_include_directories(ps3emu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps3emu_core PUBLIC ps3emu_options)

add_executable(ps3emu frontend/cli/main.cpp)
target_link_libraries(ps3emu PRIVATE ps3emu_core)
