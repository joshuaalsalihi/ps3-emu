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

# add_psl1ght_program(<name>) builds <name>.elf from <name>.c and tests it against <name>.expected,
# and the frames it displays against <name>.frames if that exists.
# If <name>_spu.c exists, it is compiled for the SPU and embedded as the array <name>_spu_elf.
# Shaders <name>.vcg and <name>.fcg are assembled and embedded too (see below).
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
    # Shaders in ARB assembly (<name>.vcg, <name>.fcg) are assembled by cgcomp and embedded as
    # <name>_vp_vpo and <name>_fp_fpo.
    set(shader_build "")
    set(shader_objects "")
    foreach(kind vp fp)
        if(kind STREQUAL vp)
            set(extension vcg)
            set(flag -v)
        else()
            set(extension fcg)
            set(flag -f)
        endif()
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}.${extension})
            string(CONCAT shader_build ${shader_build}
                "cgcomp -a ${flag} /source/${name}.${extension} /build/${name}_${kind}.${kind}o"
                " && cd /build && bin2s ${name}_${kind}.${kind}o > ${name}_${kind}.S && cd / && ")
            string(APPEND shader_objects " /build/${name}_${kind}.S")
            list(APPEND sources ${name}.${extension})
        endif()
    endforeach()
    file(GLOB headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

    # sprxlinker fills in the import counts, as PSL1GHT's own build does.
    string(CONCAT build_command ${spu_build} ${shader_build}
        "ppu-gcc -mcpu=cell -O2 -Wall -I\$PS3DEV/ppu/include /source/${name}.c ${spu_object}"
        "${shader_objects}"
        " -L\$PS3DEV/ppu/lib -lrsx -lgcm_sys -lsysutil -lrt -llv2 -lm -o /build/${name}.elf"
        " && sprxlinker /build/${name}.elf")

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
            -D FRAMES=${CMAKE_CURRENT_SOURCE_DIR}/${name}.frames
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../run_example.cmake)
    add_test(NAME example.psl1ght.${name}.resume
        COMMAND ${CMAKE_COMMAND}
            -D EMULATOR=$<TARGET_FILE:ps3emu>
            -D PROGRAM=${elf}
            -D EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${name}.expected
            -D FRAMES=${CMAKE_CURRENT_SOURCE_DIR}/${name}.frames
            -D SPLIT=10000
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../run_example.cmake)
endfunction()

add_psl1ght_program(hello)
add_psl1ght_program(threads)
add_psl1ght_program(floats)
add_psl1ght_program(spu_threads)
add_psl1ght_program(spu_events)
add_psl1ght_program(rsx_clear)
add_psl1ght_program(rsx_triangle)
add_psl1ght_program(rsx_cube)
