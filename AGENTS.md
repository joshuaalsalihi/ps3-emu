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

## Testing

- Every instruction gets unit tests covering normal cases and edge cases (flags, overflow, NaN,
  denormals).
- Every bug fix comes with a test that fails without the fix.
- There is no PS3 hardware yet. Derive expected values from the manuals and say so in the test.
  Mark anything uncertain with `// UNVERIFIED:` so it can be checked on hardware later.

## Build and test

The build is set up in milestone M0. Until then, there is nothing to build.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
