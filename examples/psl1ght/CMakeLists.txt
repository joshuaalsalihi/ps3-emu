# PSL1GHT homebrew programs, compiled with the real PS3 toolchain (GCC + PSL1GHT) inside the
# Docker image that tools/build-psl1ght-image.sh builds. Skipped if that image isn't available.

find_program(PS3EMU_DOCKER docker)
if(PS3EMU_DOCKER)
    execute_process(
        COMMAND ${PS3EMU_DOCKER} image inspect ps3emu-psl1ght
        RESULT_VARIABLE image_missing OUTPUT_QUIET ERROR_QUIET)
endif()
if(NOT PS3EMU_DOCKER OR image_missing)
    message(STATUS "PSL1GHT examples: skipped (run tools/build-psl1ght-image.sh to enable)")
    return()
endif()
message(STATUS "PSL1GHT examples: using the Docker image ps3emu-psl1ght")

# add_psl1ght_program(<name>) builds <name>.elf from <name>.c and tests it against <name>.expected.
# If <name>_spu.c exists, it is compiled for the SPU and embedded as the array <name>_spu_elf.
function(add_psl1ght_program name)
    set(elf ${CMAKE_CURRENT_BINARY_DIR}/${name}.elf)
    set(spu_build "")
    set(spu_object "")
    set(sources ${name}.c)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}_spu.c)
        string(CONCAT spu_build
            "spu-gcc -O2 -Wall -I\$PS3DEV/spu/include /source/${name}_spu.c"
            " -L\$PS3DEV/spu/lib -lsputhread -o /build/${name}_spu.elf"
            " && bin2s /build/${name}_spu.elf > /build/${name}_spu.S && ")
        set(spu_object /build/${name}_spu.S)
        list(APPEND sources ${name}_spu.c)
    endif()
    file(GLOB headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

    # sprxlinker fills in the import counts, as PSL1GHT's own build does.
    string(CONCAT build_command ${spu_build}
        "ppu-gcc -mcpu=cell -O2 -Wall -I\$PS3DEV/ppu/include /source/${name}.c ${spu_object}"
        " -L\$PS3DEV/ppu/lib -lrt -llv2 -lm -o /build/${name}.elf && sprxlinker /build/${name}.elf")

    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${PS3EMU_DOCKER} run --rm
                -v ${CMAKE_CURRENT_SOURCE_DIR}:/source:ro
                -v ${CMAKE_CURRENT_BINARY_DIR}:/build
                ps3emu-psl1ght sh -c ${build_command}
        COMMAND ${CMAKE_COMMAND} -D FILE=${elf} -P ${CMAKE_CURRENT_SOURCE_DIR}/require_file.cmake
        DEPENDS ${sources} ${headers}
        COMMENT "Compiling PSL1GHT ${name}.elf (in Docker)"
        VERBATIM)
    add_custom_target(psl1ght_${name} ALL DEPENDS ${elf})

    add_test(NAME example.psl1ght.${name}
        COMMAND ${CMAKE_COMMAND}
            -D EMULATOR=$<TARGET_FILE:ps3emu>
            -D PROGRAM=${elf}
            -D EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${name}.expected
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../run_example.cmake)
endfunction()

add_psl1ght_program(hello)
add_psl1ght_program(threads)
add_psl1ght_program(floats)
add_psl1ght_program(spu_threads)
add_psl1ght_program(spu_events)
