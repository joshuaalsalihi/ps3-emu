# Example guest programs, cross-compiled for the PPU with LLVM (clang + lld).
# They run as integration tests: each program's output must match its .expected file.
#
# On macOS: brew install llvm lld   (Apple's clang can't target PowerPC)
# On Linux: apt install clang lld

add_subdirectory(psl1ght)

file(GLOB homebrew_lld_dirs /opt/homebrew/opt/lld*/bin /usr/local/opt/lld*/bin)
find_program(PS3EMU_GUEST_CLANG clang HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(PS3EMU_GUEST_LINKER ld.lld HINTS ${homebrew_lld_dirs})

set(guest_target --target=powerpc64-unknown-none)
if(PS3EMU_GUEST_CLANG)
    execute_process(
        COMMAND ${PS3EMU_GUEST_CLANG} ${guest_target} -x assembler -c /dev/null -o /dev/null
        RESULT_VARIABLE clang_cannot_target_ppc OUTPUT_QUIET ERROR_QUIET)
endif()

if(NOT PS3EMU_GUEST_CLANG OR clang_cannot_target_ppc OR NOT PS3EMU_GUEST_LINKER)
    message(STATUS "Examples: skipped (needs a PowerPC-capable clang and ld.lld)")
    return()
endif()
message(STATUS "Examples: using ${PS3EMU_GUEST_CLANG} and ${PS3EMU_GUEST_LINKER}")

set(guest_flags ${guest_target} -mabi=elfv2 -mcpu=ppc64 -O2
    -ffreestanding -fno-builtin -nostdlib -fno-stack-protector)
set(linker_script ${CMAKE_CURRENT_SOURCE_DIR}/ps3.ld)

# add_guest_program(<name> <sources...>) builds <name>.elf from the given sources.
function(add_guest_program name)
    set(objects)
    foreach(source ${ARGN})
        set(object ${CMAKE_CURRENT_BINARY_DIR}/${name}/${source}.o)
        add_custom_command(
            OUTPUT ${object}
            COMMAND ${PS3EMU_GUEST_CLANG} ${guest_flags} -c ${CMAKE_CURRENT_SOURCE_DIR}/${source}
                    -o ${object}
            DEPENDS ${source} lv2.h
            COMMENT "Compiling guest ${name}/${source}")
        list(APPEND objects ${object})
    endforeach()

    set(elf ${CMAKE_CURRENT_BINARY_DIR}/${name}.elf)
    add_custom_command(
        OUTPUT ${elf}
        COMMAND ${PS3EMU_GUEST_LINKER} -T ${linker_script} ${objects} -o ${elf}
        DEPENDS ${objects} ${linker_script}
        COMMENT "Linking guest ${name}.elf")
    add_custom_target(example_${name} ALL DEPENDS ${elf})

    add_test(NAME example.${name}
        COMMAND ${CMAKE_COMMAND}
            -D EMULATOR=$<TARGET_FILE:ps3emu>
            -D PROGRAM=${elf}
            -D EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/${name}.expected
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
endfunction()

add_guest_program(hello hello.S)
add_guest_program(fibonacci crt0.S fibonacci.c)
add_guest_program(primes crt0.S primes.c)
add_guest_program(selftest crt0.S selftest.c)
