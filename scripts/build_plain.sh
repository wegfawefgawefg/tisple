#!/bin/sh
# Builds an optimised tisple without CMake or Ninja, for a machine that has only a C++20
# compiler (the random-program race's long runs, races/README.md): every src/ file but
# the SDL window, and the fuzz generator, into $BUILD/tisple; objects go to $BUILD/plain/.
#   scripts/build_plain.sh [JOBS]        CXX=c++ by default, BUILD=build
set -eu
cd "$(dirname "$0")/.."
JOBS="${1:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
CXX="${CXX:-c++}"
BUILD="${BUILD:-build}"
OUT="$BUILD/plain"
mkdir -p "$OUT"
FLAGS="-std=c++20 -O2 -DNDEBUG -Isrc -Ithird_party/chips -Itests -DTISPLE_LIB_DIR=\"$PWD/lib\""
export CXX FLAGS OUT
# one object per source, named after its path; the vendored chips file without warnings
{
    find src -name '*.cpp' ! -path 'src/machine/sdl_*'
    ls tests/fuzz_gen*.cpp
} | xargs -P "$JOBS" -I {} sh -c '
    obj="$OUT/$(echo "{}" | tr / _).o"
    if [ ! -f "$obj" ] || [ "{}" -nt "$obj" ]; then
        extra=""
        case "{}" in src/emu/chips_impl.cpp) extra="-w" ;; esac
        $CXX $FLAGS $extra -c "{}" -o "$obj"
    fi'
# an object is rebuilt when its .cpp is newer; after a header change delete $BUILD/plain
$CXX -o "$BUILD/tisple" "$OUT"/*.o
echo "built $BUILD/tisple"
