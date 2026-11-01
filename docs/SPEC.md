# PS3 Emulator — Design Spec

**Status:** Draft v0.9 · 2026-11-01 · M5 in progress: the RSX clears and flips

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
| **SPE** | Synergistic Processor Element: an SPU together with its MFC. |
| **Channel** | The SPU's I/O port to its MFC: mailboxes, signals, events, DMA commands. |
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
 System ── owns all emulator state; its run loop gives each runnable context a turn
   ├─ Memory ── guest address space, byte swapping, reservations
   ├─ LV2 kernel (HLE) ── syscalls, kernel objects, and PPU thread scheduling
   │    └─ PPU threads and SPU threads (the RSX joins the run loop later)
   ├─ HLE linker ── binds imported library functions to their HLE versions by NID
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
  which compilers rarely emit. Instructions the Cell lacks (`popcntb`, `fre`, `frsqrtes`, `mcrxr`)
  are illegal, as on hardware (Cell BE Programming Handbook, A.2.4).

**Floating point.** All arithmetic goes through `common/soft_float`, our own IEEE 754
implementation, never the host FPU: hosts differ in details (such as when they detect a "tiny"
result), and compilers may fuse or reorder host floating-point code. soft_float computes each
result exactly and rounds it once, following PowerPC's rules (tininess before rounding; with
overflow or underflow exceptions enabled, a wrapped exponent). The PPU layer adds NaN priority
(FRA, FRB, FRC), the invalid-operation cases, and every FPSCR bit (FR, FI, FPRF, sticky exceptions,
FX, and the VX and FEX summaries). Enabled exceptions behave as specified (e.g. the target is left
unchanged), but don't yet raise a program interrupt: LV2 runs games with them masked.

**Vector (VMX).** All AltiVec instructions, plus the Cell's own unaligned left/right loads and
stores (`lvlx`, `lvrx`, `stvlx`, `stvrx` and their `l` forms). Vector floating point has two modes:
- *IEEE mode*: single precision rounded to nearest; VSCR[NJ] makes denormals zero.
- *Graphics rounding mode*, the Cell's hardware default (Handbook, A.3.3): rounding toward zero,
  denormals are zero, and no infinities or NaNs (exponent 255 is an ordinary number; overflow
  saturates to 0x7fffffff). The SPU uses the same number format (§5.4), but has its own code: it
  also reports exception flags, and its zero results are always +0.

Estimate instructions (`fres`, `frsqrte`, `vrefp`, `vrsqrtefp`, `vlogefp`, `vexptefp`) come from
hardware tables we don't have. We return values well within the documented accuracy, computed
with soft_float so every host agrees.

### 5.4 SPU
- **State:** 128 registers of 128 bits, a 256 KB local store holding code and data, and the
  FPSCR. Every local store address wraps around (it is masked with LSLR, 0x3ffff), including
  instruction fetch and branch targets.
- **Instructions:** all of the SPU ISA v1.2 except five optional instructions added in that
  version (`dfceq`, `dfcmeq`, `dfcgt`, `dfcmgt`, `dftsv`), which postdate the PS3's Cell and are
  illegal here (UNVERIFIED). Tables, decoder, assembler and disassembler work as on the PPU; the
  syntax is spu-as's (`a $3, $4, $5`), and the reference encodings come from spu-as.
- **Single precision (`spu_float`):** the SPU's "extended range" format is not IEEE. Exponent 255
  is an ordinary number, denormals are zero, results are truncated, overflow saturates to
  ±0x7fffffff, underflow gives +0, and every zero result is +0. The OVF, UNF, DIFF and DBZ flags
  are set per word slot. Built on soft_float, like the PPU's floating point.
- **Double precision:** IEEE 754 through soft_float, with a rounding mode per doubleword slot and
  sticky FPSCR flags. UNVERIFIED: denormal operands are treated as zero (setting DENORM), and any
  NaN operand gives the default NaN (setting NAN). The manual allows either behavior.
- **Estimates:** `frest` and `frsqest` return a base and a step that `fi` interpolates between.
  The hardware's table is not public, so ours is computed: a line per interval, within 2^-10 of
  the true value (UNVERIFIED).
- **Stops:** `stop` and `stopd` end execution with pc after the instruction; halts (`heq`, ...)
  and illegal instructions stop with pc on it. Branch hints, `sync` and `dsync` change nothing
  in an interpreter that runs one instruction at a time. Interrupts aren't modeled yet; the
  branch bits that enable or disable them are only recorded.
- **Channels** (`spu::Channels`, CBEA chapter 9): every channel of Table 9-2, namely the
  mailboxes (inbound four deep, outbound one each), both signal notification registers (overwrite
  or OR mode), the event facility with all its edge-triggered events, the decrementer (counting
  at the timebase rate), tag-group status, list stall-and-notify, atomic status and SRR0. The
  interpreter stops on `rdch`, `rchcnt` and `wrch` with pc on the instruction; `spu::run`
  performs them. Each channel has a count, as the CBEA defines it, and an access stalls exactly
  when that count is 0. A stall leaves pc on the instruction and becomes a scheduler wait. A
  write to `SPU_WrOutIntrMbox` returns from `spu::run` too, since it interrupts the PPU: the
  kernel takes the value straight away.
- **MFC DMA** (`spu::Dma`, CBEA chapter 7): a 16-entry queue of gets, puts, lists (with
  stall-and-notify), signals, barriers and cache hints. The engine runs one command at a time in
  issue order, which satisfies every fence and barrier, and moves the data when a command
  completes, so code that skips the wait sees stale data as on hardware. Each command costs 256
  cycles plus 8 bytes per cycle (UNVERIFIED; see §15). `--instant-dma` makes every transfer
  complete at once, to tell timing bugs from logic bugs. Sizes and alignments are checked as the
  CBEA requires; a bad command stops the SPU with a clear message. DMA to other SPUs' local
  stores (0xf0000000 and up) is not supported yet.

### 5.5 Atomics
PPU (`lwarx`/`stwcx.`) and SPU (`getllar`/`putllc`) atomics share one mechanism:
- `Memory` keeps a short list of active reservations, one per thread, each covering a 128-byte line.
- Every write removes the reservations on the lines it touches.
- A conditional store succeeds only if the thread's reservation is still there.
- An SPU whose reservation disappears through someone else's write gets the "reservation lost"
  event (its own stores don't raise it). `getllar`, `putllc` and `putlluc` run immediately,
  outside the DMA queue; `putqlluc` is queued like a put.

The `spu_threads` example checks this end to end: two SPUs and the PPU increment one counter
thousands of times and must not lose an update.

### 5.6 JIT (later)
Not planned for early milestones. When added, it must pass differential tests against the
interpreter. On macOS it needs `MAP_JIT` memory and the `allow-jit` entitlement.

---

## 6. Scheduling and timing

**Deterministic mode (default):**
- One host thread runs all guest contexts round-robin: PPU threads and SPU threads today; the
  RSX will join the same loop.
- A **virtual clock** counts guest cycles. The timebase, sleeps, timeouts and
  `sys_time_get_current_time` all come from it, never from the host clock.

**PPU threads (implemented in M2):**
- The LV2 kernel picks threads round-robin in creation order. Each turn lasts 1,000 instructions or
  until the thread makes a syscall or HLE call.
- A blocking call (lock, condition variable, semaphore, join, sleep) marks the thread as waiting.
  Waiters wake first come, first served, or by thread priority if the object was created with the
  priority protocol.
- Timeouts are virtual-clock deadlines. When every thread is waiting, the clock jumps to the next
  deadline. If none exists, the run ends as a **deadlock**, listing what each thread waits on.
- Simplification (UNVERIFIED): priorities only decide wake-up order. They don't preempt, so a
  busy-waiting thread can't starve the others.

**SPU threads (implemented in M4):**
- They take turns with the PPU threads: one rotation covers the PPU threads in creation order,
  then the SPU threads. Each turn lasts 1,000 instructions or until the SPU stops or stalls.
- A stalled SPU thread waits until its channel's count is no longer 0. Before each turn, the
  kernel lets every SPU's DMA progress and wakes the threads that can continue.
- A wait on DMA or the decrementer is a timeout like any other: when every thread is waiting, the
  clock jumps to the next DMA completion. Waits on mailboxes, signals and event queues have no
  deadline, so a program that never sends one deadlocks, and the report names the SPU's channel
  or queue.
- Every SPU's actions advance the same virtual clock as the PPU's.

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

- **HLE functions** are registered in a table of official names and handlers. A function's NID
  is the first 4 bytes (little-endian) of SHA-1(name + fixed suffix), so the table needs no hand-copied
  IDs. Tests check the computed NIDs against PSL1GHT's import tables.
- **Linking imports:** the loader reads the PRX_PARAM segment's stub tables. Each imported function
  gets a thunk, `hle n; blr`, and a PS3 function descriptor pointing at it. `hle` is an
  emulator-defined instruction (primary opcode 1, which PowerPC leaves unused). It stops the
  interpreter so `System` can run HLE function `n`.
- **Missing functions** report success (r3 = 0) and log a warning the first time. Making this
  configurable per module (continue or halt) is still to do.
- **Thread-local storage:** each thread gets its own copy of the program's TLS segment, with r13
  pointing 0x7000 bytes past its start (the 64-bit PowerPC ELF convention).
- **Kernel objects** (threads, mutexes, semaphores, event queues, SPU thread groups, …) live in one
  serializable ID table.
- **SPU threads:** a program imports an SPU ELF it embeds (`sys_spu_image_import`), creates an
  SPU thread group, initializes each thread with the image and four arguments (in `$3`-`$6`),
  and starts the group. A thread ends with `stop 0x102` and a group with `stop 0x101`, each with
  its status in the outbound mailbox (PSL1GHT's `spu_thread_exit` and `spu_thread_group_exit`);
  `sys_spu_thread_group_join` reports how the group ended. A group can be started again, from a
  fresh copy of its images. The PPU reaches its SPUs through mailbox, signal and local store
  syscalls. Not implemented yet: raw SPUs, and other stop codes, which end the run with a clear
  fault. That includes `stop 0x20xx`, the SPU C runtime's `exit()` (a Linux convention).
- **Event queues** hold events (four 64-bit words) until a thread receives them; a PPU thread gets
  its event in r4-r7. A PPU event port sends to one queue. Each SPU thread has 64 ports of its
  own: writing `SPU_WrOutIntrMbox` stops the SPU so the kernel can send the event it describes
  (port and 24 bits of data, plus a word from the outbound mailbox), and the result comes back in
  the inbound mailbox. SPU threads receive from SPU queues bound to them with `stop 0x110` (or
  `0x111`, which doesn't wait). PSL1GHT's `spu_printf` is built on these: a PPU thread receives
  each request and formats it from the SPU's local store. The protocols come from PSL1GHT's SPU
  runtime library. Not implemented yet: SPU thread group events and DMA events.
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
- Guest layout used today. UNVERIFIED: modeled on psdevwiki's description, but the exact boundaries
  may differ from real LV2.

| Range | Use |
|---|---|
| `0x0001'0000`… | The program's segments, at the addresses in its ELF |
| `0x2f00'0000` | HLE import thunks and descriptors (emulator-owned) |
| `0x2fff'0000` | Kernel stubs that threads return to |
| `0x3000'0000` | User memory (`sys_memory_allocate`), 256 MB |
| `0x4000'0000` | Memory mapper address ranges (`sys_mmapper`), e.g. PSL1GHT's heap |
| `0xc000'0000` | RSX local memory, 256 MB, mapped once a program initializes libgcm; its top 7 MB hold libgcm's control register, context and labels |
| `0xd000'0000` | Thread stacks and TLS, each stack with a guard page below it |

---

## 9. Graphics (RSX)

```
FIFO → command decoder → RSX state (all registers, inspectable) → GPU backend
```
- **FIFO** (`rsx::Rsx`): the PPU writes commands into IO-mapped main memory and moves PUT in
  the control register; the RSX reads from GET to PUT. Commands follow NVIDIA's NV4-style
  format (envytools): method writes (increasing or not), jumps, one level of call and return.
  The RSX takes a turn before each thread's turn and handles up to 10,000 commands, until it
  runs out or must wait (a semaphore, or a queued flip). When nothing else can run, the clock
  skips to the next vblank a flip waits for.
- **Memory:** 256 MB of local memory at `0xc0000000`; main memory reaches the RSX only through
  the IO map (1 MB pages). Methods name addresses by offset plus a DMA context: local
  (`0xfeed0000`) or main (`0xfeed0001`).
- **State model:** independent of the backend, and dumpable at any draw call. Semaphores, reports and
  flip/vblank events are handled here, not in the backend. It keeps the last value of every
  method on every subchannel, and turns the ones that act (clears, draws) into backend requests.
- **libgcm and video output** are HLE (`cellGcmSys`, `cellSysutil`). Initialization maps the
  IO buffer at IO offset 0 and puts the default command buffer 4 KB into it; when it fills up,
  the firmware's callback jumps back to the start once the RSX has caught up. Flips and
  wait-flips are written as two emulator-private methods (subchannel 7), since real libgcm's
  encoding is unknown to us. A vsync flip completes at the next vblank (60 Hz), an hsync flip
  at once.
- **Frames:** each completed flip produces a frame: the display buffer as 8-bit RGB. `ps3emu run
  --frames FILE` writes each frame's SHA-1, and `--dump-frames DIR` saves PNGs. Example
  programs can have a `.frames` file of expected hashes, checked like their `.expected` output.
- **Shaders:** RSX vertex and fragment programs are decoded into a shared IR, then emitted as Metal
  Shading Language (SPIR-V later). Cached by hash.
- **Surfaces:** a cache tracks render targets. A CPU write to a surface's memory invalidates it;
  reading it back triggers a write-back. Start conservative; optimize later.
- **Backends:** `Reference` (software, pixel by pixel into guest memory; exact and identical on
  every host, so its frames are the golden images, checked in CI), `Null` (renders nothing),
  `Metal` (first GPU backend, checked against the reference with a small per-pixel tolerance),
  `Vulkan` (later).

---

## 10. Verifying accuracy

There is **no PS3 hardware available yet**, so ground truth comes from:
1. **Architecture manuals** (Cell, SPU ISA, PowerPC). Instruction tests are written from their
   pseudocode.
2. **Independent implementations.** Our assembler and disassembler are checked against encodings
   from LLVM's PowerPC assembler for every opcode (the PS3 toolchain's assemblers for the
   Cell-only ones and the SPU). soft_float is checked against the host's IEEE arithmetic on millions of random
   operations, in every rounding mode.
3. **Native execution.** The example programs are C; their expected output comes from compiling
   the same C for the host and running it there.
4. **Real homebrew.** PSL1GHT programs, built with the actual PS3 toolchain, run unmodified. Their
   expected output is chosen so it doesn't depend on thread timing, or comes from the same C run
   natively (e.g. `printf` of doubles to hundreds of digits).
5. **IBM's Cell simulator** (systemsim), if a copy can be obtained.
6. **Black-box comparison with other emulators**: run the same test program and compare its output.
   We never read their source (see §11).

**Hardware harness (prepared now, used later):** a PSL1GHT program that runs instruction test cases
on a real PS3 and dumps the results in our test format. Once hardware is available, it validates
everything above.

**Test suites:**
- Instruction unit tests: inputs → expected outputs and flags, for every instruction.
- Example programs (`examples/`): small guest programs built with LLVM, whose output must match a
  `.expected` file exactly, and PSL1GHT programs (threads, floats, SPU threads with mailboxes,
  DMA, atomics, event queues and `spu_printf`). Later: a triangle and an audio tone, with framebuffer and audio hashes.
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
  common/      integer types, byte-order helpers, 128-bit vectors, SHA-1, soft_float (IEEE 754)
  core/        System, Scheduler, Memory, save states
  cpu/ppu/     instruction defs, decoder, interpreter, disassembler
  cpu/spu/     same, plus spu_float and the MFC
  lv2/         syscalls and kernel objects
  hle/         import linker, NIDs; one file per HLE library
  loader/      ELF, SELF, PRX, SFO
  crypto/      optional; off by default
  rsx/         FIFO, state, shader IR, surfaces, texture decoding
  gpu/         backends: reference (software), null, metal
  vfs/  audio/  input/  platform/
  debug/       tracing, GDB server, snapshots
  frontend/    CLI
examples/      guest programs (assembly, C, PSL1GHT) that run as integration tests
tests/         unit tests, mirroring src/; shared fixtures in tests/support/
tools/         helper scripts (e.g. building the PSL1GHT toolchain image); later, trace diff
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
| M2 ✅ | Kernel basics | Threads, locks, timers and the scheduler work, deterministically; PSL1GHT "hello world" runs. |
| M3 ✅ | PPU float + vector | FPU and VMX tests pass, including flags. |
| M4 ✅ | SPU | SPU instructions, channels, mailboxes, DMA and atomics pass tests. |
| M5 | First pixels | FIFO + state model; the reference backend draws a PSL1GHT triangle matching its golden hash, and Metal draws it within tolerance of the reference. |
| M6 | Debugging | GDB server, trace diff, save states, replay. |
| M7 | Shaders + textures | Homebrew 3D samples render correctly. |
| M8 | Firmware | Fake-signed SELF, LLE libraries from user firmware, `/dev_flash`. |
| M9 | Commercial | A simple commercial game reaches its first frame (research goal). |

---

## 15. Open questions

1. The time slice is 1,000 instructions, for SPU threads too. Code that polls another processor
   (rather than waiting on a channel) only sees progress at turn boundaries; finer interleaving
   may be needed for some programs.
2. Can a copy of IBM systemsim be obtained legally?
3. The initial register state LV2 gives a new process (beyond r1, r2, r13 and pc) is unverified.
4. Real LV2 thread scheduling: how priorities preempt, and the length of a time slice.
5. What the 0x30-byte header before each thread's TLS block contains.
6. Does the PS3's system software leave the VMX graphics rounding mode enabled (HID1[grap_mode])?
   We follow the hardware default (enabled). And what VSCR value do threads start with?
7. The Cell's exact estimate tables, and the undefined bits we fill in (e.g. the high word of
   `fctiw`'s result, FR after a disabled overflow). Real hardware will settle these.
8. A formal definition of `lvlx`/`lvrx`/`stvlx`/`stvrx`; ours follows the handbook's description.
9. SPU details the ISA leaves to the implementation: the `frest`/`frsqest` tables, whether the
   Cell's SPU flushes double-precision denormals and propagates NaNs, whether it has the
   optional v1.2 instructions, and which SPRs exist.
10. Real MFC timing: DMA latency and bandwidth under load (we use 256 cycles plus 8 bytes per
    cycle, one command at a time), and how quickly the immediate atomic commands complete.
11. LV2's SPU behavior where we had to choose: a full inbound mailbox makes
    `sys_spu_thread_write_spu_mb` return EBUSY; an exit with an empty outbound mailbox has
    status 0; a group whose threads all exit reports status 0; the priority range is 16-255; the
    decrementer reads 0 until first written; and the registers a new SPU thread starts with
    (beyond pc, `$1` and `$3`-`$6`).
12. LV2's event queue behavior where we had to choose: the source of events from an unnamed port
    (we use the port's ID); that SPU threads get only the low 32 bits of each data word; which
    errors SPU-side receives give (ESRCH for an unbound number, EBUSY for an empty queue on
    `tryreceive`, ECANCELED when the queue is destroyed); that only SPU queues can be bound and
    PPU threads can't receive from them; and that a connected port can't be destroyed.
13. The RSX and libgcm, where we had to choose: the local memory address (`0xc0000000`) and how
    much libgcm keeps for itself; the encoding of flips (ours are emulator-private methods);
    the end of the default command buffer; the scissor after initialization (we use the
    whole 4096×4096 range); what the X8R8G8B8 formats store in the X byte; the vblank rate
    (60 Hz, not 59.94); libgcm's and sysutil's error codes (we return EINVAL); the flip status
    before the first reset; and the RSX's speed (a fixed 10,000 commands per turn).

---

## 16. References

- *Power ISA* v3.1C (OpenPOWER Foundation): the floating-point and vector chapters, with
  pseudocode for every instruction. The classic instructions are unchanged since v2.02.
- *Cell Broadband Engine Programming Handbook* v1.11 (2008): PPE specifics, graphics rounding mode
- *Cell Broadband Engine Architecture* v1.01 (2006): SPU channels, the MFC and its commands
- *Synergistic Processor Unit Instruction Set Architecture* v1.2 (2007); *SPU Language Extensions*
- psdevwiki.com (PS3): syscalls, file formats, RSX
- PSL1GHT: SDK headers and samples, for building test programs; its `rsx/nv40.h` carries
  nouveau's NV40 method numbers, and its stub libraries the NIDs of libgcm and sysutil
- envytools: public NVIDIA hardware documentation for NV40/G70 commands, including the FIFO
  command format
