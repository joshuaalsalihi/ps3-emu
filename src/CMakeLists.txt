# Emulator library: everything except the frontend, so tests can link against it.
add_library(ps3emu_core STATIC
    common/png.cpp
    common/sha1.cpp
    common/soft_float.cpp
    core/memory.cpp
    core/range_allocator.cpp
    core/system.cpp
    cpu/ppu/assembler.cpp
    cpu/ppu/branch.cpp
    cpu/ppu/disassembler.cpp
    cpu/ppu/floating_point.cpp
    cpu/ppu/integer.cpp
    cpu/ppu/interpreter.cpp
    cpu/ppu/load_store.cpp
    cpu/ppu/opcodes.cpp
    cpu/ppu/operands.cpp
    cpu/ppu/vector_float.cpp
    cpu/ppu/vector_integer.cpp
    cpu/ppu/vector_permute.cpp
    cpu/spu/assembler.cpp
    cpu/spu/branch.cpp
    cpu/spu/channels.cpp
    cpu/spu/control.cpp
    cpu/spu/disassembler.cpp
    cpu/spu/dma.cpp
    cpu/spu/floating_point.cpp
    cpu/spu/integer.cpp
    cpu/spu/interpreter.cpp
    cpu/spu/load_store.cpp
    cpu/spu/opcodes.cpp
    cpu/spu/operands.cpp
    cpu/spu/shift_rotate.cpp
    cpu/spu/spe.cpp
    cpu/spu/spu_float.cpp
    gpu/rasterizer.cpp
    gpu/reference_backend.cpp
    hle/cell_gcm_sys.cpp
    hle/cell_sysutil.cpp
    hle/linker.cpp
    hle/nid.cpp
    hle/sys_prx_for_user.cpp
    loader/elf.cpp
    loader/process_info.cpp
    loader/program.cpp
    loader/spu_elf.cpp
    lv2/kernel.cpp
    lv2/kernel_event.cpp
    lv2/kernel_memory.cpp
    lv2/kernel_spu.cpp
    lv2/kernel_sync.cpp
    lv2/sys_event.cpp
    lv2/sys_fs.cpp
    lv2/sys_memory.cpp
    lv2/sys_ppu_thread.cpp
    lv2/sys_process.cpp
    lv2/sys_spu.cpp
    lv2/sys_sync.cpp
    lv2/sys_time.cpp
    lv2/sys_tty.cpp
    rsx/fragment_program.cpp
    rsx/rsx.cpp
    rsx/shader_float.cpp
    rsx/vertex_program.cpp
    platform/host_memory_posix.cpp
)
target_include_directories(ps3emu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps3emu_core PUBLIC ps3emu_options)

add_executable(ps3emu frontend/cli/main.cpp)
target_link_libraries(ps3emu PRIVATE ps3emu_core)
