# PS3 Emulator — Design Spec

**Status:** Draft v0.3 · 2026-10-31 · M1 complete

An accuracy-focused PlayStation 3 emulator for research, homebrew development and studying the
Cell processor. Correctness and debuggability come first; speed comes later.

---

## 1. Goals

- **Exact CPU behavior.** Every PPU and SPU instruction matches the hardware, including flags and
  edge cases.
- **Deterministic.** The same inputs always produce the same result, bit for bit. This gives us
  replay, save states and trace diffing.
- **Easy to inspect.** Tracing, logging and GDB debugging are built in from the start.
- **Portable.** Developed on macOS (Apple Silicon); host-specific code sits behind small interfaces.
- **Clean-room and permissively licensed.** See §11.

**Not goals (for now):** full-speed commercial games, cycle-exact timing, online features,
backward compatibility (PS1/PS2), or emulating the hypervisor.

---

## 2. Glossary

| Term | Meaning |
|---|---|
| **Cell** | The PS3's processor: one PPU plus several SPUs. |
| **PPU** | The main CPU core. 64-bit PowerPC with VMX (AltiVec) vector instructions. |
| **SPU** | A vector co-processor. Games get 6 of them. Each has its own 256 KB **local store** (LS). |
| **MFC** | Each SPU's DMA engine. It moves data between local store and main memory. |
| **RSX** | The GPU, an NVIDIA G70 derivative. The CPU drives it by writing commands to a **FIFO**. |
| **LV2** | The PS3's operating-system kernel. Games call it through **syscalls**. |
| **PRX** | A system library (like a DLL). Functions are identified by a 32-bit **NID** hash. |
| **HLE / LLE** | High-level emulation (reimplement a function's behavior) vs. low-level emulation (run the original code). |
| **ELF / SELF** | Executable formats. SELF is a signed (often encrypted) wrapper around ELF. |
| **PSL1GHT** | An open-source homebrew SDK for the PS3. |

---

## 3. Hardware at a glance

| Component | Key facts | What makes it tricky |
|---|---|---|
| PPU | 3.2 GHz, 2 hardware threads, big-endian | Precise floating-point and vector flags (FPSCR, VSCR). |
| SPU ×6 | 128 × 128-bit registers, 256 KB local store | Single-precision floats are **not IEEE**: no denormals, NaN or infinity. |
| MFC | DMA queue (16 entries), tag groups, atomic operations | Atomics use 128-byte "reservations" shared with the PPU. |
| RSX | Command FIFO, programmable shaders | Shader translation; keeping GPU and CPU views of memory in sync. |
| Memory | 256 MB main + 256 MB video; 32-bit guest addresses | Byte swapping on every access. |
| Timebase | 79.8 MHz | Drives the emulator's virtual clock. |

---

## 4. Architecture

```
 Frontend (CLI)  ── args, config, GDB server
        │
 System ── owns all emulator state
   ├─ Scheduler ── runs every guest context in turn on a virtual clock
   │    ├─ PPU threads
   │    ├─ SPU threads
   │    └─ RSX command processor
   ├─ Memory ── guest address space, byte swapping, reservations
   ├─ LV2 kernel (HLE) ── syscalls and kernel objects
   ├─ Module manager ── loads libraries, resolves NIDs, binds HLE functions
   ├─ VFS ── maps guest paths (/dev_hdd0, …) to host folders
   └─ Devices ── audio, controller input, timers
        │
 Host layer ── CPU backend (interpreter) · GPU backend (null, Metal) · audio · input · platform
```

**Three rules hold everywhere:**
1. **Each instruction is defined once**, in the interpreter. Faster backends must match it.
2. **Guest-visible state only changes through `System`.** No globals, so save states are just
   serialization.
3. **Nothing reads the host clock or other host state directly.** Time comes from the virtual clock;
   input and file data go through a replay log.

---

## 5. CPU emulation

### 5.1 Shared core interface
```cpp
class Core {
public:
    // Run until the cycle budget is used up or something needs the scheduler
    // (a syscall, a blocking wait, a breakpoint). Returns the cycles used.
    virtual StepResult run(u64 cycle_budget) = 0;
};
```
The scheduler only talks to `Core`, so an interpreter and a future JIT are interchangeable.

### 5.2 Instruction definitions
Each instruction is one row in a C++ opcode table that sits next to its handlers: mnemonic, format,
opcode numbers, handler and operand layout (e.g. `"rD, d(rA)"`). The decoder, disassembler and a
small assembler are all driven by these tables, so they cannot disagree. Handlers are named after
the mnemonic and follow the manual's pseudocode.

The assembler exists so tests can be written as readable assembly text.

### 5.3 PPU
- Full register state: GPR, FPR, VR, CR, LR, CTR, XER, FPSCR, VSCR.
- The `sc` instruction calls into the LV2 kernel (syscall number in r11).
- Each of the two hardware threads is modeled as an independent context.
- Not implemented yet: the load/store string instructions (`lswi`, `lswx`, `stswi`, `stswx`),
  which compilers rarely emit.

### 5.4 SPU
- **SPU floats:** a dedicated `spu_float` type implements the non-IEEE single-precision rules.
- **Channels:** mailboxes, signals, events, the decrementer and MFC commands. A channel access that
  would stall becomes a scheduler wait.
- **MFC:** DMA takes time per the timing model, so code that polls for completion behaves
  realistically. An "instant DMA" debug option helps isolate bugs.

### 5.5 Atomics
PPU (`lwarx`/`stwcx.`) and SPU (`getllar`/`putllc`) atomics share one mechanism:
- `Memory` keeps a short list of active reservations, one per thread, each covering a 128-byte line.
- Every write removes the reservations on the lines it touches.
- A conditional store succeeds only if the thread's reservation is still there.
- The same check will raise the SPU "reservation lost" event.

### 5.6 JIT (later)
Not planned for early milestones. When added, it must pass differential tests against the
interpreter. On macOS it needs `MAP_JIT` memory and the `allow-jit` entitlement.

---

## 6. Scheduling and timing

**Deterministic mode (default):**
- One host thread runs all guest contexts (PPU, SPU, RSX) round-robin, in fixed slices of guest
  cycles.
- Each context counts its cycles; a shared **virtual clock** derives from them. The timebase, SPU
  decrementer, kernel timers, vblank and audio all use it.
- Blocking calls (sleep, lock wait, mailbox stall) yield and register a wake-up condition.

**Timing levels:**

| Level | Model | Use |
|---|---|---|
| T0 | 1 cycle per instruction | Bring-up |
| T1 | Per-instruction latency tables; DMA cost by size | **Default target** |
| T2 | SPU dual-issue pipeline model | Research |

A multi-threaded mode for speed may come later. It will not support replay or trace diffing.

---

## 7. Operating system

| Layer | Approach |
|---|---|
| Hypervisor (LV1) | Not emulated; games never call it. |
| LV2 kernel syscalls | **HLE** |
| Core libraries (graphics, filesystem, input, audio, threads, `cellSpurs`) | **HLE** |
| Pure-compute libraries (codecs, compression) | **LLE** if the user supplies firmware; otherwise HLE or "unimplemented". |

- **HLE functions** are registered in a table: module, NID, name, handler and signature. Argument
  passing is generated from the signature.
- **Missing functions** bind to a stub that logs the call. Each module can be configured to either
  continue or halt.
- **Kernel objects** (threads, mutexes, semaphores, event queues, SPU thread groups, …) live in one
  serializable ID table.
- **Executable formats**, in order: ELF → fake-signed SELF (PSL1GHT output) → retail SELF/SPRX using
  keys derived from user-supplied firmware. Decryption lives in an optional `crypto/` module.
- **VFS** maps `/dev_hdd0`, `/dev_bdvd`, `/app_home`, `/dev_flash`, etc. to host folders, using PS3
  semantics (case sensitivity, error codes).

---

## 8. Memory

- Reserve 4 GB of host address space for the 32-bit guest space.
- All guest accesses go through the `Memory` API. It handles byte swapping, permissions and
  watchpoints. HLE code uses bounds-checked `GuestPtr<T>` instead of raw pointers.
- Pages are tracked at 4 KB in software, independent of the host page size (16 KB on Apple
  Silicon).
- Exact region base addresses are to be confirmed from public documentation before
  implementation.

---

## 9. Graphics (RSX)

```
FIFO → command decoder → RSX state (all registers, inspectable) → GPU backend
```
- **State model:** independent of the backend, and dumpable at any draw call. Semaphores, reports and
  flip/vblank events are handled here, not in the backend.
- **Shaders:** RSX vertex and fragment programs are decoded into a shared IR, then emitted as Metal
  Shading Language (SPIR-V later). Cached by hash.
- **Surfaces:** a cache tracks render targets. A CPU write to a surface's memory invalidates it;
  reading it back triggers a write-back. Start conservative; optimize later.
- **Backends:** `Null` (renders nothing; used by CI), `Metal` (first real backend), `Vulkan` (later).

---

## 10. Verifying accuracy

There is **no PS3 hardware available yet**, so ground truth comes from:
1. **Architecture manuals** (Cell, SPU ISA, PowerPC). Instruction tests are written from their
   pseudocode.
2. **LLVM, as an independent implementation.** Our assembler and disassembler are checked against
   instruction encodings produced by LLVM's PowerPC assembler, covering every opcode.
3. **Native execution.** The example programs are C; their expected output comes from compiling
   the same C for the host and running it there.
4. **IBM's Cell simulator** (systemsim), if a copy can be obtained.
5. **Black-box comparison with other emulators**: run the same test program and compare its output.
   We never read their source (see §11).

**Hardware harness (prepared now, used later):** a PSL1GHT program that runs instruction test cases
on a real PS3 and dumps the results in our test format. Once hardware is available, it validates
everything above.

**Test suites:**
- Instruction unit tests: inputs → expected outputs and flags, for every instruction.
- Example programs (`examples/`): small guest programs built with LLVM, whose output must match a
  `.expected` file exactly. Later, PSL1GHT programs (threads, SPU mailboxes, DMA, a triangle, an
  audio tone) with known-good logs and framebuffer hashes.
- Determinism tests: two runs must produce identical state hashes.
- CI runs everything headless, using the `Null` GPU backend.

---

## 11. Clean-room policy and license

- **License:** 0BSD (Zero-Clause BSD) — use it for anything, no attribution required.
- **Allowed sources:** public hardware manuals, ISA specs, public wikis that describe behavior
  (e.g. psdevwiki), SDK headers for ABI facts and constants, and observing the output of other
  software.
- **Not allowed:** reading, copying or translating source code from other PS3 emulators
  (e.g. RPCS3), or from leaked/proprietary SDKs.
- Each non-obvious hardware behavior in the code cites its public source.

---

## 12. Debugging

- **GDB** is the only debugger. We implement the GDB remote protocol for PPU threads.
- Current GDB releases no longer support the SPU architecture, so SPU state is exposed through
  GDB `monitor` commands (e.g. `monitor spu regs 0`, `monitor spu ls 0 0x100 64`).
- **Tracing:** instructions (filterable by address), syscalls, HLE calls, DMA and RSX commands.
  A CLI tool diffs two traces.
- **Logging:** per subsystem; unimplemented functions are always logged.
- **Save states:** a full `System` snapshot.

---

## 13. Project layout and tooling

```
src/
  common/      integer types, byte-order helpers
  core/        System, Scheduler, Memory, save states
  cpu/ppu/     instruction defs, decoder, interpreter, disassembler
  cpu/spu/     same, plus spu_float and the MFC
  lv2/         syscalls and kernel objects
  hle/         module manager; one file per HLE library
  loader/      ELF, SELF, PRX, SFO
  crypto/      optional; off by default
  rsx/         FIFO, state, shader IR, surfaces, texture decoding
  gpu/         backends: null, metal
  vfs/  audio/  input/  platform/
  debug/       tracing, GDB server, snapshots
  frontend/    CLI
examples/      guest programs (assembly and C) that run as integration tests
tests/         unit tests, mirroring src/; shared fixtures in tests/support/
tools/         code generators, trace diff
docs/          this spec and notes
```

C++20 · clang · CMake + Ninja · Catch2 · clang-format. Few dependencies: `std::format` instead of
a formatting library, metal-cpp for graphics, LLVM only if a JIT is added. Debug builds use
ASan/UBSan.

---

## 14. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 ✅ | Skeleton | Builds; CLI loads an ELF and lists its segments; CI runs. |
| M1 ✅ | PPU integer core | Integer/branch/load-store/atomic tests pass; compiled C programs run and print. |
| M2 | Kernel basics | Threads, locks, timers and the scheduler work, deterministically; PSL1GHT "hello world" runs. |
| M3 | PPU float + vector | FPU and VMX tests pass, including flags. |
| M4 | SPU | SPU instructions, channels, mailboxes, DMA and atomics pass tests. |
| M5 | First pixels | FIFO + state model; Metal draws a PSL1GHT triangle matching its golden hash. |
| M6 | Debugging | GDB server, trace diff, save states, replay. |
| M7 | Shaders + textures | Homebrew 3D samples render correctly. |
| M8 | Firmware | Fake-signed SELF, LLE libraries from user firmware, `/dev_flash`. |
| M9 | Commercial | A simple commercial game reaches its first frame (research goal). |

---

## 15. Open questions

1. Default time-slice size: settle by experiment once there are multiple threads (M2).
2. Can a copy of IBM systemsim be obtained legally?
3. The initial register state LV2 gives a new process (beyond r1, r2 and pc) is unverified.

---

## 16. References

- *Cell Broadband Engine Programming Handbook*; *Cell BE Architecture*; *SPU ISA*;
  *SPU Language Extensions*
- *PowerPC Architecture Books I–III*; *AltiVec / VMX Programming Environments Manual*
- psdevwiki.com (PS3): syscalls, file formats, RSX
- PSL1GHT: SDK headers and samples, for building test programs
- envytools: public NVIDIA hardware documentation for NV40/G70 commands
