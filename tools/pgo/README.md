# Profile-guided build (optional)

`./build.sh` builds with -O3 and no profile. A profile-guided build is a few percent faster on
the interpreter-bound benchmarks (measured here: Richards, Earley-Boyer, RegExp +5..10%, a large
real-world script about -4% time) and neutral elsewhere. The profile belongs to one source
revision, so it is generated locally rather than checked in.

    tools/pgo/train.sh [workload.js ...]

builds an instrumented binary in `build-pgo/`, runs the workloads (by default the scripts in
`tools/perf/micro` plus whatever you pass, e.g. the V8 benchmark suite and a large real script),
merges the counters and rebuilds `build/bin/quanta` with `-fprofile-use`. It needs
`llvm-profdata` and clang's profile runtime (`libclang-rt-<N>-dev` on Debian/Ubuntu).
Use representative workloads: a profile of toy loops optimises toy loops.
