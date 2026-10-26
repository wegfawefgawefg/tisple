#!/bin/sh
# Installs the three race competitors under ~/.local/share/tisple/compilers (no root).
# Versions are pinned; races/README.md records them. Rerunning skips what is there.
#   cc65     V2.19 built from source
#   llvm-mos llvm-mos-sdk v23.4.0, the prebuilt Linux release
#   oscar64  v1.32.273 built from source
set -eu

PREFIX="${TISPLE_COMPILERS:-$HOME/.local/share/tisple/compilers}"
JOBS="${JOBS:-4}"

CC65_TAG=V2.19
CC65_COMMIT=555282497c3ecf8b313d87d5973093af19c35bd5
LLVM_MOS_TAG=v23.4.0
LLVM_MOS_URL="https://github.com/llvm-mos/llvm-mos-sdk/releases/download/$LLVM_MOS_TAG/llvm-mos-linux.tar.xz"
OSCAR64_TAG=v1.32.273
OSCAR64_COMMIT=9408778695b5442e755832711b89243b4f94a9ff

mkdir -p "$PREFIX"

clone_at() { # url tag commit dir
    if [ ! -d "$4/.git" ]; then
        git clone --quiet --depth 1 --branch "$2" "$1" "$4"
    fi
    got=$(git -C "$4" rev-parse HEAD)
    if [ "$got" != "$3" ]; then
        echo "install.sh: $4 is at $got, expected $3" >&2
        exit 1
    fi
}

# cc65: built in place; bin/cl65 finds its lib/ and include/ next to it
CC65_DIR="$PREFIX/cc65-$CC65_TAG"
if [ ! -x "$CC65_DIR/bin/cl65" ] || [ ! -f "$CC65_DIR/lib/sim6502.lib" ]; then
    clone_at https://github.com/cc65/cc65.git "$CC65_TAG" "$CC65_COMMIT" "$CC65_DIR"
    nice make -C "$CC65_DIR" -j "$JOBS" bin QUIET=1
    nice make -C "$CC65_DIR/libsrc" -j "$JOBS" sim6502 QUIET=1
fi
ln -sfn "cc65-$CC65_TAG" "$PREFIX/cc65"

# llvm-mos: the SDK tarball unpacks to llvm-mos/
LLVM_MOS_DIR="$PREFIX/llvm-mos-$LLVM_MOS_TAG"
if [ ! -x "$LLVM_MOS_DIR/bin/mos-sim-clang" ]; then
    tmp="$PREFIX/.llvm-mos-download"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    curl -fL --retry 3 -o "$tmp/llvm-mos-linux.tar.xz" "$LLVM_MOS_URL"
    tar -C "$tmp" -xJf "$tmp/llvm-mos-linux.tar.xz"
    rm -rf "$LLVM_MOS_DIR"
    mv "$tmp/llvm-mos" "$LLVM_MOS_DIR"
    rm -rf "$tmp"
fi
ln -sfn "llvm-mos-$LLVM_MOS_TAG" "$PREFIX/llvm-mos"

# oscar64: its own makefile builds bin/oscar64; include/ sits beside it
OSCAR64_DIR="$PREFIX/oscar64-$OSCAR64_TAG"
if [ ! -x "$OSCAR64_DIR/bin/oscar64" ]; then
    clone_at https://github.com/drmortalwombat/oscar64.git "$OSCAR64_TAG" "$OSCAR64_COMMIT" "$OSCAR64_DIR"
    nice make -C "$OSCAR64_DIR/make" -j "$JOBS" compiler
fi
ln -sfn "oscar64-$OSCAR64_TAG" "$PREFIX/oscar64"

"$PREFIX/cc65/bin/cc65" --version 2>&1 | head -1
"$PREFIX/llvm-mos/bin/mos-sim-clang" --version | head -1
"$PREFIX/oscar64/bin/oscar64" -h 2>&1 | head -1 || true
