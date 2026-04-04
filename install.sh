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

# Step 1: Compile
echo "[1/2] Compiling..."
SOURCES=$(find src -name '*.cpp' | tr '\n' ' ')
$CXX -std=c++17 -O2 -Wall -Isrc/ -include src/pch.h -o sighmake $SOURCES
echo "      OK"
echo

# Step 2: Install
echo "[2/2] Installing to $PREFIX/bin..."

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
    sudo install -m 755 sighmake "$PREFIX/bin/sighmake"
else
    install -d "$PREFIX/bin"
    install -m 755 sighmake "$PREFIX/bin/sighmake"
fi

rm -f sighmake

echo
echo "sighmake installed to $PREFIX/bin/sighmake"
echo "Run 'sighmake --help' to get started."
