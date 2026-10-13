#!/bin/bash
# Deterministic instruction counts for a set of scripts, JIT off.
#
#   tools/perf/count.sh [-b path/to/quanta] script.js [script.js ...]
#
# Instruction counts do not move with machine load the way timings do, so a change of a
# fraction of a percent is visible; build with QUANTA_ARCH_FLAGS="-march=nehalem -mtune=nehalem"
# to keep the numbers comparable between machines. Prints one count per script and the total.
bin=build/bin/quanta
if [ "$1" = "-b" ]; then bin=$2; shift 2; fi
out=$(mktemp)
trap 'rm -f "$out"' EXIT
total=0
for script in "$@"; do
  n=$(QUANTA_JIT=0 valgrind --tool=callgrind --callgrind-out-file="$out" "$bin" "$script" 2>&1 >/dev/null \
      | grep Collected | sed 's/.*: //')
  printf "%-40s %s\n" "$script" "$n"
  total=$((total + n))
done
printf "%-40s %s\n" total "$total"
