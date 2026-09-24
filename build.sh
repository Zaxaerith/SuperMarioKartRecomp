#!/usr/bin/env bash
set -e

echo "=== Super Mario Kart (SNESRecomp Native AOT) Builder ==="

mkdir -p build
cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
ninja smk_play

echo "Build successful! Executable: build/smk_play"
