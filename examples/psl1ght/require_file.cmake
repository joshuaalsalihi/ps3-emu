# Fails the build if FILE doesn't exist. Docker can't write to directories it doesn't share with
# the host (Docker Desktop only shares some paths, such as /Users on macOS), and the container
# then "succeeds" without producing anything.
if(NOT EXISTS ${FILE})
    message(FATAL_ERROR "${FILE} was not created. Is the build directory shared with Docker?")
endif()
