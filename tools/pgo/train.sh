#!/bin/bash
# Instrumented build -> train -> profile-guided build. See README.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
raw=$(mktemp -d)
trap 'rm -rf "$raw"' EXIT

command -v llvm-profdata >/dev/null || { echo "llvm-profdata not found" >&2; exit 1; }

echo "== instrumented build"
rm -rf build
QUANTA_EXTRA_CXXFLAGS="-fprofile-generate=$raw/%p.profraw -DQUANTA_PGO_INSTRUMENT" ./build.sh
instrumented=build/bin/quanta

echo "== training"
export LLVM_PROFILE_FILE="$raw/%p.profraw"
for w in tools/perf/micro/*.js "$@"; do
    timeout 600 "$instrumented" "$w" >/dev/null 2>&1 || true
done
llvm-profdata merge -o "$raw/quanta.profdata" "$raw"/*.profraw

echo "== profile-guided build"
rm -rf build
QUANTA_EXTRA_CXXFLAGS="-fprofile-use=$raw/quanta.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" ./build.sh
