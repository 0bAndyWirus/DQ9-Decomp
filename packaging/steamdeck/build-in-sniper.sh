#!/usr/bin/env bash
# Runs INSIDE the Steam Runtime 3 "sniper" SDK container (see
# tools/build_deck.sh): builds dq9 against the runtime's glibc 2.31 and SDL2,
# assembles the Deck bundle and checks its ABI requirements.
#
# Expects: repository at /src; a separately supplied generated-code directory at /gen.
# Environment: DQ9_DECK_JOBS (parallel jobs, default: ninja's choice).
set -euo pipefail

src=/src
gen=/gen
build="$src/build/deck-build"
bundle="$src/build/deck/dq9-deck"
max_glibc=2.31

# Newest GCC shipped by the SDK (sniper: gcc-10 default, gcc-14 alternative).
# Its libstdc++ is newer than the runtime's (GLIBCXX_3.4.28, from gcc 10),
# so libstdc++/libgcc are linked statically below.
gxx=$(ls /usr/bin/g++-[0-9]* | sort -V | tail -n 1)
gcc="${gxx/g++/gcc}"
echo "== compiler: $("$gxx" --version | head -n 1)"
echo "== SDL2: $(pkg-config --modversion sdl2), $(ldd --version | head -n 1)"

cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$gcc" -DCMAKE_CXX_COMPILER="$gxx" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DDQ9_GENERATED_DIR="$gen"
cmake --build "$build" --target dq9 ${DQ9_DECK_JOBS:+-j "$DQ9_DECK_JOBS"}

rm -rf "$bundle"
mkdir -p "$bundle/bin"
install -m 755 -s "$build/game/dq9" "$bundle/bin/dq9"
install -m 755 "$src/packaging/steamdeck/dq9.sh" "$bundle/dq9.sh"
install -m 644 "$src/packaging/steamdeck/dq9.desktop" "$bundle/dq9.desktop"
install -m 644 "$src/packaging/steamdeck/README.md" "$bundle/README.md"

echo "== NEEDED"
objdump -p "$bundle/bin/dq9" | awk '/NEEDED/ {print "  " $2}'
glibc=$(objdump -T "$bundle/bin/dq9" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
echo "== max GLIBC symbol version: $glibc (limit $max_glibc)"
if objdump -T "$bundle/bin/dq9" | grep -q 'GLIBCXX_\|CXXABI_'; then
    echo "error: bin/dq9 still references libstdc++ symbol versions" >&2
    exit 1
fi
if [ "$(printf '%s\n%s\n' "$glibc" "$max_glibc" | sort -V | tail -n 1)" != "$max_glibc" ]; then
    echo "error: bin/dq9 needs GLIBC_$glibc, newer than sniper's $max_glibc" >&2
    exit 1
fi
echo "== bundle: $bundle"
