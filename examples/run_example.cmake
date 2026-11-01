# Runs one example program and checks that it exits with 0 and prints exactly what the
# .expected file says. Usage: cmake -D EMULATOR=... -D PROGRAM=... -D EXPECTED=... -P <this file>
# With -D FRAMES=<file>, if that file exists, the frames the program displays must also match it
# (one line per frame, with its SHA-1, as `ps3emu run --frames` writes them).

set(frame_options "")
if(DEFINED FRAMES AND EXISTS ${FRAMES})
    set(frame_log ${PROGRAM}.frames)
    set(frame_options --frames ${frame_log})
endif()

execute_process(
    COMMAND ${EMULATOR} run ${frame_options} ${PROGRAM}
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE exit_status)

file(READ ${EXPECTED} expected)

if(NOT exit_status EQUAL 0)
    message(FATAL_ERROR "exited with ${exit_status}\n${errors}\noutput:\n${output}")
endif()
if(NOT output STREQUAL expected)
    message(FATAL_ERROR "output differs from ${EXPECTED}\n--- got:\n${output}")
endif()
if(frame_options)
    file(READ ${FRAMES} expected_frames)
    file(READ ${frame_log} frames)
    if(NOT frames STREQUAL expected_frames)
        message(FATAL_ERROR "frames differ from ${FRAMES}\n--- got:\n${frames}")
    endif()
endif()
