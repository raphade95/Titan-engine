#!/bin/bash
# Mutation test for the golden harness.
#
# A regression test that cannot detect the bug it was written for is worse
# than no test: it manufactures confidence. This script reintroduces each
# determinism defect the harness exists to catch and asserts that
# titan_golden FAILS. If a mutation survives, the harness has a blind spot.
#
# Run it whenever tests/test_golden.cpp or the tolerances change:
#     cpp/tools/mutation_test.sh
set -uo pipefail
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SRC="libTitanCore/src/TitanNoise.cpp libTitanCore/src/TerrainEngine.cpp
     libTitanCore/src/Erosion.cpp libTitanCore/src/Fluvial.cpp
     libTitanCore/src/Layers.cpp libTitanCore/src/Filters.cpp
     libTitanCore/src/Export.cpp libTitanCore/src/CAPI.cpp"
FLAGS="-std=c++20 -O2 -fno-fast-math -ffp-contract=off"

pass=0
fail=0

# run_mutant <kill|survive> <name> <rationale>
#
# "kill"    — this mutation is a real defect; the harness MUST catch it.
# "survive" — this mutation is provably output-neutral. It is run anyway, as a
#             standing assertion that the property still holds: if it ever
#             starts getting killed, someone has made the algorithm sensitive
#             to an ordering that used to be irrelevant, and that is worth
#             finding out about deliberately rather than by accident.
run_mutant() {
    local expect="$1" name="$2" desc="$3"
    if ! clang++ $FLAGS -I "$WORK/src/libTitanCore/include" \
         $(cd "$WORK/src" && echo $SRC | tr ' ' '\n' | grep -v '^$' | sed "s|^|$WORK/src/|") \
         "$WORK/src/tests/test_golden.cpp" -o "$WORK/mutant" 2>"$WORK/build.log"; then
        echo "  ERROR     $name — mutant did not compile"
        sed 's/^/            /' "$WORK/build.log" | head -5
        fail=$((fail + 1))
        return
    fi

    local killed=0
    "$WORK/mutant" >"$WORK/run.log" 2>&1 || killed=1
    local caught
    caught=$(grep -c '^  FAIL' "$WORK/run.log" || true)

    if [ "$expect" = "kill" ]; then
        if [ "$killed" = 1 ]; then
            echo "  KILLED    $name — $caught check(s) failed, as intended"
            pass=$((pass + 1))
        else
            echo "  SURVIVED  $name — golden checks still passed!"
            echo "            $desc went undetected: the harness has a blind spot"
            fail=$((fail + 1))
        fi
    else
        if [ "$killed" = 0 ]; then
            echo "  NEUTRAL   $name — no effect, as expected"
            echo "            $desc"
            pass=$((pass + 1))
        else
            echo "  CHANGED   $name — this used to be output-neutral and no longer is"
            echo "            $desc"
            fail=$((fail + 1))
        fi
    fi
}

reset_src() {
    rm -rf "$WORK/src"
    mkdir -p "$WORK/src"
    cp -R libTitanCore tests "$WORK/src/"
}

echo "=== mutation test: can titan_golden actually catch these? ==="

# --- M1: RNG seeding order -------------------------------------------------
# Models a compiler that initialises function parameters right-to-left, which
# is what the original `Pcg32 rng(SplitMix64(s), SplitMix64(s))` allowed.
reset_src
python3 - "$WORK/src/libTitanCore/include/TitanRandom.h" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
assert "return Pcg32(s0, s1);" in s
open(p, "w").write(s.replace("return Pcg32(s0, s1);", "return Pcg32(s1, s0);"))
PY
run_mutant kill "M1 swapped SplitMix64 draw order" \
    "the argument-evaluation-order bug"

# --- M2: fluvial sort tie-break -------------------------------------------
# Models a standard library whose unstable sort permutes equal keys the other
# way. Dropping the tie-break entirely would not change results on *this*
# libc++, which is exactly why the bug was invisible; reversing it reproduces
# what a different STL would do.
reset_src
python3 - "$WORK/src/libTitanCore/src/Fluvial.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "return filled[a] != filled[b] ? filled[a] > filled[b] : a < b;"
assert old in s
open(p, "w").write(s.replace(old, "return filled[a] != filled[b] ? filled[a] > filled[b] : a > b;"))
PY
run_mutant kill "M2 reversed fluvial sort tie-break" \
    "STL-dependent ordering of equal-height cells"

# --- M3: priority-flood heap tie-break -------------------------------------
reset_src
python3 - "$WORK/src/libTitanCore/src/Fluvial.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "return h != o.h ? h > o.h : idx > o.idx;"
assert old in s
open(p, "w").write(s.replace(old, "return h != o.h ? h > o.h : idx < o.idx;"))
PY
run_mutant survive "M3 reversed fluvial heap tie-break" \
    "priority-flood propagates max(height[n], popped.h + eps); tied cells have
            equal h, so pop order cannot change any filled value. The tie-break
            is defensive, not corrective."

# --- M4: water heap tie-break ---------------------------------------------
reset_src
python3 - "$WORK/src/libTitanCore/src/Layers.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "return h != o.h ? h > o.h : idx > o.idx;"
assert old in s
open(p, "w").write(s.replace(old, "return h != o.h ? h > o.h : idx < o.idx;"))
PY
run_mutant survive "M4 reversed water heap tie-break" \
    "same invariant as M3, applied to the lake fill."

# --- M5: seed hash convention ---------------------------------------------
# Models the old per-host hashes that truncated multi-byte UTF-8.
reset_src
python3 - "$WORK/src/libTitanCore/include/TitanRandom.h" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "h ^= static_cast<uint32_t>(*p);"
assert old in s
open(p, "w").write(s.replace(old, "h ^= static_cast<uint32_t>(*p) & 0x7Fu;"))
PY
run_mutant kill "M5 truncated seed-hash bytes" \
    "the divergent per-host seed hashes"

echo
if [ "$fail" -eq 0 ]; then
    echo "ALL $pass MUTANTS BEHAVED AS SPECIFIED — the golden harness has teeth"
    exit 0
fi
echo "$fail MUTANT(S) MISBEHAVED out of $((pass + fail)) — fix the harness"
exit 1
