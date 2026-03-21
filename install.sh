#!/usr/bin/env bash
set -eu

PREFIX="${PREFIX:-/usr/local}"

cd "$(dirname "$(realpath "$0")")"

echo "=== sighmake installer ==="
echo

# Detect platform and set compiler
OS="$(uname)"
if [ "$OS" = "Darwin" ]; then
    PLATFORM="macOS"
    CXX="${CXX:-clang++}"
else
    PLATFORM="Linux"
    CXX="${CXX:-g++}"
fi

echo "Platform:        $PLATFORM"
echo "Compiler:        $CXX"
echo "Install prefix:  $PREFIX"
echo

# Step 1: Bootstrap — compile sighmake from source
echo "[1/4] Compiling sighmake from source..."
SOURCES=$(find src -name '*.cpp' | tr '\n' ' ')
SIGHMAKE="./sighmake_bootstrap"
$CXX -std=c++17 -O2 -Wall -Isrc/ -include src/pch.h -o "$SIGHMAKE" $SOURCES
echo "      Bootstrap compilation successful."
echo

# Step 2: Generate Makefiles
echo "[2/4] Generating Makefiles..."
$SIGHMAKE sighmake.buildscript
rm -f "$SIGHMAKE"
echo

# Step 3: Build Release
echo "[3/4] Building Release..."
make -C build Release
echo

# Step 4: Install
echo "[4/4] Installing to $PREFIX/bin..."
BUILT_BIN=$(find build/bin/Release -type f -perm +111 2>/dev/null | head -1)
if [ -z "$BUILT_BIN" ]; then
    echo "Error: no binary found in build/bin/Release"
    exit 1
fi

# Check if we can write to the install directory
NEED_SUDO=false
if [ -d "$PREFIX/bin" ]; then
    [ -w "$PREFIX/bin" ] || NEED_SUDO=true
elif [ -d "$PREFIX" ]; then
    [ -w "$PREFIX" ] || NEED_SUDO=true
else
    # Try to create it — if it fails, we need sudo
    mkdir -p "$PREFIX/bin" 2>/dev/null || NEED_SUDO=true
fi

if [ "$NEED_SUDO" = true ]; then
    echo "      Need elevated permissions for $PREFIX/bin"
    sudo install -d "$PREFIX/bin"
    sudo install -m 755 "$BUILT_BIN" "$PREFIX/bin/sighmake"
else
    install -d "$PREFIX/bin"
    install -m 755 "$BUILT_BIN" "$PREFIX/bin/sighmake"
fi

echo
echo "sighmake installed to $PREFIX/bin/sighmake"
echo "Run 'sighmake --help' to get started."
