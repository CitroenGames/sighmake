#!/usr/bin/env bash
set -eu

PREFIX="${PREFIX:-/usr/local}"

cd "$(dirname "$(realpath "$0")")"

echo "=== sighmake installer ==="
echo "Install prefix: $PREFIX"
echo

# Detect platform and set compiler
if [ "$(uname)" = "Darwin" ]; then
    CXX="clang++"
    SIGHMAKE="./sighmake_macos"
else
    CXX="${CXX:-g++}"
    SIGHMAKE="./sighmake"
fi

# Bootstrap: compile sighmake from source if no binary exists
if [ ! -f "$SIGHMAKE" ]; then
    echo "No sighmake binary found, compiling from source..."
    SOURCES=$(find src -name '*.cpp' | tr '\n' ' ')
    $CXX -std=c++17 -O2 -Wall -Isrc/ -include src/pch.h -o "$SIGHMAKE" $SOURCES
    echo "Bootstrap compilation successful."
    echo
fi

# Generate Makefiles
echo "Generating Makefiles..."
$SIGHMAKE sighmake.buildscript
echo

# Build Release
echo "Building Release..."
make -C build Release
echo

# Install - find the built binary and install as 'sighmake'
echo "Installing to $PREFIX/bin..."
BUILT_BIN=$(find build/bin/Release -type f -perm +111 | head -1)
if [ -z "$BUILT_BIN" ]; then
    echo "Error: no binary found in build/bin/Release"
    exit 1
fi
install -d "$PREFIX/bin"
install -m 755 "$BUILT_BIN" "$PREFIX/bin/sighmake"
echo
echo "sighmake installed to $PREFIX/bin/sighmake"
echo "Run 'sighmake --help' to get started."
