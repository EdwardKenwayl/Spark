#!/bin/sh
set -eu
if [ "$(uname -s)" != Darwin ]; then
    printf '%s\n' 'Run this script on macOS with Apple Command Line Tools installed.' >&2
    exit 1
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source=$(CDPATH= cd -- "${1:?Supply the unpacked wxWidgets 3.2 source directory.}" && pwd)
prefix=${2:-"$root/build-macos-deps/installed"}
mkdir -p "$prefix"
prefix=$(CDPATH= cd -- "$prefix" && pwd)
build="$root/build-macos-deps/wx"
mkdir -p "$build"
cd "$build"
"$source/configure" --prefix="$prefix" --with-osx_cocoa --disable-shared \
    --with-macosx-version-min="${MACOSX_DEPLOYMENT_TARGET:-12.0}" \
    --with-libpng=builtin --with-libjpeg=builtin --with-zlib=sys --without-libtiff \
    --disable-debug --disable-tests --without-subdirs --disable-webview \
    --disable-mediactrl --disable-richtext --disable-html --disable-htmlhelp \
    --disable-xrc --disable-stc --disable-ribbon --disable-propgrid --disable-aui
make -j1
make install
licenses="$prefix/share/spark-dependencies"
mkdir -p "$licenses"
cp "$source/docs/licence.txt" "$licenses/THIRD_PARTY_WXWIDGETS_LICENSE.txt"
cp "$source/docs/preamble.txt" "$licenses/THIRD_PARTY_WXWIDGETS_PREAMBLE.txt"
cp "$source/docs/lgpl.txt" "$licenses/THIRD_PARTY_WXWIDGETS_LGPL.txt"
cp "$source/src/png/LICENSE" "$licenses/THIRD_PARTY_LIBPNG_LICENSE.txt"
cp "$source/src/jpeg/README" "$licenses/THIRD_PARTY_IJG_JPEG_README.txt"
printf '%s\n' "Use PATH=\"$prefix/bin:\$PATH\" to build and package Spark."
