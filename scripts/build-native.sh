#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-"$root/build-native"}
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DSPARK_CORE_ONLY=OFF -DSPARK_RUNTIME_TESTS_ONLY=OFF \
    -DSPARK_PLATFORM_TESTS_ONLY=OFF -DSPARK_BUILD_TESTS=ON
# Imported source archives can retain timestamps older than existing object files.
cmake --build "$build" --clean-first --parallel 1
ctest --test-dir "$build" --output-on-failure --parallel 1
