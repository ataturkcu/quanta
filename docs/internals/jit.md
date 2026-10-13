# Baseline JIT (experimental)

x86-64 only (Linux, macOS on Intel, Windows); anywhere else every chunk stays with the interpreter. Source: `src/core/vm/Jit.inc` (included by `Interpreter.cpp`, because it is built on the interpreter's `Frame` and handlers), `JitLayout.h`, `X64Asm.h`, `JitPlatform.h`.

## Shape of it

A template compiler: each bytecode instruction becomes a fixed run of machine code, nothing is optimised across instructions. The register file stays in `Frame::regs`; only the accumulator lives in a machine register (`r13`). The `Frame` is the whole state, so interpreter and generated code can hand a frame back and forth at any instruction boundary.

The rule that keeps it small and safe: the JIT never re-implements a semantic it is not sure about.

- **Translated** instructions handle only the case provable inline (two finite doubles, an Ordinary object whose shape matches a site cache, a dense Array in range, ...). Anything else goes to the slow path.
- **Everything else is stepped**: the interpreter's own handler runs that one instruction on the same `Frame`, then control returns to generated code. A stop byte (`h_JitHalt`) is placed where the instruction continues (the next one, a branch target, exception landing pads), so the handler chain ends there. Frames that cannot be left any other way than falling through run on a private copy of the instruction's bytes.
- **Fast helpers** (`fast_get_named`, `fast_set_named`, `fast_lookup`, `fast_env_slot`, `fast_set_keyed`, `call_op`, `binary_op`) are the interpreter's own fast paths as plain C++ functions, used before falling back to a step.

Property accesses call shared per-thread **stubs** (4-way polymorphic own/prototype ICs, global lookups, dense array reads/writes) instead of inlining the cache logic: inlining made the code large enough to thrash the L1 instruction cache.

## When code is compiled

- A function containing a loop: on its second call, or earlier by **on-stack replacement** — the interpreter counts back edges (`Frame::osr_budget`) and `osr_try` compiles and enters at the loop head.
- Loop-free functions stay interpreted by default (`QUANTA_JIT_MODE=0` compiles them after `QUANTA_JIT_THRESHOLD` calls). Measured: loops are where dispatch cost multiplies; short call/property-bound methods gain nothing.
- Chunks with suspension points (generators, async) are refused.
- A chunk is also refused when it cannot pay for itself, before or just after any code is mapped for it: bytecode over 16 KB (`QUANTA_JIT_MAXBYTES`) is not attempted at all, because decoding and emitting it needs scratch memory proportional to its size before there is a verdict, and one whose instructions are more than 10% plain steps into the interpreter (`QUANTA_JIT_MAXSTEPS`) runs slower compiled. A real-world bundle (3 MB of JS) showed both: it ran 5-8% slower and held 27 MB more with the JIT on until these limits went in; the 2x winners (Crypto, NavierStokes) sit at or under 3% steps.

## Memory safety

- Every register / constant / name / jump operand is range-checked when compiling; a chunk failing any check is refused.
- Generated code touches only `Frame`, the register bank, the chunk's constants, its own `JitCode`, and objects through the same checks the interpreter's inline-cache handlers make.
- C++ exceptions never unwind through generated code: helpers catch everything, store it, and the trampoline rethrows once the native frame is gone.
- The GC is conservative and non-moving, so values in generated code are found on the stack; the accumulator is spilled to the `Frame` before every call out. IC entries holding cells (prototypes, holders, cached values) are traced through `BytecodeChunk::trace`.
- Code pages are written once, then flipped to read+execute. Code only runs on the thread that compiled it.

## Platforms

Everything operating-system dependent is in `JitPlatform.h` (executable memory: `mmap`/`mprotect` on POSIX, `VirtualAlloc`/`VirtualProtect` on Windows; pages are written, then flipped to read+execute, never writable and executable at once). The other difference is the calling convention, which lives in the code generator (`namespace abi` in `Jit.inc`):

| | System V (Linux, macOS) | Microsoft x64 (Windows) |
|---|---|---|
| argument registers | rdi, rsi, rdx, rcx | rcx, rdx, r8, r9 |
| stack for a call | none | 32 bytes of shadow space at `rsp`, reserved once in the prologue |
| callee-saved | rbx, rbp, r12-r15 | those plus rdi, rsi |

Every helper takes at most four arguments, so none needs a stack argument (`step` used to take five; it now derives the instruction length from the opcode). Generated code uses xmm0-xmm3 only, which are volatile in both conventions, and never touches memory below `rsp`.

The Windows half cannot be run on a Linux machine, so it is exercised another way: building with `-DQUANTA_JIT_FORCE_WIN64=1` makes the code generator emit the Microsoft convention on any x86-64 target and declares every helper and the entry point with clang's `ms_abi` attribute, so the generated code and the C++ it calls really do speak Win64 to each other. The differential tests, the embed tests and the benchmarks pass in that build. What it cannot cover is the operating-system layer, which is small enough to read and which is type-checked for the Windows target against the SDK's signatures.

Two Windows facts worth knowing if the JIT ever fails to start there: a process that forbids dynamic code (the `ProhibitDynamicCode` mitigation) makes `VirtualAlloc`/`VirtualProtect` fail, and every function then simply stays interpreted; and a binary built with Control Flow Guard checks indirect calls, which would reject a call into a mapping the loader never registered, so the JIT expects a build without `/guard:cf`.

## Environment

| variable | meaning |
|---|---|
| `QUANTA_JIT=0` | disable |
| `QUANTA_JIT_MODE=0` | also compile loop-free hot functions |
| `QUANTA_JIT_THRESHOLD=n` | calls before compiling (default 6; loops: 2) |
| `QUANTA_JIT_LOG=1` / `QUANTA_JIT_STATS=1` | compile log / what is still stepped, per opcode and site |
| `QUANTA_JIT_MAXBYTES=n`, `QUANTA_JIT_MAXSTEPS=pct` | size limit (default 16384) and share of plain steps (default 10) above which a chunk is not compiled |
| `QUANTA_JIT_LIMIT=n`, `QUANTA_JIT_SKIP=n` | compile only functions number [skip, skip+limit): bisecting |
| `QUANTA_GC_STRESS` | also makes the generated code poll the GC at every back edge |

Verified by running the whole test262 `language` and `built-ins` suites with every function forced through the JIT (`QUANTA_JIT_THRESHOLD=1 QUANTA_JIT_MODE=0`, also with `QUANTA_GC_STRESS=2` on a subset): results identical to the interpreter's.
