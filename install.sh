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
    CORES="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    AVAIL_MB="$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1048576 ))"
else
    PLATFORM="Linux"
    CXX="${CXX:-g++}"
    CORES="$(nproc 2>/dev/null || echo 4)"
    AVAIL_MB="$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)"
fi

# Pick the parallel job count. Cap by available RAM (~450 MB per compile) so we
# don't thrash swap hard on low-memory machines, but always allow at least 2 jobs
# when more than one core is present. Set JOBS=N to override.
if [ -n "${JOBS:-}" ]; then
    :  # honour an explicit override
elif [ "$AVAIL_MB" -gt 0 ]; then
    MEM_JOBS=$(( AVAIL_MB / 450 ))
    [ "$MEM_JOBS" -lt 2 ] && MEM_JOBS=2          # floor at 2; swap covers brief overshoot
    JOBS=$(( CORES < MEM_JOBS ? CORES : MEM_JOBS ))
else
    JOBS="$CORES"
fi

# Release-mode compile flags
CXXFLAGS="${CXXFLAGS:--std=c++17 -O2 -DNDEBUG -Wall -Isrc/}"

echo "Platform:        $PLATFORM"
echo "Compiler:        $CXX"
echo "Parallel jobs:   $JOBS"
echo "Install prefix:  $PREFIX"
echo

# Step 1: Compile (release build, parallel)
echo "[1/2] Compiling (release)..."

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sighmake-build.XXXXXX")"
trap 'rm -rf "$BUILD_DIR" src/pch.h.gch' EXIT

# Precompile the shared header once so every translation unit reuses it
$CXX $CXXFLAGS -x c++-header src/pch.h -o src/pch.h.gch

# Compile each source to an object file in parallel across all cores.
# Stop the whole build if any single compile fails.
export BUILD_DIR CXX CXXFLAGS
find src -name '*.cpp' -print0 | \
    xargs -0 -P "$JOBS" -I '{}' sh -c \
    'out="$BUILD_DIR/$(echo "$1" | tr "/" "_").o"; exec $CXX $CXXFLAGS -include src/pch.h -c "$1" -o "$out"' _ '{}' || {
        echo "      compile failed" >&2
        exit 1
    }

# Link, then strip symbols for a smaller release binary
$CXX $CXXFLAGS -o sighmake "$BUILD_DIR"/*.o
strip sighmake 2>/dev/null || true
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
