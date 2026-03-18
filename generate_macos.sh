#!/usr/bin/env bash

# Stop on unset variables
set -u

# Change to the directory where the script resides
cd "$(dirname "$(realpath "$0")")" || exit 1

# Run sighmake
./sighmake "sighmake.buildscript"
STATUS=$?

if [ $STATUS -ne 0 ]; then
    echo
    echo "Generation failed with an error."
    read -rp "Press Enter to continue..."
else
    echo
    echo "Makefiles generated successfully."
fi
