# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository.

## Project

An accuracy-focused PS3 emulator written in C++20. Correctness, determinism and debuggability come
before speed. The design lives in [`docs/SPEC.md`](docs/SPEC.md); read it before making
architectural changes, and update it when a design decision changes.

## Readability first

The code is meant to be read and learned from. When in doubt, choose the clearer option over the
cleverer or faster one.

- Prefer small functions with descriptive names and early returns.
- Keep each instruction handler close to the manual's pseudocode, named after its mnemonic
  (e.g. `addic`, `rotqby`).
- Avoid macros, deep template tricks and heavy abstraction unless they remove real duplication.
- Optimize only when a measurement shows the need, and keep a readable reference path.
- Match the style of the surrounding code.

## Comments and docs

- Keep comments short. Explain *why*, not *what*.
- For hardware behavior, cite the public source: `// SPU ISA v1.2, "Floating-Point Add"`.
- Docs use plain language and short sections. Define jargon on first use, or link to the glossary
  in `docs/SPEC.md`.

## C++ conventions

- Format with `clang-format` (config in `.clang-format`).
- Names: `PascalCase` types; `snake_case` functions and variables; `k_snake_case` constants;
  trailing `_` for private members.
- Use the fixed-width aliases `u8`…`u64` and `s8`…`s64`.
- Guest memory is accessed only through the `Memory` API or `GuestPtr<T>`, never through raw host
  pointers. Guest data is big-endian; use the `be<T>` types for guest structs.
- Guest errors are returned as PS3 error codes. Host failures (out of memory, bad file) produce a
  clear error message, not a silent fallback.

## Determinism rules

- Never read the host clock, random numbers or other host state from emulation code. Time comes from
  the scheduler's virtual clock; external input goes through the replay log.
- Never let iteration order of unordered containers affect guest-visible state.
- All emulator state is owned by `System`, with no globals, so it can be saved and restored.

## Clean-room policy

This project is clean-room and 0BSD-licensed.

- **Allowed:** public hardware manuals and ISA specs, behavior descriptions on public wikis
  (e.g. psdevwiki), SDK headers for ABI facts and constants, and observing other programs' output.
- **Not allowed:** reading, copying, translating or paraphrasing source code from other PS3
  emulators (e.g. RPCS3) or from proprietary/leaked SDKs. Do not search for or open such code.
- Never commit firmware, keys, game files or other copyrighted material.

## Adding an instruction

1. Write the handler next to similar ones (e.g. `src/cpu/ppu/integer.cpp`), following the manual's
   pseudocode.
2. Add a row to that file's opcode table. The decoder, disassembler and assembler all use it.
3. Add a reference encoding to `tests/cpu/ppu/assembler_test.cpp`, generated with
   `llvm-mc -triple=powerpc64 -show-encoding`. A test fails if any opcode lacks one.
4. Add behavior tests using the `PpuTest` fixture, which runs assembly text:
   `run({"addi r3, r1, -8"}); CHECK(r(3) == ...);`

## Adding a syscall or library function

- **Syscall:** add a handler and a table row to the matching `src/lv2/sys_*.cpp` file. The number,
  name and signature come from psdevwiki ("LV2 Functions and Syscalls"). Keep the logic in a
  `Kernel` method, and keep argument reading and result writing in the handler.
- **HLE library function:** add it to the module's table (e.g. `src/hle/sys_prx_for_user.cpp`)
  under its official name. Its NID is computed from the name; check it against a known NID in
  `tests/hle/nid_test.cpp`.
- Struct layouts may come from SDK headers (PSL1GHT is MIT-licensed). Remember that firmware
  structs use 32-bit pointers even though PSL1GHT programs use 64-bit ones.
- A handler either sets a result or makes the thread wait; see `Kernel::wait`. Never block the host.
- Test through `KernelTest` (`tests/support/kernel_test.h`), which makes syscalls from fake threads.

## Testing

- Every instruction gets unit tests covering normal cases and edge cases (flags, overflow, NaN,
  denormals).
- Every bug fix comes with a test that fails without the fix.
- There is no PS3 hardware yet. Derive expected values from the manuals and say so in the test.
  Mark anything uncertain with `// UNVERIFIED:` so it can be checked on hardware later.
- Don't trust hand-computed constants for large values; check them independently (e.g. with
  Python, or by running the same C natively, as the `examples/*.expected` files do).

## Build and test

Requires CMake 3.24+, Ninja and a C++20 compiler (clang 17+ or GCC 13+). Catch2 is downloaded
automatically if it isn't installed.

```sh
cmake -S . -B build -G Ninja      # Debug build with ASan/UBSan by default
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/ps3emu run --trace build/examples/hello.elf
```

- Warnings are errors. Fix them; don't suppress them.
- Run `clang-format -i` on changed files before committing. CI checks formatting.
- Tests mirror the source tree: `src/loader/elf.cpp` → `tests/loader/elf_test.cpp`. Shared
  fixtures live in `tests/support/`.
- Test inputs are built in code (`tests/support/elf_builder.h`, the PPU assembler) rather than
  committed as binaries, so each test shows exactly what it runs.
- The programs in `examples/` need LLVM (`brew install llvm lld` or `apt install clang lld`).
  The PSL1GHT ones need the Docker image from `tools/build-psl1ght-image.sh`, and a build
  directory Docker can write to (on macOS, somewhere under /Users). CMake skips whatever it
  can't build, with a message.
