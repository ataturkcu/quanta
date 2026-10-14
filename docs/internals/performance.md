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

A large real-world program (a 3 MB minified player script, 482 million bytecodes, ~35 G instructions after this work, ~77 per bytecode) spends them roughly like this:

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

**Natives go through the lean call handlers too.** `Call`/`CallResolved` take any function callee, compiled or native; the generated handlers they replaced paid six saved registers and two exception tests around every `charCodeAt`/`push`/`indexOf`. ~48 instructions per native call.

**A captured parameter's environment slot is predicted where it is created.** In a function whose other names live in registers, the entry bytecode copies a captured parameter into the environment *after* every local; the compiler predicted the slot *before* them, so each guarded `LdaEnvSlot` of it missed and walked the scope chain by name. A closure-call loop spent 14% on it.

**`==`/`!=` are settled by the values' bits** for nullish, object and boolean pairs (`loose_equal_quick`, mirroring `apply_operator`'s EQUAL/NOT_EQUAL cases) instead of falling to the tree-walker's `apply_operator` for everything but two numbers -- including `x != null`, most of what loose equality is used for. Richards −4.5%.

**Megamorphic sites got two fixes and a new cache.** A constructor shared by many "classes" (`Class.create()`'s `function() { this.initialize.apply(this, arguments); }`, the Prototype.js idiom, still common in old libraries) sees the same shapes under many prototypes.
* The thread-wide read cache indexed its entries by (shape, name) only, so twelve prototypes evicted each other; and an entry whose epoch had gone stale was never refreshed within a sweep generation (the "already decided" early return ignored the epoch), so after the first prototype change a site missed on every read until the next collection. Entries are now keyed on the prototype and relearn when the chain has changed.
* The per-site store-transition cache replaced an entry for the same shape whatever its prototype, so the same twelve prototypes thrashed it; it now keeps one per (shape, prototype) up to its capacity.
* Past that capacity, a site now uses a thread-wide transition cache (`MegaTransition`) keyed on (shape, name, prototype), valid for one `proto_epoch` and one sweep generation like the read cache, and only for an Ordinary, extensible receiver with no descriptor map. It records a transition only when the new shape is the old one plus exactly that property -- a store that ran an inherited setter also moves the object, to a shape holding whatever the setter's body added, and replaying that for a plain store would skip the setter (caught by the embed regression test).
RayTrace −20% from these; a 12-class construct loop −73%.

**`F.prototype` read off a function is the field** (`Function::has_plain_prototype`), skipping the name checks and descriptor walk of the general path. RayTrace −5%.

**`new F()` through its own lean handler** (`h_ConstructLean`) and a `Function::construct` that is only the choice of road (the general body moved out so a plain `new` does not pay its frame).

**Stackless interpreter calls.** `Call`/`CallResolved` of a plain compiled callee (no `this`/strictness switch, no env-mode, at most 32 registers, a chunk the JIT has refused) build the callee's `Frame` on a side arena and continue dispatching in it; `Return` pops it and dispatches the caller's next instruction, so the call costs no C++ frame, no `run_with_regs` set-up and no second dispatch loop. The pieces that make it safe: the arena's values are a GC root (`Collector::set_value_stack`), an exception unwinds stackless frames by searching each one's handlers from the call's pc, a C++ exception from a native lands in `run_with_regs`, which finds the innermost frame of its activation, and a frame the baseline JIT is stepping never starts one. Calls into another realm stay on the old path. Eligibility is decided once per chunk (`BytecodeChunk::inline_state`). −2.9% instructions on the suite (Richards −4%, DeltaBlue −5.6%, Earley −3.1%); `QUANTA_INLINE_CALLS=0` turns it off.

**Stackless calls, measured on a real program.** The suite gained 3% and the large real-world script nothing: a third of its calls are refused (closure-heavy functions with environments of their own, natives), and the refused ones pay the screen. What made the difference was moving the eligibility test to one per-chunk verdict (`BytecodeChunk::inline_state`), splitting the lean handlers into a cheap screen (`h_CallInline`) and the entry itself (`h_CallInlineGo`, six saved registers that a refused call should not pay), and keeping the return (`h_ReturnInline`) free of the collector's safepoint call.

**Property cache entries hold the receiver's whole shape word.** `FeedbackBody::Entry::word` is the shape pointer plus the object-type bits exactly as `Object` stores them, so `entries[0].word == obj->raw_shape_word()` answers "same shape, and a type the own-slot path serves" in one compare; an accessor, or a shape learned from an excluded type, is stored with a bit no receiver's word has and is found by the general handler's scan (`Entry::shape()`). The capacity test on the slot is gone: a shape that has the slot has it in the butterfly, the same fact the collector's `trace_fast` already walks unchecked. -17 instructions per cached read (49 -> 32 for `s += o.x`), -3.6% on the real-world script.

**Environments.** A call environment is seeded by index (`Environment::fresh_with_room` / `seed_binding`) when its names are distinct, instead of one lookup-and-insert per name; the same for loop and block scopes, whose "advance" no longer allocates a vector per iteration. `InlineEntry` leaves free slots unconstructed (the constructor wrote six empty ones into every environment), the environment and context pools are intrusive free lists instead of vectors behind a thread-local guard, and `EnvironmentRoot` is a node chained through its owner's stack frame (two stores to register, one to drop) instead of a push/pop on a thread-local vector; fibers' stacks are purged from the chain before they are freed. A closure-creating function's call: -5%; the real-world script: -2%.

**Collector.** Leaf strings (`CellKind::FlatString`, everything but ropes and slices) are marked from the block header and never traced; the mark loop walks `Object::trace_default`'s body through `Object::trace_fast` with inlined edge visits and an inlined `mark_exact`/`clear_remembered`; `String::make_concat` copies up to 128 bytes instead of building a 2-3 cell rope; `write_barrier` returns inline for a container nothing has marked. Splay: major-collection time -33%, instructions -16%, score +20-25%, memory unchanged. What did not help: a larger nursery (+16% for +60% memory), the major-collection trigger keyed on promoted growth (13 -> 10 majors, no score change), a larger prefetch ring. Spacing the majors out does help Splay a lot (+85% at 3.4x the memory) -- the cost is the memory, not the algorithm.

**Compiler.** An object operand is read in place when nothing between its read and its use can assign it (`BytecodeCompiler::borrow_object_register`, `borrow_tape_object_register`): a method call's receiver, the object of a member store, `this.x`, the left operand of a binary expression. One dispatch fewer in about one bytecode in twenty.

**Profile-guided build.** Optional (`tools/pgo/`): +5-10% on Richards, Earley-Boyer and RegExp, about -4% time on the real-world script. The profile belongs to one revision, so it is made locally.

**Smaller things:** register-bank initialisation, the argument copy (a plain loop; left alone the compiler turns it into a `memcpy` call that costs more than the copy for one to three arguments), frameless `Jump`/`JumpIf`/bitwise handlers and `read_*` helpers that compile to single loads, `h_BackEdgeDue` as the only place a loop back-edge reaches the collector or the JIT.

Taken together: the benchmark suite is −13% against the start of this round (RayTrace −22%, Richards −14%, DeltaBlue −12%, Earley-Boyer −10%), and the real-world script went from 41.5 G to 34.8 G instructions (−16%; ~10% less wall-clock time on a quiet machine, RSS +1%). On the V8 benchmark scores the same build is ahead of the one it started from by Richards +22%, DeltaBlue +23%, RayTrace +50%, Earley-Boyer +9%; Splay and RegExp did not move (garbage collection and the regular-expression engine, not the interpreter).

## Tried and not kept

Recorded so the next attempt starts from the result, not the idea.

* **A memo of the first inline-cache entry in the `FeedbackSlot`** (raw shape word + slot index, so a hit is one compare). Saves about ten instructions per own-property hit but the suite moved by 0.4%; not worth a second copy of cache state to invalidate.
* **A leaner `call_gated` that only skipped checks** without removing the Context traffic: same instruction count, the compiler turned the conditionals into flag juggling.
* **GC growth-factor knob, call-share gate:** no measurable change in instructions.

* **Whole-function baseline JIT on the real-world script** (`QUANTA_JIT_MODE=0`): 5% slower than the interpreter. Its calls and property accesses go through the same helpers, so compiling the dispatch away gains less than the compile costs. A tier worth having would first need JIT-to-JIT calls and inline caches in the generated code.
* **Making an env-mode function register-only when its closures capture only outer names:** of 99 env-mode functions in the real-world script, 9 would qualify. The others capture their own locals.
* **Raising the major-collection growth factor / clock** (Splay +85%, memory x3.4) and **aging in the nursery** (Splay's garbage lives ~4 nurseries; no aging depth helps).

## Where the rest is

* **The call path is still ~300 instructions** (lean handler ~60, `call_fast_gate` with `call_gated` ~110, `run_register_call` ~140). The frame itself is ~40 stores; most of what is left is real state.
* **Closure variable access** walks the environment chain by name (`SlotMap::find_interned`, ~50 instructions per level). `LdaEnvSlotAt hops,slot` exists for statically resolvable chains; extending it to every captured variable (V8's context slots) is the larger, more invasive fix, and env-mode calls (a function with inner closures) are ~9% of the real-world profile.
* **GC:** 125 minors at ~2.5 ms and 12 majors at ~35 ms. The minors are dominated by fixed per-collection costs (stack scan, root set, rebuild) rather than by marking, so a larger nursery is the lever; the earlier attempt at a growth knob did not move instruction counts.
* **One in four dispatches is a register shuffle** (`Ldar`, `Star`, `LdarStar`). Better register allocation in the bytecode compiler would remove them; each is only ~10 instructions, so it is worth ~3% at most.
* **Dispatch itself:** `lea kHandlers(%rip)` is paid by every handler under PIE. A non-PIE build or a pinned table register would save one instruction per bytecode (~1%); not taken because the static library is meant to link into position-independent executables.
