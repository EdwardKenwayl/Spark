#!/bin/sh
set -eu

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${SPARK_PLATFORM_BUILD_DIR:-"$source_root/build-platform"}

# Foundation only: this does not build or package a native GUI.
cmake -S "$source_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSPARK_BUILD_TESTS=ON \
    -DSPARK_PLATFORM_TESTS_ONLY=ON
cmake --build "$build_dir" --parallel 1
ctest --test-dir "$build_dir" --output-on-failure
