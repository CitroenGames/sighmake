#!/usr/bin/env bash

# Stop on unset variables
set -u

# Change to the directory where the script resides
cd "$(dirname "$(realpath "$0")")" || exit 1

# Check if a macOS-compatible sighmake binary exists
SIGHMAKE="./sighmake_macos"
if [ -f "$SIGHMAKE" ] && file "$SIGHMAKE" | grep -q "Mach-O"; then
    echo "Using existing macOS binary."
else
    echo "No macOS binary found, compiling from source..."
    clang++ -std=c++17 -O2 -Wall -Isrc/ -include src/pch.h -o sighmake_macos \
        src/main.cpp src/pch.cpp src/pugixml.cpp \
        src/common/toolset_registry.cpp src/common/vs_detector.cpp src/common/build_cache.cpp \
        src/generators/vcxproj_generator.cpp src/generators/makefile_generator.cpp \
        src/generators/deps_exporter.cpp \
        src/parsers/buildscript_parser.cpp src/parsers/cmake_parser.cpp \
        src/parsers/vcxproj_reader.cpp src/parsers/vpc_parser.cpp
    if [ $? -ne 0 ]; then
        echo
        echo "Compilation failed."
        exit 1
    fi
    echo "Compilation successful."
fi

# Run sighmake
./sighmake_macos "sighmake.buildscript"
STATUS=$?

if [ $STATUS -ne 0 ]; then
    echo
    echo "Generation failed with an error."
    read -rp "Press Enter to continue..."
else
    echo
    echo "Makefiles generated successfully."
fi
