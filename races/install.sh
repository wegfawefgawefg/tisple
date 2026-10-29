#!/bin/sh
# Installs the three race competitors under ~/.local/share/tisple/compilers (no root), on
# Linux x86-64 or macOS (Apple silicon or Intel). Versions are pinned; races/README.md
# records them. Rerunning skips what is there.
#   cc65     V2.19 built from source (the system C compiler: gcc, or Apple clang)
#   llvm-mos llvm-mos-sdk v23.4.0, the prebuilt release for this system, checksum checked
#   oscar64  v1.32.273 built from source
# Needs git, make, a C and C++ compiler, curl, tar with xz.
set -eu

PREFIX="${TISPLE_COMPILERS:-$HOME/.local/share/tisple/compilers}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

CC65_TAG=V2.19
CC65_COMMIT=555282497c3ecf8b313d87d5973093af19c35bd5
LLVM_MOS_TAG=v23.4.0
OSCAR64_TAG=v1.32.273
OSCAR64_COMMIT=9408778695b5442e755832711b89243b4f94a9ff

case "$(uname -s)" in
Linux)
    LLVM_MOS_ASSET=llvm-mos-linux.tar.xz
    LLVM_MOS_SHA256=0b1dbc8a2b15db88a1a400a687ea24cf1a38548c5ba9fdd61a473e4580ba00c1
    HOST_CC=gcc
    ;;
Darwin)
    # a universal binary: arm64 and x86-64
    LLVM_MOS_ASSET=llvm-mos-macos.tar.xz
    LLVM_MOS_SHA256=089afeff92ad4a5321bea0118b678afa6c725206d32bd8e9691bbe65880f8b7c
    HOST_CC=cc
    ;;
*)
    echo "install.sh: no llvm-mos release for $(uname -s)" >&2
    exit 1
    ;;
esac
LLVM_MOS_URL="https://github.com/llvm-mos/llvm-mos-sdk/releases/download/$LLVM_MOS_TAG/$LLVM_MOS_ASSET"

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

sha256() { # file
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# cc65: built in place; bin/cl65 finds its lib/ and include/ next to it
CC65_DIR="$PREFIX/cc65-$CC65_TAG"
if [ ! -x "$CC65_DIR/bin/cl65" ] || [ ! -f "$CC65_DIR/lib/sim6502.lib" ]; then
    clone_at https://github.com/cc65/cc65.git "$CC65_TAG" "$CC65_COMMIT" "$CC65_DIR"
    nice make -C "$CC65_DIR" -j "$JOBS" bin QUIET=1 CC="$HOST_CC"
    nice make -C "$CC65_DIR/libsrc" -j "$JOBS" sim6502 QUIET=1
fi
ln -sfn "cc65-$CC65_TAG" "$PREFIX/cc65"

# llvm-mos: the SDK tarball unpacks to llvm-mos/
LLVM_MOS_DIR="$PREFIX/llvm-mos-$LLVM_MOS_TAG"
if [ ! -x "$LLVM_MOS_DIR/bin/mos-sim-clang" ]; then
    tmp="$PREFIX/.llvm-mos-download"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    curl -fL --retry 3 -o "$tmp/$LLVM_MOS_ASSET" "$LLVM_MOS_URL"
    got=$(sha256 "$tmp/$LLVM_MOS_ASSET")
    if [ "$got" != "$LLVM_MOS_SHA256" ]; then
        echo "install.sh: $LLVM_MOS_ASSET has sha256 $got, expected $LLVM_MOS_SHA256" >&2
        exit 1
    fi
    tar -C "$tmp" -xJf "$tmp/$LLVM_MOS_ASSET"
    rm -rf "$LLVM_MOS_DIR"
    mv "$tmp/llvm-mos" "$LLVM_MOS_DIR"
    rm -rf "$tmp"
fi
ln -sfn "llvm-mos-$LLVM_MOS_TAG" "$PREFIX/llvm-mos"

# oscar64: its own makefile builds bin/oscar64 (it knows Darwin); include/ sits beside it
OSCAR64_DIR="$PREFIX/oscar64-$OSCAR64_TAG"
if [ ! -x "$OSCAR64_DIR/bin/oscar64" ]; then
    clone_at https://github.com/drmortalwombat/oscar64.git "$OSCAR64_TAG" "$OSCAR64_COMMIT" "$OSCAR64_DIR"
    nice make -C "$OSCAR64_DIR/make" -j "$JOBS" compiler
fi
ln -sfn "oscar64-$OSCAR64_TAG" "$PREFIX/oscar64"

"$PREFIX/cc65/bin/cc65" --version 2>&1 | head -1
"$PREFIX/llvm-mos/bin/mos-sim-clang" --version | head -1
"$PREFIX/oscar64/bin/oscar64" -h 2>&1 | head -1 || true
