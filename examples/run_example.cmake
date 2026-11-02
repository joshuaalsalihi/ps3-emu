# Runs one example program and checks that it exits with 0 and prints exactly what the
# .expected file says. Usage: cmake -D EMULATOR=... -D PROGRAM=... -D EXPECTED=... -P <this file>
# With -D FRAMES=<file>, if that file exists, the frames the program displays must also match it
# (one line per frame, with its SHA-1, as `ps3emu run --frames` writes them).
# With -D SPLIT=<n>, the run stops after n instructions and saves its state, and a second
# process resumes it; together they must print and display the same.

# Output files are named per test, since tests of the same program may run in parallel.
set(log_base ${PROGRAM})
if(DEFINED SPLIT)
    set(log_base ${PROGRAM}.split)
endif()

set(frame_options "")
set(resume_frame_options "")
if(DEFINED FRAMES AND EXISTS ${FRAMES})
    set(frame_log ${log_base}.frames)
    set(frame_options --frames ${frame_log})
    set(resume_frame_options --frames ${frame_log}.resumed)
endif()

if(DEFINED SPLIT)
    set(state_file ${log_base}.state)
    execute_process(
        COMMAND ${EMULATOR} run --max-instructions ${SPLIT} --save-state ${state_file}
            ${frame_options} ${PROGRAM}
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE exit_status)
    # A program shorter than SPLIT has already exited; resuming it must change nothing.
    if(NOT exit_status EQUAL 0 AND NOT errors MATCHES "^stopped: reached the limit")
        message(FATAL_ERROR "first part exited with ${exit_status}\n${errors}")
    endif()
    execute_process(
        COMMAND ${EMULATOR} resume ${resume_frame_options} ${state_file}
        OUTPUT_VARIABLE resumed_output
        ERROR_VARIABLE errors
        RESULT_VARIABLE exit_status)
    string(APPEND output "${resumed_output}")
    if(frame_options)
        file(READ ${frame_log}.resumed resumed_frames)
        file(APPEND ${frame_log} "${resumed_frames}")
    endif()
else()
    execute_process(
        COMMAND ${EMULATOR} run ${frame_options} ${PROGRAM}
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE exit_status)
endif()

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
