# ps3-emu

Seeing if giving Anthropic $20/month is enough to fulfill a childhood dream.

An accuracy-focused PlayStation 3 emulator, built for research, homebrew development and learning
how the Cell processor works.

**Status:** milestone M4 is done. Real PS3 homebrew built with the PSL1GHT SDK runs unmodified:
multiple threads, locks, condition variables, semaphores, sleeps, `printf` (floats included),
`malloc`, and SPU threads that talk to the PPU through mailboxes, DMA, atomics and event queues
(`spu_printf` included).

- **CPU:** the PS3's main core (the PPU) runs its full user instruction set: integer, branch,
  load/store, atomics, floating point (with every FPSCR flag) and the VMX vector unit, including
  the Cell's own vector modes and instructions. Floating point is computed in software, so results
  are exact and identical on every host.
- **SPU:** the Cell's vector co-processors run their full instruction set, including their
  non-IEEE single-precision arithmetic, with every channel: mailboxes, signals, events, the
  decrementer, and a DMA engine with lists and the atomic lock-line commands. DMA takes virtual
  time, so programs that forget to wait for it misbehave as they would on hardware.
- **Kernel:** the PS3 kernel (LV2) and its core system library are emulated at a high level,
  with deterministic thread scheduling on a virtual clock.
- **RSX (in progress):** the GPU's command FIFO and register state, display setup through
  libgcm, clears and flips. A software reference renderer produces exact, host-independent
  frames, which can be saved as PNGs or checked by hash.

See the [spec](docs/SPEC.md) for the architecture and roadmap.

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

The [example programs](examples/) need extra tools, and are skipped if they're missing:

- **LLVM** for the small assembly and C examples: `brew install llvm lld` on macOS, or
  `apt install clang lld` on Linux.
- **Docker** for the PSL1GHT examples: `tools/build-psl1ght-image.sh` builds the PS3 homebrew
  toolchain from source into an image (this takes a while), then re-run CMake.

## Usage

```
$ ./build/src/ps3emu run build/examples/hello.elf
Hello from the PPU!

$ ./build/src/ps3emu run build/examples/selftest.elf
34 of 34 checks passed

$ ./build/src/ps3emu run build/examples/psl1ght/threads.elf
4 threads added 1..1000: total 2002000
consumer received 1..100: sum 5050
ping-pong: 10 turns
slept at least 5 ms: yes

$ ./build/src/ps3emu run build/examples/psl1ght/spu_threads.elf
group ended: cause 2
spu 0: exit status 100, sum x 3 = 31344
spu 1: exit status 101, sum x 5 = 374800
counter: 3000

$ ./build/src/ps3emu run build/examples/psl1ght/spu_events.elf
spu: waiting for commands
spu: 2 x 3 + 4 = 10
ppu: user event from the calculator, port 2: 2 -> 10
...

$ ./build/src/ps3emu run --trace build/examples/hello.elf
--- thread 0x1000 "main"
0x00010000  38600000  li r3, 0
0x00010004  3c800002  lis r4, 2
0x00010008  38840008  addi r4, r4, 8
0x0001000c  38a00014  li r5, 20
0x00010010  38c00000  li r6, 0
0x00010014  39600193  li r11, 403
0x00010018  44000002  sc
    syscall 403 sys_tty_write
...
```

| Command | What it does |
|---|---|
| `ps3emu run <file.elf>` | Runs a program; exits with the program's exit status. |
| `--trace` | Prints every instruction, syscall, library call and thread switch (to stderr). |
| `--max-instructions N` | Stops after N instructions. |
| `--instant-dma` | Makes SPU DMA complete instantly, to tell timing bugs from logic bugs. |
| `--frames FILE` | Writes the size and SHA-1 of each frame the program displays to FILE. |
| `--dump-frames DIR` | Saves each displayed frame as a PNG in DIR. |
| `--gpu reference\|null` | Renders in software (the default), or not at all. |
| `ps3emu info <file.elf>` | Prints the ELF header and segment table. |

## Legal

This is a clean-room implementation, based only on public documentation. It contains no Sony
firmware, keys or code. You must supply your own firmware and software.

## License

[0BSD](LICENSE)
