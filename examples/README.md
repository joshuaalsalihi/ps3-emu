# Example programs

Small programs for the emulated PPU, cross-compiled with LLVM. Each one also runs as an
integration test: its output must match its `.expected` file exactly.

| Program | Shows |
|---|---|
| `hello.S` | The bare minimum, in assembly: one syscall to print, one to exit. |
| `fibonacci.c` | Recursion, the stack and 64-bit division (for printing numbers). |
| `primes.c` | A sieve over a zero-initialized (.bss) array. |
| `selftest.c` | 34 checks of compiler-generated code: 128-bit multiplies, shifts, atomics, jump tables, indirect calls. |

The `psl1ght/` directory holds real PS3 homebrew, written against the
[PSL1GHT](https://github.com/ps3dev/PSL1GHT) SDK and compiled with the PS3 toolchain (GCC):

| Program | Shows |
|---|---|
| `psl1ght/hello.c` | `printf` through newlib, PSL1GHT's runtime, imported system library calls and LV2. |
| `psl1ght/threads.c` | Threads, lightweight mutexes and condition variables, semaphores, join and sleep. |

Only the main thread prints, after its workers finish, so the expected output holds no matter how
the threads interleave. The toolchain runs in Docker: build its image once with
`tools/build-psl1ght-image.sh`, then re-run CMake.

Build them with the rest of the project, then run one:

```sh
./build/src/ps3emu run build/examples/primes.elf
```

## How they are built

- There is no C library. `lv2.h` provides syscalls and printing; `crt0.S` calls `main` and exits.
- `ps3.ld` places code at 0x10000 and data at 0x20000, like PS3 executables.
- The ELF entry point is a PS3-style function descriptor: a 32-bit code address and TOC pointer.
- The C code uses the ELFv2 ABI, because lld cannot link calls through the older ELFv1
  descriptors the PS3 uses. This only affects how the examples call their own functions.

## Expected output

The `.expected` files never come from the emulator. For the LLVM examples, they come from
compiling the same C for the host and running it natively. For the PSL1GHT examples, the values
are simple arithmetic (for example 1 + 2 + … + 100 = 5050).
