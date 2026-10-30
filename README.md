# ps3-emu

Seeing if giving Anthropic $20/month is enough to fulfill a childhood dream.

An accuracy-focused PlayStation 3 emulator, built for research, homebrew development and learning
how the Cell processor works.

**Status:** early development. Milestone M0 is done: the project builds, and the CLI can inspect
PS3 executables. See the [spec](docs/SPEC.md) for the architecture and roadmap.

## Building

Requires CMake 3.24+, Ninja and a C++20 compiler (clang 17+ or GCC 13+).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Usage

```
$ ./build/src/ps3emu info EBOOT.elf
Type:    EXEC (executable)
Entry:   0x00020010 (function descriptor)

Segments (3):
  #   Type         Offset       Address      File size    Mem size     Flags
  0   LOAD         0x1000       0x10000      0x2000       0x2000       R-X
  1   LOAD         0x3000       0x20000      0x100        0x800        RW-
  2   PROC_PARAM   0x3100       0x20100      0x40         0x40         R--
```

## Goals

- **Exact:** every CPU instruction behaves like the hardware, flags and edge cases included.
- **Deterministic:** the same inputs give the same output, so runs can be replayed and compared.
- **Inspectable:** tracing, logging and GDB debugging are built in.
- **Readable:** the code is written to be understood.

Running commercial games at full speed is not an early goal.

## Legal

This is a clean-room implementation, based only on public documentation. It contains no Sony
firmware, keys or code. You must supply your own firmware and software.

## License

[0BSD](LICENSE)
