# ps3-emu

Seeing if giving Anthropic $20/month is enough to fulfill a childhood dream.

An accuracy-focused PlayStation 3 emulator, built for research, homebrew development and learning
how the Cell processor works.

**Status:** milestone M1 is done. The emulator runs PowerPC programs on the PS3's main CPU core
(the PPU): all integer, branch, load/store and atomic instructions, plus enough of the PS3 kernel
to print text and exit. See the [spec](docs/SPEC.md) for the architecture and roadmap.

## Goals

- **Exact:** every CPU instruction behaves like the hardware, flags and edge cases included.
- **Deterministic:** the same inputs give the same output, so runs can be replayed and compared.
- **Inspectable:** tracing, logging and GDB debugging are built in.
- **Readable:** the code is written to be understood.

Running commercial games at full speed is not an early goal.

## Building

Requires CMake 3.24+, Ninja and a C++20 compiler (clang 17+ or GCC 13+).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The [example programs](examples/) also need a PowerPC cross-compiler, which LLVM provides:
`brew install llvm lld` on macOS, or `apt install clang lld` on Linux. Without it, the examples
are skipped.

## Usage

```
$ ./build/src/ps3emu run build/examples/hello.elf
Hello from the PPU!

$ ./build/src/ps3emu run build/examples/selftest.elf
34 of 34 checks passed

$ ./build/src/ps3emu run --trace build/examples/hello.elf
0x00010000  38600000  li r3, 0
0x00010004  3c800002  lis r4, 2
0x00010008  38840008  addi r4, r4, 8
...
```

| Command | What it does |
|---|---|
| `ps3emu run <file.elf>` | Runs a program; exits with the program's exit status. |
| `--trace` | Prints every instruction as it executes (to stderr). |
| `--max-instructions N` | Stops after N instructions. |
| `ps3emu info <file.elf>` | Prints the ELF header and segment table. |

## Legal

This is a clean-room implementation, based only on public documentation. It contains no Sony
firmware, keys or code. You must supply your own firmware and software.

## License

[0BSD](LICENSE)
