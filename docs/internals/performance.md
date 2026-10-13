# Instruction-count performance

The interpreter's speed on call- and property-heavy code is decided by how many machine instructions a bytecode costs, so that is what this page tracks. Wall-clock time on a shared machine moves by ten percent between runs; instruction counts do not move at all, which makes a change of a fraction of a percent visible and lets a regression be bisected.

## Measuring

`valgrind --tool=callgrind` counts every instruction. Two things make it usable here:

* **Build for a CPU valgrind can emulate.** The default build targets the host and uses instructions valgrind does not know; the binary then dies at start-up with a count of a couple of million. `QUANTA_ARCH_FLAGS="-march=nehalem -mtune=nehalem" ./build.sh` produces one that runs, and counts that stay comparable between machines.
* **Run with the JIT off** (`QUANTA_JIT=0`): the question here is what the interpreter costs.

`tools/perf/` has the pieces:

| | |
|---|---|
| `count.sh -b <quanta> a.js b.js ...` | instruction count per script and their sum |
| `instrprof.py`, `annot.py` | per-instruction execution counts of one function (`--dump-instr=yes`), joined with its disassembly. When a function runs once per loop iteration, the number of executed lines *is* its cost per call |
| `micro/*.js` | one-feature loops (call, construct, object literal, property read, closure call) each paired with a `_baseline` that has the same loop without the feature; the difference divided by the iteration count is the feature's cost |

The loop is: write the micro-benchmark for the thing, annotate the function that runs it, delete instructions, re-count.

## Where the instructions go

A large real-world program (a 3 MB minified player script, 482 million bytecodes, ~37 G instructions after this work, ~77 per bytecode) spends them roughly like this:

| | share |
|---|---|
| call path (`CallResolved`/`Call` → `call_fast_gate` → `run_register_call`, env-mode `call_default_impl`/`run`, natives) | 22% |
| property access (`GetNamed*`, `SetNamed*`, `DefineOwn*`, object creation) | 22% |
| simple bytecodes (`Ldar`, `Star`, jumps, arithmetic, compares) | 13% |
| garbage collection | 12% |
| environment/closure variable access and env-mode frames | 9% |
| parsing/compilation | 5% |

It is a flat profile. The biggest single function is `run_register_call` at about 5.6%, and the two `GetNamedFast` instantiations together at about 9%; nothing above them is bigger than a few percent, which is why the wins below are individually small.

## What changed, and why

Measured with the benchmark suite in `count.sh` (Richards, DeltaBlue, RayTrace, Earley-Boyer, a DOM workload) unless noted; percentages are against the previous step.

**Value tag tests compare the top sixteen bits after a shift** (`Value::has_tag_`) instead of masking with a 64-bit constant. A mask needs its constant in a register and handlers test two or three types each, so it was an instruction or two per test. Safe because no constructor produces a value with both the sign bit and an all-ones exponent (infinities and NaNs are re-tagged). `is_boolean`/`is_nullish` are one compare, since the two tags are adjacent. −1.0%, then −0.4% for the two adjacent-tag forms.

**`call_gated` writes nothing to the caller's `Context` for a plain call.** The per-call identity fields (`strict`, `arrow`, `this_value`, `new_target`, `this_needs_super`) live in packed bit-fields, where each save and each restore is a load, a mask and a store, and an ordinary call changes none of them. The short form takes only calls that provably change nothing; the general form is unchanged. A body that reads `this` through the frame (`LdaThis`) no longer forces the `this_value` write (only super access does: `Chunk::uses_super`). −1.9% and −1.0% from inlining the short form into `call_fast_gate`.

**The `Context` flags the call path touches are whole bytes** (`has_exception_`, the constructor flags, `strict_mode_`, ...). A bit-field store is a read-modify-write; a byte is one instruction, and the exception test after every call becomes one compare. Costs eight bytes per `Context`. −1.3%.

**`Call`/`CallResolved` handlers keep only the frame and pc across the call.** The generated handler held the whole decoded instruction in callee-saved registers (six pushes) and tested for exceptions twice. The lean form decodes after the call returns, searches the handler table out of line (`h_ExceptionAt`) and tail-calls the generated handler for anything that is not a plain compiled callee. −0.7%.

**Comparison + conditional jump.** A comparison whose next instruction is `JumpIfTrue/False` takes the branch itself. The boolean stays in the accumulator, so a path that reads it (`a < b && c`) is unaffected, and the jump remains its own instruction for anything that lands on it. Small (−0.2% with the next item) because the dispatch it saves is cheap; it matters more for the JIT-less loop conditions.

**`LdaTdzStar`.** A function that declares `let`/`const` seeds each register's TDZ with `LdaTdz; Star`; fused, that is one dispatch instead of two.

**Single-pass binding creation** (`SlotMap::create_interned_if_absent`). Seeding a call environment scanned its entries three times per name (find, find again inside `get_or_create`, find a free slot).

**`new F()` fast path** (`Function::construct_plain`) for a base constructor with no `new.target` chain: no prototype-through-new.target resolution, no auto-super, no swapped results. −8% on a construct loop. `ConstructFlagsGuard` is shared with the general path.

**Property-write helpers without calls.** A `SetNamed` that adds a property skips the write-barrier call for numbers and booleans and no longer calls `Object::is_extensible()` out of line; `add_shape_property_cached` is inline (the butterfly only grows out of line). −7.6% on an object-literal loop, −1.7% on a construct loop.

**Feedback slots are never null.** An untouched slot points at the shared empty body, so the property handlers load and test it with no null select; `shape_fast_path_ok` is one bit probe against a mask instead of five compares. −1.4%.

**`Value(Object*)` is inline** (it is a null test and the object's type bits; it sat behind a call at every allocation and every property read that returned an object).

**Smaller things:** register-bank initialisation, the argument copy (a plain loop; left alone the compiler turns it into a `memcpy` call that costs more than the copy for one to three arguments), frameless `Jump`/`JumpIf`/bitwise handlers and `read_*` helpers that compile to single loads, `h_BackEdgeDue` as the only place a loop back-edge reaches the collector or the JIT.

Taken together: the suite is −8.5% against the start of this round, and the real-world script went from 41.5 G to ~36.5 G instructions (−12%).

## Tried and not kept

Recorded so the next attempt starts from the result, not the idea.

* **A memo of the first inline-cache entry in the `FeedbackSlot`** (raw shape word + slot index, so a hit is one compare). Saves about ten instructions per own-property hit but the suite moved by 0.4%; not worth a second copy of cache state to invalidate.
* **A leaner `call_gated` that only skipped checks** without removing the Context traffic: same instruction count, the compiler turned the conditionals into flag juggling.
* **GC growth-factor knob, call-share gate:** no measurable change in instructions.
* **Stackless interpreter calls** (a call handler that pushes a frame on a side stack and continues dispatch, `Return` popping it). Estimated before building: the plain-callee calls that would qualify (loop-free functions, which the JIT declines) are ~60% of calls, and the saving is ~130 of ~300 instructions per call, so ~3.5% overall for a change that touches exception unwinding, the GC's view of registers, OSR and the JIT's stepping. Not done; if the call path is revisited this is the shape of the answer.

## Where the rest is

* **The call path is still ~300 instructions** (lean handler ~60, `call_fast_gate` with `call_gated` ~110, `run_register_call` ~140). The frame itself is ~40 stores; most of what is left is real state.
* **Closure variable access** walks the environment chain by name (`SlotMap::find_interned`, ~50 instructions per level). `LdaEnvSlotAt hops,slot` exists for statically resolvable chains; extending it to every captured variable (V8's context slots) is the larger, more invasive fix, and env-mode calls (a function with inner closures) are ~9% of the real-world profile.
* **GC:** 125 minors at ~2.5 ms and 12 majors at ~35 ms. The minors are dominated by fixed per-collection costs (stack scan, root set, rebuild) rather than by marking, so a larger nursery is the lever; the earlier attempt at a growth knob did not move instruction counts.
* **One in four dispatches is a register shuffle** (`Ldar`, `Star`, `LdarStar`). Better register allocation in the bytecode compiler would remove them; each is only ~10 instructions, so it is worth ~3% at most.
* **Dispatch itself:** `lea kHandlers(%rip)` is paid by every handler under PIE. A non-PIE build or a pinned table register would save one instruction per bytecode (~1%); not taken because the static library is meant to link into position-independent executables.
