# Example programs

Small programs for the emulated PPU, cross-compiled with LLVM. Each one also runs as an
integration test: its output must match its `.expected` file exactly.

| Program | Shows |
|---|---|
| `hello.S` | The bare minimum, in assembly: one syscall to print, one to exit. |
| `fibonacci.c` | Recursion, the stack and 64-bit division (for printing numbers). |
| `primes.c` | A sieve over a zero-initialized (.bss) array. |
| `selftest.c` | 34 checks of compiler-generated code: 128-bit multiplies, shifts, atomics, jump tables, indirect calls. |
| `float_selftest.c` | Exact bit patterns of compiler-generated floating point (double and single), compared with a native run. |
| `vector_selftest.c` | 28 checks of AltiVec intrinsics against scalar code computing the same results. |

The `psl1ght/` directory holds real PS3 homebrew, written against the
[PSL1GHT](https://github.com/ps3dev/PSL1GHT) SDK and compiled with the PS3 toolchain (GCC):

| Program | Shows |
|---|---|
| `psl1ght/hello.c` | `printf` through newlib, PSL1GHT's runtime, imported system library calls and LV2. |
| `psl1ght/threads.c` | Threads, lightweight mutexes and condition variables, semaphores, join and sleep. |
| `psl1ght/floats.c` | `printf` of doubles (to hundreds of digits) and square roots, compared with a native run. |
| `psl1ght/spu_threads.c` | Two SPU threads (`spu_threads_spu.c`, built with `spu-gcc`): mailboxes, DMA gets and puts, and an atomic counter shared with the PPU. |
| `psl1ght/rsx_clear.c` | The RSX through libgcm: display buffers, clears and vsync flips. `rsx_clear.frames` holds the hashes of the frames it shows. |
| `psl1ght/rsx_triangle.c` | A smooth-shaded triangle, with a vertex and a fragment program (`rsx_triangle.vcg`, `.fcg`) assembled by the toolchain's `cgcomp`. |
| `psl1ght/rsx_cube.c` | Two lit, textured cubes turning over three frames, one cutting through the other: matrices in vertex program constants, an index array, a depth buffer, back-face culling and bilinear filtering. |
| `psl1ght/rsx_scene.c` | A floor running to the horizon with a swizzled, mipmapped texture (trilinear filtering), a ring cut out by the alpha test from a 16-bit texture in main memory, and blended panes of glass, in a viewport smaller than the screen. |
| `psl1ght/spu_events.c` | Event queues and ports in both directions between the PPU and an SPU thread (`spu_events_spu.c`), and `spu_printf`. |

PSL1GHT's own samples are built too, unmodified, from the PSL1GHT sources in the
`ps3emu-cgcomp` image: their Cg shaders are compiled there (`extract-samples.sh`), and the rest
by their own makefiles in the toolchain image (`cgcomp-prebuilt` hands them the shaders).
Samples that never exit are checked over their first frames:

| Sample | Shows |
|---|---|
| `graphics/rsxtest` | A textured sphere and torus with per-pixel diffuse and specular lighting, and text from a debug font; its first 2 frames. Cg's fragment programs use half-precision registers and condition codes. |

Output never depends on how the threads interleave: either only the main thread prints, after its
workers finish, or each print waits for the one before it (as in `spu_events.c`, where every
`spu_printf` returns before the SPU sends its result). A program with a `<name>_spu.c` gets it compiled for the SPU and embedded
as the array `<name>_spu_elf`. The toolchain runs in Docker: build its image once with
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
- It targets `-mcpu=970` (the PowerPC G5), the closest LLVM CPU to the Cell's PPU, with
  `-ffp-contract=off` so neither this build nor the native one fuses `a * b + c` differently.

## Expected output

The `.expected` files never come from the emulator. For the LLVM examples, they come from
compiling the same C for the host and running it natively. For the PSL1GHT examples, the values
are simple arithmetic (for example 1 + 2 + … + 100 = 5050).
