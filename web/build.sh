#!/bin/bash
# Builds the phone player: tisple machine as WebAssembly, in web/dist with the page.
# Needs Emscripten: source emsdk_env.sh first, or set EMSDK to its folder.
#   web/build.sh            build into web/dist
set -e
cd "$(dirname "$0")/.."
if ! command -v em++ > /dev/null; then
    source "${EMSDK:-/media/vega/mdot2b/Tools/emsdk}/emsdk_env.sh" > /dev/null 2>&1
fi
mkdir -p web/dist web/obj

SOURCES=(
    web/player.cpp
    src/emu/machine.cpp
    src/emu/chips_impl.cpp
    src/machine/apple2.cpp
    src/machine/ay8910.cpp
    src/machine/bus.cpp
    src/machine/disk2.cpp
    src/machine/disk2_rom.cpp
    src/machine/font.cpp
    src/machine/memory_map.cpp
    src/machine/mockingboard.cpp
    src/machine/mockingboard_audio.cpp
    src/machine/mouse.cpp
    src/machine/mouse_rom.cpp
    src/machine/slots.cpp
    src/machine/speaker_audio.cpp
    src/machine/via6522.cpp
    src/machine/video_render.cpp
    src/machine/video_scan.cpp
    src/disk/image.cpp
    src/disk/machine_disks.cpp
    src/disk/nibble.cpp
    src/disk/dos33.cpp
    src/asm/asm.cpp
    src/asm/opcodes.cpp
    src/emu/disasm.cpp
    src/asm/wait.cpp
    src/disk/loader_read.cpp
    src/disk/boot_loader.cpp
)
FLAGS=(-std=c++20 -O2 -Isrc -Ithird_party/chips -DNDEBUG)

# one object per source, rebuilt when the source is newer
OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="web/obj/$(echo "$src" | tr '/' '_').o"
    OBJECTS+=("$obj")
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        extra=()
        [ "$src" = src/emu/chips_impl.cpp ] && extra=(-w)
        em++ "${FLAGS[@]}" "${extra[@]}" -c "$src" -o "$obj"
    fi
done

em++ "${OBJECTS[@]}" -O2 -o web/dist/machine.js \
    -sMODULARIZE=1 -sEXPORT_NAME=TispleMachine -sENVIRONMENT=web \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
    -sEXPORTED_FUNCTIONS=_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16
cp web/index.html web/player.js web/manifest.json web/icon.svg web/dist/
echo "web/dist: $(du -sh web/dist | cut -f1)"
