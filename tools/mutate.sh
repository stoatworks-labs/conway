#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run.
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
MUTANTS=(
	"source/Shaders.cpp|next = ( ( Survive >> n ) & 1u ) != 0u;|next = ( ( Survive >> n ) ^ 1u ) != 0u;|--patterns|GLSL: the survival test inverted, & -> ^"
	"source/Shaders.cpp|c.x += Grid.x - WrapSkewForTest;|c.x -= Grid.x - WrapSkewForTest;|--reference|GLSL: the torus wraps the wrong way in x, += -> -="
	"source/Shaders.cpp|vec2 b = min( hi, vec2( x + 1, y + 1 ) - inset );|vec2 b = min( hi, vec2( x + 1, y + 1 ) + inset );|--coverage|GLSL: the lit square grows past its cell on one side, - -> +"
	"source/Clock.h|owed -= whole;|owed += whole;|--clock-law|C++: the clock banks the generations it ran, -= -> +="
	"source/Settle.h|for( int p = 1; p <= maxPeriod; ++p )|for( int p = 2; p <= maxPeriod; ++p )|--settle-law|C++: a constant population is never settled, 1 -> 2"
)

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target cwtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	if "$tree/build/cwtest" "$check" >"$WORK/log" 2>&1; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant:\n' "$check"
		grep -E '^  FAIL' "$WORK/log" | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
