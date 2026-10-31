include(FetchContent)

# Uses an installed Catch2 if one is found; otherwise downloads it.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS 3
)
FetchContent_MakeAvailable(Catch2)

add_executable(ps3emu_tests
    common/bits_test.cpp
    common/endian_test.cpp
    core/memory_test.cpp
    core/system_test.cpp
    cpu/ppu/assembler_test.cpp
    cpu/ppu/branch_test.cpp
    cpu/ppu/decoder_test.cpp
    cpu/ppu/integer_test.cpp
    cpu/ppu/load_store_test.cpp
    loader/elf_test.cpp
    loader/program_test.cpp
    lv2/kernel_test.cpp
)
target_include_directories(ps3emu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps3emu_tests PRIVATE ps3emu_core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(ps3emu_tests)
