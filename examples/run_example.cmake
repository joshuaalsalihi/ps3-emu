# Runs one example program and checks that it exits with 0 and prints exactly what the
# .expected file says. Usage: cmake -D EMULATOR=... -D PROGRAM=... -D EXPECTED=... -P <this file>

execute_process(
    COMMAND ${EMULATOR} run ${PROGRAM}
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
